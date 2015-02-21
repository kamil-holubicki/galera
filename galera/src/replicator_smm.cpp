//
// Copyright (C) 2010-2014 Codership Oy <info@codership.com>
//

#include "galera_common.hpp"
#include "replicator_smm.hpp"
#include "galera_exception.hpp"
#include "uuid.hpp"

#include "galera_info.hpp"

#include "gu_debug_sync.hpp"

#include <sstream>
#include <iostream>


static void
apply_trx_ws(void*                    recv_ctx,
             wsrep_apply_cb_t         apply_cb,
             wsrep_commit_cb_t        commit_cb,
             const galera::TrxHandleSlave& trx,
             const wsrep_trx_meta_t&  meta)
{
    using galera::TrxHandle;
    static const size_t max_apply_attempts(4);
    size_t attempts(1);

    do
    {
        try
        {
            if (trx.is_toi())
            {
                log_debug << "Executing TO isolated action: " << trx;
            }

            gu_trace(trx.apply(recv_ctx, apply_cb, meta));

            if (trx.is_toi())
            {
                log_debug << "Done executing TO isolated action: "
                         << trx.global_seqno();
            }
            break;
        }
        catch (galera::ApplyException& e)
        {
            if (trx.is_toi())
            {
                log_warn << "Ignoring error for TO isolated action: " << trx;
                break;
            }
            else
            {
                int const err(e.status());

                if (err > 0)
                {
                    uint32_t const flags
                        (TrxHandle::trx_flags_to_wsrep_flags(trx.flags()) |
                         WSREP_FLAG_ROLLBACK);
                    wsrep_bool_t unused(false);
                    int const rcode(commit_cb(recv_ctx, flags, &meta, &unused));

                    if (WSREP_CB_SUCCESS != rcode)
                    {
                        gu_throw_fatal << "Rollback failed. Trx: " << trx;
                    }

                    ++attempts;

                    if (attempts <= max_apply_attempts)
                    {
                        log_warn << e.what()
                                 << "\nRetrying " << attempts << "th time";
                    }
                }
                else
                {
                    GU_TRACE(e);
                    throw;
                }
            }
        }
    }
    while (attempts <= max_apply_attempts);

    if (gu_unlikely(attempts > max_apply_attempts))
    {
        std::ostringstream msg;

        msg << "Failed to apply trx " << trx.global_seqno() << " "
            << max_apply_attempts << " times";

        throw galera::ApplyException(msg.str(), WSREP_CB_FAILURE);
    }

    return;
}


std::ostream& galera::operator<<(std::ostream& os, ReplicatorSMM::State state)
{
    switch (state)
    {
    case ReplicatorSMM::S_DESTROYED: return (os << "DESTROYED");
    case ReplicatorSMM::S_CLOSED:    return (os << "CLOSED");
    case ReplicatorSMM::S_CLOSING:   return (os << "CLOSING");
    case ReplicatorSMM::S_CONNECTED: return (os << "CONNECTED");
    case ReplicatorSMM::S_JOINING:   return (os << "JOINING");
    case ReplicatorSMM::S_JOINED:    return (os << "JOINED");
    case ReplicatorSMM::S_SYNCED:    return (os << "SYNCED");
    case ReplicatorSMM::S_DONOR:     return (os << "DONOR");
    }

    gu_throw_fatal << "invalid state " << static_cast<int>(state);
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//                           Public
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

galera::ReplicatorSMM::ReplicatorSMM(const struct wsrep_init_args* args)
    :
    init_lib_           (reinterpret_cast<gu_log_cb_t>(args->logger_cb)),
    config_             (),
    init_config_        (config_, args->node_address, args->data_dir),
    parse_options_      (*this, config_, args->options),
    init_ssl_           (config_),
    str_proto_ver_      (-1),
    protocol_version_   (-1),
    proto_max_          (gu::from_string<int>(config_.get(Param::proto_max))),
    state_              (S_CLOSED),
    sst_state_          (SST_NONE),
    co_mode_            (CommitOrder::from_string(
                             config_.get(Param::commit_order))),
    state_file_         (config_.get(BASE_DIR)+'/'+GALERA_STATE_FILE),
    st_                 (state_file_),
    trx_params_         (config_.get(BASE_DIR), -1,
                         KeySet::version(config_.get(Param::key_format)),
                         gu::from_string<int>(config_.get(
                             Param::max_write_set_size))),
    uuid_               (WSREP_UUID_UNDEFINED),
    state_uuid_         (WSREP_UUID_UNDEFINED),
    state_uuid_str_     (),
    cc_seqno_           (WSREP_SEQNO_UNDEFINED),
    cc_lowest_trx_seqno_(WSREP_SEQNO_UNDEFINED),
    pause_seqno_        (WSREP_SEQNO_UNDEFINED),
    app_ctx_            (args->app_ctx),
    view_cb_            (args->view_handler_cb),
    apply_cb_           (args->apply_cb),
    commit_cb_          (args->commit_cb),
    unordered_cb_       (args->unordered_cb),
    sst_donate_cb_      (args->sst_donate_cb),
    synced_cb_          (args->synced_cb),
    sst_donor_          (),
    sst_uuid_           (WSREP_UUID_UNDEFINED),
    sst_seqno_          (WSREP_SEQNO_UNDEFINED),
    sst_mutex_          (),
    sst_cond_           (),
    sst_retry_sec_      (1),
    sst_received_       (false),
    gcache_             (config_, config_.get(BASE_DIR)),
    gcs_                (config_, gcache_, proto_max_, args->proto_ver,
                         args->node_name, args->node_incoming),
    service_thd_        (gcs_, gcache_),
    slave_pool_         (sizeof(TrxHandleSlave), 1024, "TrxHandleSlave"),
    as_                 (0),
    gcs_as_             (slave_pool_, gcs_, *this, gcache_),
    ist_receiver_       (config_, slave_pool_, gcache_, *this,
                         args->node_address),
    ist_senders_        (gcs_, gcache_),
    wsdb_               (),
    cert_               (config_, service_thd_),
    local_monitor_      (),
    apply_monitor_      (),
    commit_monitor_     (),
    causal_read_timeout_(config_.get(Param::causal_read_timeout)),
    receivers_          (),
    replicated_         (),
    replicated_bytes_   (),
    keys_count_         (),
    keys_bytes_         (),
    data_bytes_         (),
    unrd_bytes_         (),
    local_commits_      (),
    local_rollbacks_    (),
    local_cert_failures_(),
    local_replays_      (),
    causal_reads_       (),
    preordered_id_      (),
    incoming_list_      (""),
    incoming_mutex_     (),
    wsrep_stats_        ()
{
    // @todo add guards (and perhaps actions)
    state_.add_transition(Transition(S_CLOSED,  S_DESTROYED));
    state_.add_transition(Transition(S_CLOSED,  S_CONNECTED));
    state_.add_transition(Transition(S_CLOSING, S_CLOSED));

    state_.add_transition(Transition(S_CONNECTED, S_CLOSING));
    state_.add_transition(Transition(S_CONNECTED, S_CONNECTED));
    state_.add_transition(Transition(S_CONNECTED, S_JOINING));
    // the following is possible only when bootstrapping new cluster
    // (trivial wsrep_cluster_address)
    state_.add_transition(Transition(S_CONNECTED, S_JOINED));
    // the following are possible on PC remerge
    state_.add_transition(Transition(S_CONNECTED, S_DONOR));
    state_.add_transition(Transition(S_CONNECTED, S_SYNCED));

    state_.add_transition(Transition(S_JOINING, S_CLOSING));
    // the following is possible if one non-prim conf follows another
    state_.add_transition(Transition(S_JOINING, S_CONNECTED));
    state_.add_transition(Transition(S_JOINING, S_JOINED));

    state_.add_transition(Transition(S_JOINED, S_CLOSING));
    state_.add_transition(Transition(S_JOINED, S_CONNECTED));
    state_.add_transition(Transition(S_JOINED, S_SYNCED));

    state_.add_transition(Transition(S_SYNCED, S_CLOSING));
    state_.add_transition(Transition(S_SYNCED, S_CONNECTED));
    state_.add_transition(Transition(S_SYNCED, S_DONOR));

    state_.add_transition(Transition(S_DONOR, S_CLOSING));
    state_.add_transition(Transition(S_DONOR, S_CONNECTED));
    state_.add_transition(Transition(S_DONOR, S_JOINED));

    local_monitor_.set_initial_position(0);

    wsrep_uuid_t  uuid;
    wsrep_seqno_t seqno;

    st_.get (uuid, seqno);

    if (0 != args->state_id &&
        args->state_id->uuid != WSREP_UUID_UNDEFINED &&
        args->state_id->uuid == uuid                 &&
        seqno                == WSREP_SEQNO_UNDEFINED)
    {
        /* non-trivial recovery information provided on startup, and db is safe
         * so use recovered seqno value */
        seqno = args->state_id->seqno;
    }

    log_debug << "End state: " << uuid << ':' << seqno << " #################";

    update_state_uuid (uuid);

    cc_seqno_ = seqno; // is it needed here?
    apply_monitor_.set_initial_position(seqno);

    if (co_mode_ != CommitOrder::BYPASS)
        commit_monitor_.set_initial_position(seqno);

    // Initialize cert index position to zero in the case recovery information
    // was provided. This is required to avoid initializing cert position
    // above index rebuild trx seqnos.
    cert_.assign_initial_position(seqno < 0 ? seqno : 0, trx_proto_ver());

    build_stats_vars(wsrep_stats_);
}

galera::ReplicatorSMM::~ReplicatorSMM()
{
    log_info << "dtor state: " << state_();
    switch (state_())
    {
    case S_CONNECTED:
    case S_JOINING:
    case S_JOINED:
    case S_SYNCED:
    case S_DONOR:
        close();
    case S_CLOSING:
        // @todo wait that all users have left the building
    case S_CLOSED:
        ist_senders_.cancel();
        break;
    case S_DESTROYED:
        break;
    }
}


wsrep_status_t galera::ReplicatorSMM::connect(const std::string& cluster_name,
                                              const std::string& cluster_url,
                                              const std::string& state_donor,
                                              bool  const        bootstrap)
{
    sst_donor_ = state_donor;
    service_thd_.reset();

    ssize_t err;
    wsrep_status_t ret(WSREP_OK);
    wsrep_seqno_t const seqno(STATE_SEQNO());
    wsrep_uuid_t  const gcs_uuid(seqno < 0 ? WSREP_UUID_UNDEFINED :state_uuid_);

    log_info << "Setting initial position to " << gcs_uuid << ':' << seqno;

    if ((err = gcs_.set_initial_position(gcs_uuid, seqno)) != 0)
    {
        log_error << "gcs init failed:" << strerror(-err);
        ret = WSREP_NODE_FAIL;
    }

    gcache_.reset();

    if (ret == WSREP_OK &&
        (err = gcs_.connect(cluster_name, cluster_url, bootstrap)) != 0)
    {
        log_error << "gcs connect failed: " << strerror(-err);
        ret = WSREP_NODE_FAIL;
    }

    if (ret == WSREP_OK)
    {
        state_.shift_to(S_CONNECTED);
    }

    return ret;
}


wsrep_status_t galera::ReplicatorSMM::close()
{
    if (state_() != S_CLOSED)
    {
        gcs_.close();
    }

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::async_recv(void* recv_ctx)
{
    assert(recv_ctx != 0);

    if (state_() == S_CLOSED || state_() == S_CLOSING)
    {
        log_error <<"async recv cannot start, provider in closed/closing state";
        return WSREP_FATAL;
    }

    ++receivers_;
    as_ = &gcs_as_;

    bool exit_loop(false);
    wsrep_status_t retval(WSREP_OK);

    while (WSREP_OK == retval && state_() != S_CLOSING)
    {
        ssize_t rc;

        while (gu_unlikely((rc = as_->process(recv_ctx, exit_loop))
                           == -ECANCELED))
        {
            recv_IST(recv_ctx);
            // hack: prevent fast looping until ist controlling thread
            // resumes gcs prosessing
            usleep(10000);
        }

        if (gu_unlikely(rc <= 0))
        {
            retval = WSREP_CONN_FAIL;
        }
        else if (gu_unlikely(exit_loop == true))
        {
            assert(WSREP_OK == retval);

            if (receivers_.sub_and_fetch(1) > 0)
            {
                log_info << "Slave thread exiting on request.";
                break;
            }

            ++receivers_;
            log_warn << "Refusing exit for the last slave thread.";
        }
    }

    /* exiting loop already did proper checks */
    if (!exit_loop && receivers_.sub_and_fetch(1) == 0)
    {
        if (state_() != S_CLOSING)
        {
            if (retval == WSREP_OK)
            {
                log_warn << "Broken shutdown sequence, provider state: "
                         << state_() << ", retval: " << retval;
                assert (0);
            }
            else
            {
                // Generate zero view before exit to notify application
                wsrep_view_info_t* err_view(galera_view_info_create(0, false));
                void* fake_sst_req(0);
                size_t fake_sst_req_len(0);
                view_cb_(app_ctx_, recv_ctx, err_view, 0, 0,
                         &fake_sst_req, &fake_sst_req_len);
                free(err_view);
            }
            /* avoid abort in production */
            state_.shift_to(S_CLOSING);
        }
        state_.shift_to(S_CLOSED);
    }

    log_debug << "Slave thread exit. Return code: " << retval;

    return retval;
}


void galera::ReplicatorSMM::apply_trx(void* recv_ctx, TrxHandleSlave* trx)
{
    assert(trx != 0);
    assert(trx->global_seqno() > 0);
    assert(trx->is_certified() == true);
    assert(trx->global_seqno() > STATE_SEQNO());
    assert(trx->is_local() == false);

    ApplyOrder ao(*trx);
    CommitOrder co(*trx, co_mode_);

    gu_trace(apply_monitor_.enter(ao));
    trx->set_state(TrxHandle::S_APPLYING);

    wsrep_trx_meta_t meta = { { state_uuid_,      trx->global_seqno() },
                              { trx->source_id(), trx->trx_id()       },
                              trx->depends_seqno() };

    gu_trace(apply_trx_ws(recv_ctx, apply_cb_, commit_cb_, *trx, meta));
    /* at this point any exception in apply_trx_ws() is fatal, not
     * catching anything. */

    wsrep_bool_t exit_loop(false);

    if (gu_likely(co_mode_ != CommitOrder::BYPASS))
    {
        gu_trace(commit_monitor_.enter(co));
    }
    trx->set_state(TrxHandle::S_COMMITTING);

    uint32_t const flags(TrxHandle::trx_flags_to_wsrep_flags(trx->flags()));

    wsrep_cb_status_t const rcode(commit_cb_(recv_ctx, flags, &meta,&exit_loop));

    if (gu_unlikely (rcode > 0))
        gu_throw_fatal << (trx->flags() & TrxHandle::F_ROLLBACK ?
                           "Rollback" : "Commit")
                       << " failed. Trx: " << trx;

    if (gu_likely(co_mode_ != CommitOrder::BYPASS))
    {
        commit_monitor_.leave(co);
    }
    trx->set_state(TrxHandle::S_COMMITTED);

    if (trx->local_seqno() != -1)
    {
        // trx with local seqno -1 originates from IST (or other source not gcs)
        report_last_committed(cert_.set_trx_committed(trx));
    }

    /* For now need to keep it inside apply monitor to ensure all processing
     * ends by the time monitors are drained because of potential gcache
     * cleanup (and loss of the writeset buffer). Perhaps unordered monitor
     * is needed here. */
    trx->unordered(recv_ctx, unordered_cb_);

    apply_monitor_.leave(ao);

    trx->set_exit_loop(exit_loop);
}


wsrep_status_t galera::ReplicatorSMM::replicate(TrxHandleMaster* trx,
                                                wsrep_trx_meta_t* meta)
{
    assert(trx->locked());

    if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    assert(trx->state() == TrxHandle::S_EXECUTING ||
           trx->state() == TrxHandle::S_MUST_ABORT);

    wsrep_status_t retval(WSREP_TRX_FAIL);

    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
    must_abort:
        trx->set_state(TrxHandle::S_ABORTING);
        return retval;
    }

    WriteSetNG::GatherVector actv;

    gcs_action act;
    act.type = GCS_ACT_TORDERED;
#ifndef NDEBUG
    act.seqno_g = GCS_SEQNO_ILL;
#endif

    assert(trx->version() >= WS_NG_VERSION);
    act.buf  = NULL;
    act.size = trx->write_set_out().gather(trx->source_id(),
                                           trx->conn_id(),
                                           trx->trx_id(),
                                           actv);

    trx->set_state(TrxHandle::S_REPLICATING);

    ssize_t rcode(-1);

    do
    {
        assert(act.seqno_g == GCS_SEQNO_ILL);

        const ssize_t gcs_handle(gcs_.schedule());

        if (gu_unlikely(gcs_handle < 0))
        {
            log_debug << "gcs schedule " << strerror(-gcs_handle);
            trx->set_state(TrxHandle::S_MUST_ABORT);
            assert(WSREP_TRX_FAIL == retval);
            goto must_abort;
        }

        trx->set_gcs_handle(gcs_handle);

        assert(trx->version() >= WS_NG_VERSION);
        trx->finalize(last_committed());
        trx->unlock();
        assert (act.buf == NULL); // just a sanity check
        rcode = gcs_.replv(actv, act, true);

        GU_DBUG_SYNC_WAIT("after_replicate_sync")
        trx->lock();
    }
    while (rcode == -EAGAIN && trx->state() != TrxHandle::S_MUST_ABORT &&
           (usleep(1000), true));

    if (rcode < 0)
    {
        if (rcode != -EINTR)
        {
            log_debug << "gcs_repl() failed with " << strerror(-rcode)
                      << " for trx " << *trx;
        }

        assert(rcode != -EINTR || trx->state() == TrxHandle::S_MUST_ABORT);
        assert(act.seqno_l == GCS_SEQNO_ILL && act.seqno_g == GCS_SEQNO_ILL);
        assert(NULL == act.buf /*|| !trx->new_version()*/);

        if (trx->state() != TrxHandle::S_MUST_ABORT)
        {
            trx->set_state(TrxHandle::S_MUST_ABORT);
        }

        trx->set_gcs_handle(-1);
        goto must_abort;
    }

    assert(act.buf != NULL);
    assert(act.size == rcode);
    assert(act.seqno_l != GCS_SEQNO_ILL);
    assert(act.seqno_g != GCS_SEQNO_ILL);

    trx->set_gcs_handle(-1);

    TrxHandleSlave* const ts(trx->repld());

    gu_trace(ts->unserialize(static_cast<const gu::byte_t*>(act.buf),
                             act.size, 0));

    ts->update_stats(keys_count_, keys_bytes_, data_bytes_, unrd_bytes_);
    ts->set_received(act.buf, act.seqno_l, act.seqno_g);

    ++replicated_;
    replicated_bytes_ += rcode;

    assert(trx->version() >= WS_NG_VERSION);

    assert(trx->source_id() == ts->source_id());
    assert(trx->conn_id()   == ts->conn_id());
    assert(trx->trx_id()    == ts->trx_id());

    assert(ts->global_seqno() == act.seqno_g);
    assert(ts->last_seen_seqno() >= 0);

    assert(trx->repld() == ts);
    assert(trx->global_seqno() == ts->global_seqno());
    assert(trx->global_seqno() == act.seqno_g);
    assert(trx->last_seen_seqno() == ts->last_seen_seqno());

    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        assert(ts->state() == TrxHandle::S_MUST_ABORT);

        retval = cert_for_aborted(ts);

        if (retval != WSREP_BF_ABORT)
        {
            LocalOrder  lo(*ts);
            ApplyOrder  ao(*ts);
            CommitOrder co(*ts, co_mode_);
            local_monitor_.self_cancel(lo);
            apply_monitor_.self_cancel(ao);
            if (co_mode_ !=CommitOrder::BYPASS) commit_monitor_.self_cancel(co);

            assert(ts->state()  == TrxHandle::S_MUST_ABORT);
            assert(trx->state() == TrxHandle::S_MUST_ABORT);
            assert(WSREP_OK != retval);

            goto must_abort;
        }
        else
        {
            assert(ts->state() == TrxHandle::S_REPLICATING);
            trx->set_state(TrxHandle::S_MUST_CERT_AND_REPLAY);

            if (meta != 0)
            {
                meta->gtid.uuid  = state_uuid_;
                meta->gtid.seqno = ts->global_seqno();
                meta->depends_on = ts->depends_seqno();
            }
        }
    }
    else
    {
        assert(ts->state() == TrxHandle::S_REPLICATING);
        retval = WSREP_OK;
    }

    return retval;
}

void
galera::ReplicatorSMM::abort_trx(TrxHandleMaster* trx)
{
    assert(trx != 0);
    assert(trx->is_local() == true);
    assert(trx->locked());

    log_debug << "aborting trx " << *trx << " " << trx;

    switch (trx->state())
    {
    case TrxHandle::S_MUST_ABORT:
    case TrxHandle::S_ABORTING: // guess this is here because we can have a race
        return;
    case TrxHandle::S_EXECUTING:
        trx->set_state(TrxHandle::S_MUST_ABORT);
        break;
    case TrxHandle::S_REPLICATING:
    {
        TrxHandleSlave& tr(*trx->repld());

        switch (tr.state())
        {
        case TrxHandle::S_REPLICATING:
        {
            // trx is in gcs repl

            /* @note: it is important to place set_state() into beginning of
             * every case, because state must be changed AFTER switch() and
             * BEFORE entering monitors or taking any other action. */
            trx->set_state(TrxHandle::S_MUST_ABORT);

            int rc;
            if (trx->gcs_handle() > 0 &&
                ((rc = gcs_.interrupt(trx->gcs_handle()))) != 0)
            {
                log_debug << "gcs_interrupt(): handle "
                          << trx->gcs_handle()
                          << " trx id " << trx->trx_id()
                          << ": " << strerror(-rc);
            }
            break;
        }
        case TrxHandle::S_CERTIFYING:
        {
            // tr is waiting in local monitor

            trx->set_state(TrxHandle::S_MUST_ABORT);

            LocalOrder lo(tr);
            tr.unlock();
            local_monitor_.interrupt(lo);
            tr.lock();
            break;
        }
        case TrxHandle::S_APPLYING:
        {
            // trx is waiting in apply monitor

            trx->set_state(TrxHandle::S_MUST_ABORT);

            ApplyOrder ao(tr);
            tr.unlock();
            apply_monitor_.interrupt(ao);
            tr.lock();
            break;
        }
        default:
            log_fatal << "invalid state " << trx->state();
            abort();
        }
        break;
    }
    case TrxHandle::S_COMMITTING:
    {
        TrxHandleSlave& tr(*trx->repld());

        switch (tr.state())
        {
        case TrxHandle::S_COMMITTING:

            trx->set_state(TrxHandle::S_MUST_ABORT);

            if (co_mode_ != CommitOrder::BYPASS)
            {
                // trx waiting in commit monitor
                CommitOrder co(tr, co_mode_);
                tr.unlock();
                commit_monitor_.interrupt(co);
                tr.lock();
            }
            break;
        default:
            log_fatal << "invalid state " << tr.state();
            abort();
        }
        break;
    }
    default:
        log_fatal << "invalid state " << trx->state();
        abort();
    }
}


wsrep_status_t galera::ReplicatorSMM::pre_commit(TrxHandleMaster*  trx,
                                                 wsrep_trx_meta_t* meta)
{
    assert(trx->state() == TrxHandle::S_REPLICATING);

    TrxHandleSlave* const tr(trx->repld());
    assert(tr->state() == TrxHandle::S_REPLICATING);

    assert(tr->local_seqno()  > -1);
    assert(tr->global_seqno() > -1);
    assert(tr->last_seen_seqno() >= 0);

    if (meta != 0)
    {
        meta->gtid.uuid  = state_uuid_;
        meta->gtid.seqno = tr->global_seqno();
        meta->depends_on = tr->depends_seqno();
    }
    // State should not be checked here: If trx has been replicated,
    // it has to be certified and potentially applied. #528
    // if (state_() < S_JOINED) return WSREP_TRX_FAIL;

    wsrep_status_t retval(cert_and_catch(tr));

    if (gu_unlikely(retval != WSREP_OK))
    {
        switch(retval)
        {
        case WSREP_BF_ABORT:
            if (tr->state() == TrxHandle::S_CERTIFYING)
            {
                trx->set_state(TrxHandle::S_MUST_REPLAY_AM);
            }
            else
            {
                assert(tr->state() == TrxHandle::S_REPLICATING);
                trx->set_state(TrxHandle::S_MUST_CERT_AND_REPLAY);
            }
            break;
        case WSREP_TRX_FAIL:
            assert(tr->state() == TrxHandle::S_MUST_ABORT);
            trx->set_state(TrxHandle::S_MUST_ABORT);
            break;
        default:
            assert(0);
        }

        assert(trx->state() == TrxHandle::S_MUST_ABORT ||
               trx->state() == TrxHandle::S_MUST_REPLAY_AM ||
               trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY);

        if (tr->state() == TrxHandle::S_MUST_ABORT)
            trx->set_state(TrxHandle::S_ABORTING);

        return retval;
    }

    assert(trx->state() == TrxHandle::S_REPLICATING ||
           trx->state() == TrxHandle::S_MUST_ABORT); // from abort_pre_commit()
    assert(tr->state() == TrxHandle::S_CERTIFYING);
    assert(tr->global_seqno() > STATE_SEQNO());
    tr->set_state(TrxHandle::S_APPLYING);

    ApplyOrder  ao(*tr);
    CommitOrder co(*tr, co_mode_);
    bool interrupted(false);

    try
    {
        gu_trace(apply_monitor_.enter(ao));
    }
    catch (gu::Exception& e)
    {
        if (e.get_errno() == EINTR)
        {
            interrupted = true;
        }
        else throw;
    }

    if (gu_unlikely(interrupted || tr->state() == TrxHandle::S_MUST_ABORT))
    {
        if (interrupted) trx->set_state(TrxHandle::S_MUST_REPLAY_AM);
        else             trx->set_state(TrxHandle::S_MUST_REPLAY_CM);

        assert(tr->state() == TrxHandle::S_MUST_ABORT);
        // I wonder if we need to check for interrupt at all...

        tr->set_state(TrxHandle::S_MUST_REPLAY);
        retval = WSREP_BF_ABORT;
    }
    else
    {
        trx->set_state(TrxHandle::S_COMMITTING);
        tr->set_state(TrxHandle::S_COMMITTING);

        if (co_mode_ != CommitOrder::BYPASS)
        {
            try
            {
                gu_trace(commit_monitor_.enter(co));
            }
            catch (gu::Exception& e)
            {
                if (e.get_errno() == EINTR)
                {
                    if (trx->state() != TrxHandle::S_MUST_ABORT)
                        trx->set_state(TrxHandle::S_MUST_ABORT);
                    interrupted = true;
                }
                else throw;
            }

            if (gu_unlikely(interrupted ||
                            trx->state() == TrxHandle::S_MUST_ABORT))
            {
                if (interrupted) trx->set_state(TrxHandle::S_MUST_REPLAY_CM);
                else             trx->set_state(TrxHandle::S_MUST_REPLAY);

                // this seems totally wrong: why entering commit monitor
                // may change trx state? But OK...
                assert(tr->state() == TrxHandle::S_MUST_ABORT);
                tr->set_state(TrxHandle::S_MUST_REPLAY);

                retval = WSREP_BF_ABORT;
            }
        }
    }

    assert((retval == WSREP_OK && (tr->state() == TrxHandle::S_COMMITTING ||
                                   trx->state() == TrxHandle::S_EXECUTING))
           ||
           (retval == WSREP_BF_ABORT && (
               trx->state() == TrxHandle::S_MUST_REPLAY_AM ||
               trx->state() == TrxHandle::S_MUST_REPLAY_CM ||
               trx->state() == TrxHandle::S_MUST_REPLAY)));

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::replay_trx(TrxHandleMaster* trx,
                                                 void*            trx_ctx)
{
    assert(trx->state() == TrxHandle::S_MUST_CERT_AND_REPLAY ||
           trx->state() == TrxHandle::S_MUST_REPLAY_AM       ||
           trx->state() == TrxHandle::S_MUST_REPLAY_CM       ||
           trx->state() == TrxHandle::S_MUST_REPLAY);
    assert(trx->trx_id() != static_cast<wsrep_trx_id_t>(-1));

    TrxHandleSlave* const txs(trx->repld());

    assert(txs->global_seqno() > STATE_SEQNO());

    wsrep_status_t retval(WSREP_OK);

    switch (trx->state())
    {
    case TrxHandle::S_MUST_CERT_AND_REPLAY:
        retval = cert_and_catch(txs);
        if (retval != WSREP_OK)
        {
            trx->set_state(TrxHandle::S_MUST_ABORT);
            // apply monitor is self canceled in cert
            break;
        }
        trx->set_state(TrxHandle::S_MUST_REPLAY_AM);
        // fall through
    case TrxHandle::S_MUST_REPLAY_AM:
    {
        // safety measure to make sure that all preceding trxs finish before
        // replaying
        txs->set_depends_seqno(txs->global_seqno() - 1);
        ApplyOrder ao(*txs);
        gu_trace(apply_monitor_.enter(ao));
        trx->set_state(TrxHandle::S_MUST_REPLAY_CM);
        // fall through
    }
    case TrxHandle::S_MUST_REPLAY_CM:
        if (co_mode_ != CommitOrder::BYPASS)
        {
            CommitOrder co(*txs, co_mode_);
            gu_trace(commit_monitor_.enter(co));
        }
        trx->set_state(TrxHandle::S_MUST_REPLAY);
        // fall through
    case TrxHandle::S_MUST_REPLAY:
        ++local_replays_;

        trx->set_state(TrxHandle::S_REPLAYING);
        try
        {
            TrxHandleSlave* const tr(trx->repld());

            wsrep_trx_meta_t meta = {{ state_uuid_,     tr->global_seqno() },
                                     { tr->source_id(), tr->trx_id()       },
                                     tr->depends_seqno()};

            tr->set_state(TrxHandle::S_REPLAYING);

            gu_trace(apply_trx_ws(trx_ctx, apply_cb_, commit_cb_, *tr,meta));

            uint32_t const flags
                (TrxHandle::trx_flags_to_wsrep_flags(tr->flags()));
            wsrep_bool_t unused(false);

            wsrep_cb_status_t rcode(commit_cb_(trx_ctx,flags,&meta,&unused));

            if (gu_unlikely(rcode != WSREP_CB_SUCCESS))
                gu_throw_fatal << "Commit failed. Trx: " << *tr;

            /* fragment will be set sommitted in post_commit() */
        }
        catch (gu::Exception& e)
        {
            st_.mark_corrupt();
            throw;
        }

        // apply, commit monitors are released in post commit
        return WSREP_OK;
    default:
        assert(0);
        gu_throw_fatal << "Invalid state in replay for trx " << *trx;
    }

    log_debug << "replaying failed for trx " << *trx;
    trx->set_state(TrxHandle::S_ABORTING);

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::post_commit(TrxHandleMaster* trx)
{
    TrxHandleSlave* const txs(trx->repld());

    if (txs->state() == TrxHandle::S_MUST_ABORT)
    {
        assert(trx->state() == TrxHandle::S_MUST_ABORT);
        // This is possible in case of ALG: BF applier BF aborts
        // trx that has already grabbed commit monitor and is committing.
        // However, this should be acceptable assuming that commit
        // operation does not reserve any more resources and is able
        // to release already reserved resources.
        log_debug << "trx was BF aborted during commit: " << *txs;
        // manipulate state to avoid crash
        txs->set_state(TrxHandle::S_MUST_REPLAY);
        txs->set_state(TrxHandle::S_REPLAYING);
        trx->set_state(TrxHandle::S_MUST_REPLAY);
        trx->set_state(TrxHandle::S_REPLAYING);
    }

    assert(txs->state() == TrxHandle::S_COMMITTING ||
           txs->state() == TrxHandle::S_REPLAYING);
    assert(trx->state() == TrxHandle::S_COMMITTING ||
           trx->state() == TrxHandle::S_REPLAYING);
    assert(txs->local_seqno() > -1 && txs->global_seqno() > -1);

    CommitOrder co(*txs, co_mode_);
    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.leave(co);

    ApplyOrder ao(*txs);
    report_last_committed(cert_.set_trx_committed(txs));
    apply_monitor_.leave(ao);

    txs->set_state(TrxHandle::S_COMMITTED);

    if (gu_likely((txs->flags() & TrxHandle::F_COMMIT) != 0))
    {
        trx->set_state(TrxHandle::S_COMMITTED);
    }
    else
    {
        // continue streaming - and add a new fragment handle
        trx->set_state(TrxHandle::S_EXECUTING);
        TrxHandleSlave* const tr(TrxHandleSlave::New(slave_pool_));
        trx->add_replicated(tr);
    }

    ++local_commits_;

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::post_rollback(TrxHandleMaster* trx)
{
    if (trx->state() == TrxHandle::S_MUST_ABORT)
    {
        trx->set_state(TrxHandle::S_ABORTING);
    }

    assert(trx->state() == TrxHandle::S_ABORTING ||
           trx->state() == TrxHandle::S_EXECUTING);

    trx->set_state(TrxHandle::S_ROLLED_BACK);

    // Trx was either rolled back by user or via certification failure,
    // last committed report not needed since cert index state didn't change.
    // report_last_committed();
    ++local_rollbacks_;

    return WSREP_OK;
}


wsrep_status_t galera::ReplicatorSMM::causal_read(wsrep_gtid_t* gtid)
{
    wsrep_seqno_t cseq(static_cast<wsrep_seqno_t>(gcs_.caused()));

    if (cseq < 0)
    {
        log_warn << "gcs_caused() returned " << cseq << " (" << strerror(-cseq)
                 << ')';
        return WSREP_TRX_FAIL;
    }

    try
    {
        // @note: Using timed wait for monitor is currently a hack
        // to avoid deadlock resulting from race between monitor wait
        // and drain during configuration change. Instead of this,
        // monitor should have proper mechanism to interrupt waiters
        // at monitor drain and disallowing further waits until
        // configuration change related operations (SST etc) have been
        // finished.
        gu::datetime::Date wait_until(gu::datetime::Date::calendar()
                                      + causal_read_timeout_);
        if (gu_likely(co_mode_ != CommitOrder::BYPASS))
        {
            commit_monitor_.wait(cseq, wait_until);
        }
        else
        {
            apply_monitor_.wait(cseq, wait_until);
        }
        if (gtid != 0)
        {
            gtid->uuid = state_uuid_;
            gtid->seqno = cseq;
        }
        ++causal_reads_;
        return WSREP_OK;
    }
    catch (gu::Exception& e)
    {
        log_debug << "monitor wait failed for causal read: " << e.what();
        return WSREP_TRX_FAIL;
    }
}


wsrep_status_t galera::ReplicatorSMM::to_isolation_begin(TrxHandleMaster*  trx,
                                                         wsrep_trx_meta_t* meta)
{
    TrxHandleSlave* const txs(trx->repld());

    if (meta != 0)
    {
        meta->gtid.uuid  = state_uuid_;
        meta->gtid.seqno = txs->global_seqno();
        meta->depends_on = txs->depends_seqno();
    }

    assert(trx->state() == TrxHandle::S_REPLICATING);
    assert(trx->trx_id() == static_cast<wsrep_trx_id_t>(-1));
    assert(txs->local_seqno() > -1 && txs->global_seqno() > -1);
    assert(txs->global_seqno() > STATE_SEQNO());

    wsrep_status_t retval;
    switch ((retval = cert_and_catch(txs)))
    {
    case WSREP_OK:
    {
        ApplyOrder ao(*txs);
        CommitOrder co(*txs, co_mode_);

        gu_trace(apply_monitor_.enter(ao));

        if (co_mode_ != CommitOrder::BYPASS)
            try
            {
                commit_monitor_.enter(co);
            }
            catch (...)
            {
                gu_throw_fatal << "unable to enter commit monitor: " << *trx;
            }

        txs->set_state(TrxHandle::S_APPLYING);
        log_debug << "Executing TO isolated action: " << *txs;
        st_.mark_unsafe();
        break;
    }
    case WSREP_TRX_FAIL:
        // Apply monitor is released in cert() in case of failure.
        trx->set_state(TrxHandle::S_MUST_ABORT);
        trx->set_state(TrxHandle::S_ABORTING);
        break;
    default:
        log_error << "unrecognized retval "
                  << retval
                  << " for to isolation certification for "
                  << *txs;
        retval = WSREP_FATAL;
        break;
    }

    return retval;
}


wsrep_status_t galera::ReplicatorSMM::to_isolation_end(TrxHandleMaster* trx)
{
    assert(trx->state() == TrxHandle::S_REPLICATING);

    TrxHandleSlave* const txs(trx->repld());
    assert(txs->state() == TrxHandle::S_APPLYING);

    log_debug << "Done executing TO isolated action: " << *txs;

    CommitOrder co(*txs, co_mode_);
    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.leave(co);
    ApplyOrder ao(*txs);
    report_last_committed(cert_.set_trx_committed(txs));
    apply_monitor_.leave(ao);

    st_.mark_safe();

    return WSREP_OK;
}

namespace galera
{

static WriteSetOut*
writeset_from_handle (wsrep_po_handle_t&             handle,
                      const TrxHandleMaster::Params& trx_params)
{
    WriteSetOut* ret = reinterpret_cast<WriteSetOut*>(handle.opaque);

    if (NULL == ret)
    {
        try
        {
            ret = new WriteSetOut(
//                gu::String<256>(trx_params.working_dir_) << '/' << &handle,
                trx_params.working_dir_, wsrep_trx_id_t(&handle),
                /* key format is not essential since we're not adding keys */
                KeySet::version(trx_params.key_format_), NULL, 0, 0,
                WriteSetNG::MAX_VERSION, DataSet::MAX_VERSION, DataSet::MAX_VERSION,
                trx_params.max_write_set_size_);

            handle.opaque = ret;
        }
        catch (std::bad_alloc& ba)
        {
            gu_throw_error(ENOMEM) << "Could not create WriteSetOut";
        }
    }

    return ret;
}

} /* namespace galera */

wsrep_status_t
galera::ReplicatorSMM::preordered_collect(wsrep_po_handle_t&            handle,
                                          const struct wsrep_buf* const data,
                                          size_t                  const count,
                                          bool                    const copy)
{
    if (gu_unlikely(trx_params_.version_ < WS_NG_VERSION))
        return WSREP_NOT_IMPLEMENTED;

    WriteSetOut* const ws(writeset_from_handle(handle, trx_params_));

    for (size_t i(0); i < count; ++i)
    {
        ws->append_data(data[i].ptr, data[i].len, copy);
    }

    return WSREP_OK;
}


wsrep_status_t
galera::ReplicatorSMM::preordered_commit(wsrep_po_handle_t&         handle,
                                         const wsrep_uuid_t&        source,
                                         uint64_t             const flags,
                                         int                  const pa_range,
                                         bool                 const commit)
{
    if (gu_unlikely(trx_params_.version_ < WS_NG_VERSION))
        return WSREP_NOT_IMPLEMENTED;

    WriteSetOut* const ws(writeset_from_handle(handle, trx_params_));

    if (gu_likely(true == commit))
    {
        ws->set_flags (WriteSetNG::wsrep_flags_to_ws_flags(flags) |
                       WriteSetNG::F_CERTIFIED);

        /* by loooking at trx_id we should be able to detect gaps / lost events
         * (however resending is not implemented yet). Something like
         *
         * wsrep_trx_id_t const trx_id(cert_.append_preordered(source, ws));
         *
         * begs to be here. */
        wsrep_trx_id_t const trx_id(preordered_id_.add_and_fetch(1));

        WriteSetNG::GatherVector actv;

        size_t const actv_size(ws->gather(source, 0, trx_id, actv));

        ws->finalize_preordered(pa_range); // also adds checksum

        int rcode;
        do
        {
            rcode = gcs_.sendv(actv, actv_size, GCS_ACT_TORDERED, false);
        }
        while (rcode == -EAGAIN && (usleep(1000), true));

        if (rcode < 0)
            gu_throw_error(-rcode)
                << "Replication of preordered writeset failed.";
    }

    delete ws;
    handle.opaque = NULL;

    return WSREP_OK;
}


wsrep_status_t
galera::ReplicatorSMM::sst_sent(const wsrep_gtid_t& state_id, int const rcode)
{
    assert (rcode <= 0);
    assert (rcode == 0 || state_id.seqno == WSREP_SEQNO_UNDEFINED);
    assert (rcode != 0 || state_id.seqno >= 0);

    if (state_() != S_DONOR)
    {
        log_error << "sst sent called when not SST donor, state " << state_();
        return WSREP_CONN_FAIL;
    }

    gcs_seqno_t seqno(rcode ? rcode : state_id.seqno);

    if (state_id.uuid != state_uuid_ && seqno >= 0)
    {
        // state we have sent no longer corresponds to the current group state
        // mark an error
        seqno = -EREMCHG;
    }

    try {
        gcs_.join(seqno);
        return WSREP_OK;
    }
    catch (gu::Exception& e)
    {
        log_error << "failed to recover from DONOR state: " << e.what();
        return WSREP_CONN_FAIL;
    }
}


void galera::ReplicatorSMM::process_trx(void* recv_ctx, TrxHandleSlave* trx)
{
    assert(recv_ctx != 0);
    assert(trx != 0);
    assert(trx->local_seqno() > 0);
    assert(trx->global_seqno() > 0);
    assert(trx->last_seen_seqno() >= 0);
    assert(trx->depends_seqno() == -1 || trx->version() >= 4);
    assert(trx->state() == TrxHandle::S_REPLICATING);

    wsrep_status_t const retval(cert_and_catch(trx));

    switch (retval)
    {
    case WSREP_OK:
        try
        {
            gu_trace(apply_trx(recv_ctx, trx));
        }
        catch (std::exception& e)
        {
            st_.mark_corrupt();

            log_fatal << "Failed to apply trx: " << *trx;
            log_fatal << e.what();
            log_fatal << "Node consistency compromized, aborting...";
            abort();
        }
        break;
    case WSREP_TRX_FAIL:
        // certification failed, apply monitor has been canceled
        trx->set_state(TrxHandle::S_ROLLED_BACK);
        break;
    default:
        // this should not happen for remote actions
        gu_throw_error(EINVAL)
            << "unrecognized retval for remote trx certification: "
            << retval << " trx: " << *trx;
    }
}


void galera::ReplicatorSMM::process_commit_cut(wsrep_seqno_t seq,
                                               wsrep_seqno_t seqno_l)
{
    assert(seq > 0);
    assert(seqno_l > 0);
    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));

    if (seq >= cc_seqno_) /* Refs #782. workaround for
                           * assert(seqno >= seqno_released_) in gcache. */
        cert_.purge_trxs_upto(seq, true);

    local_monitor_.leave(lo);
    log_debug << "Got commit cut from GCS: " << seq;
}

void galera::ReplicatorSMM::establish_protocol_versions (int proto_ver)
{
    switch (proto_ver)
    {
    case 1:
        trx_params_.version_ = 1;
        str_proto_ver_ = 0;
        break;
    case 2:
        trx_params_.version_ = 1;
        str_proto_ver_ = 1;
        break;
    case 3:
    case 4:
        trx_params_.version_ = 2;
        str_proto_ver_ = 1;
        break;
    case 5:
        trx_params_.version_ = 3;
        str_proto_ver_ = 1;
        break;
    case 6:
        trx_params_.version_  = 3;
        str_proto_ver_ = 2; // gcs intelligent donor selection.
        // include handling dangling comma in donor string.
        break;
    case 7:
        // Protocol upgrade to handle IST SSL backwards compatibility,
        // no effect to TRX or STR protocols.
        trx_params_.version_ = 3;
        str_proto_ver_ = 2;
        break;
    case 8:
        trx_params_.version_ = 4;
        str_proto_ver_ = 2;
        break;
    default:
        log_fatal << "Configuration change resulted in an unsupported protocol "
            "version: " << proto_ver << ". Can't continue.";
        abort();
    };

    protocol_version_ = proto_ver;
    log_info << "REPL Protocols: " << protocol_version_ << " ("
              << trx_params_.version_ << ", " << str_proto_ver_ << ")";
}

static bool
app_wants_state_transfer (const void* const req, ssize_t const req_len)
{
    return (req_len != (strlen(WSREP_STATE_TRANSFER_NONE) + 1) ||
            memcmp(req, WSREP_STATE_TRANSFER_NONE, req_len));
}

void
galera::ReplicatorSMM::update_incoming_list(const wsrep_view_info_t& view)
{
    static char const separator(',');

    ssize_t new_size(0);

    if (view.memb_num > 0)
    {
        new_size += view.memb_num - 1; // separators

        for (int i = 0; i < view.memb_num; ++i)
        {
            new_size += strlen(view.members[i].incoming);
        }
    }

    gu::Lock lock(incoming_mutex_);

    incoming_list_.clear();
    incoming_list_.resize(new_size);

    if (new_size <= 0) return;

    incoming_list_ = view.members[0].incoming;

    for (int i = 1; i < view.memb_num; ++i)
    {
        incoming_list_ += separator;
        incoming_list_ += view.members[i].incoming;
    }
}

void
galera::ReplicatorSMM::process_conf_change(void*                    recv_ctx,
                                           const wsrep_view_info_t& view_info,
                                           int                      repl_proto,
                                           State                    next_state,
                                           wsrep_seqno_t            seqno_l)
{
    assert(seqno_l > -1);

    update_incoming_list(view_info);

    LocalOrder lo(seqno_l);
    gu_trace(local_monitor_.enter(lo));

    wsrep_seqno_t const upto(cert_.position());

    apply_monitor_.drain(upto);

    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(upto);

    if (view_info.my_idx >= 0)
    {
        uuid_ = view_info.members[view_info.my_idx].id;
    }

    bool const          st_required(state_transfer_required(view_info));
    wsrep_seqno_t const group_seqno(view_info.state_id.seqno);
    const wsrep_uuid_t& group_uuid (view_info.state_id.uuid);

    if (st_required)
    {
        log_info << "State transfer required: "
                 << "\n\tGroup state: " << group_uuid << ":" << group_seqno
                 << "\n\tLocal state: " << state_uuid_<< ":" << STATE_SEQNO();

        if (S_CONNECTED != state_()) state_.shift_to(S_CONNECTED);
    }

    void*  app_req(0);
    size_t app_req_len(0);

    const_cast<wsrep_view_info_t&>(view_info).state_gap = st_required;
    wsrep_cb_status_t const rcode(
        view_cb_(app_ctx_, recv_ctx, &view_info, 0, 0, &app_req, &app_req_len));

    if (WSREP_CB_SUCCESS != rcode)
    {
        assert(app_req_len <= 0);
        log_fatal << "View callback failed. This is unrecoverable, "
                  << "restart required.";
        close();
        abort();
    }
    else if (st_required && 0 == app_req_len && state_uuid_ != group_uuid)
    {
        log_fatal << "Local state UUID " << state_uuid_
                  << " is different from group state UUID " << group_uuid
                  << ", and SST request is null: restart required.";
        close();
        abort();
    }

    if (view_info.view >= 0) // Primary configuration
    {
        int prev_protocol_version(protocol_version_);
        establish_protocol_versions (repl_proto);

        bool const app_wants_st(app_wants_state_transfer(app_req, app_req_len));

        //
        // Starting from protocol_version_ 8 joiner's cert index  is rebuilt
        // from IST.
        //
        // The reasons to reset cert index:
        // - Protocol version lower than 8    (ALL)
        // - Protocol upgrade                 (ALL)
        // - State transfer will take a place (JOINER)
        //
        if (protocol_version_ < 8 ||
            prev_protocol_version != protocol_version_ ||
            (st_required && app_wants_st))
        {
            log_info << "Cert index reset " << protocol_version_
                     << " " << app_wants_st;
            cert_.assign_initial_position(
                protocol_version_ < 8 ? STATE_SEQNO() : 0,
                trx_params_.version_);
            service_thd_.flush();             // make sure service thd is idle

            if (STATE_SEQNO() > 0) gcache_.seqno_release(STATE_SEQNO());
            // make sure all gcache buffers are released
        }
        else
        {
            log_info << "Skipping cert index reset";
        }
        // at this point there is no ongoing master or slave transactions
        // and no new requests to service thread should be possible

        // record state seqno, needed for IST on DONOR
        cc_seqno_ = group_seqno;
        // Record lowest trx seqno in cert index to set cert index
        // rebuild flag appropriately in IST. Notice that if cert index
        // was completely reset above, the value returned is zero and
        // no rebuild should happen.
        cc_lowest_trx_seqno_ = cert_.lowest_trx_seqno();

        if (st_required && app_wants_st)
        {
            // GCache::Seqno_reset() happens here
            request_state_transfer (recv_ctx,
                                    group_uuid, group_seqno, app_req,
                                    app_req_len);
        }
        else
        {
            if (view_info.view == 1 || !app_wants_st)
            {
                update_state_uuid (group_uuid);
                apply_monitor_.set_initial_position(group_seqno);
                if (co_mode_ != CommitOrder::BYPASS)
                    commit_monitor_.set_initial_position(group_seqno);
            }

            if (state_() == S_CONNECTED || state_() == S_DONOR)
            {
                switch (next_state)
                {
                case S_JOINING:
                    state_.shift_to(S_JOINING);
                    break;
                case S_DONOR:
                    if (state_() == S_CONNECTED)
                    {
                        state_.shift_to(S_DONOR);
                    }
                    break;
                case S_JOINED:
                    state_.shift_to(S_JOINED);
                    break;
                case S_SYNCED:
                    state_.shift_to(S_SYNCED);
                    synced_cb_(app_ctx_);
                    break;
                default:
                    log_debug << "next_state " << next_state;
                    break;
                }
            }

            st_.set(state_uuid_, WSREP_SEQNO_UNDEFINED);
        }

        if (state_() == S_JOINING && sst_state_ != SST_NONE)
        {
            /* There are two reasons we can be here:
             * 1) we just got state transfer in request_state_transfer() above;
             * 2) we failed here previously (probably due to partition).
             */
            try {
                gcs_.join(sst_seqno_);
                sst_state_ = SST_NONE;
            }
            catch (gu::Exception& e)
            {
                log_error << "Failed to JOIN the cluster after SST";
            }
        }
    }
    else
    {
        // Non-primary configuration
        if (state_uuid_ != WSREP_UUID_UNDEFINED)
        {
            st_.set (state_uuid_, STATE_SEQNO());
        }

        if (next_state != S_CONNECTED && next_state != S_CLOSING)
        {
            log_fatal << "Internal error: unexpected next state for "
                      << "non-prim: " << next_state << ". Restart required.";
            close();
            abort();
        }

        state_.shift_to(next_state);
    }

    double foo, bar;
    size_t index_size;
    cert_.stats_get(foo, bar, index_size);
    local_monitor_.leave(lo);
    gcs_.resume_recv();
    free(app_req);
}


void galera::ReplicatorSMM::process_join(wsrep_seqno_t seqno_j,
                                         wsrep_seqno_t seqno_l)
{
    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));

    wsrep_seqno_t const upto(cert_.position());

    apply_monitor_.drain(upto);

    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(upto);

    if (seqno_j < 0 && S_JOINING == state_())
    {
        // #595, @todo: find a way to re-request state transfer
        log_fatal << "Failed to receive state transfer: " << seqno_j
                  << " (" << strerror (-seqno_j) << "), need to restart.";
        abort();
    }
    else
    {
        state_.shift_to(S_JOINED);
    }

    local_monitor_.leave(lo);
}


void galera::ReplicatorSMM::process_sync(wsrep_seqno_t seqno_l)
{
    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));

    wsrep_seqno_t const upto(cert_.position());

    apply_monitor_.drain(upto);

    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(upto);

    state_.shift_to(S_SYNCED);
    synced_cb_(app_ctx_);
    local_monitor_.leave(lo);
}

wsrep_seqno_t galera::ReplicatorSMM::pause()
{
    // Grab local seqno for local_monitor_
    wsrep_seqno_t const local_seqno(
        static_cast<wsrep_seqno_t>(gcs_.local_sequence()));
    LocalOrder lo(local_seqno);
    local_monitor_.enter(lo);

    // Local monitor should take care that concurrent
    // pause requests are enqueued
    assert(pause_seqno_ == WSREP_SEQNO_UNDEFINED);
    pause_seqno_ = local_seqno;

    // Get drain seqno from cert index
    wsrep_seqno_t const upto(cert_.position());
    apply_monitor_.drain(upto);
    assert (apply_monitor_.last_left() >= upto);

    if (co_mode_ != CommitOrder::BYPASS)
    {
        commit_monitor_.drain(upto);
        assert (commit_monitor_.last_left() >= upto);
        assert (commit_monitor_.last_left() == apply_monitor_.last_left());
    }

    wsrep_seqno_t const ret(STATE_SEQNO());
    st_.set(state_uuid_, ret);

    log_info << "Provider paused at " << state_uuid_ << ':' << ret
             << " (" << pause_seqno_ << ")";

    return ret;
}

void galera::ReplicatorSMM::resume()
{
    assert(pause_seqno_ != WSREP_SEQNO_UNDEFINED);
    if (pause_seqno_ == WSREP_SEQNO_UNDEFINED)
    {
        gu_throw_error(EALREADY) << "tried to resume unpaused provider";
    }

    st_.set(state_uuid_, WSREP_SEQNO_UNDEFINED);
    log_info << "resuming provider at " << pause_seqno_;
    LocalOrder lo(pause_seqno_);
    pause_seqno_ = WSREP_SEQNO_UNDEFINED;
    local_monitor_.leave(lo);
    log_info << "Provider resumed.";
}

void galera::ReplicatorSMM::desync()
{
    wsrep_seqno_t seqno_l;

    ssize_t const ret(gcs_.desync(&seqno_l));

    if (seqno_l > 0)
    {
        LocalOrder lo(seqno_l); // need to process it regardless of ret value

        if (ret == 0)
        {
/* #706 - the check below must be state request-specific. We are not holding
          any locks here and must be able to wait like any other action.
          However practice may prove different, leaving it here as a reminder.
            if (local_monitor_.would_block(seqno_l))
            {
                gu_throw_error (-EDEADLK) << "Ran out of resources waiting to "
                                          << "desync the node. "
                                          << "The node must be restarted.";
            }
*/
            local_monitor_.enter(lo);
            state_.shift_to(S_DONOR);
            local_monitor_.leave(lo);
        }
        else
        {
            local_monitor_.self_cancel(lo);
        }
    }

    if (ret)
    {
        gu_throw_error (-ret) << "Node desync failed.";
    }
}

void galera::ReplicatorSMM::resync()
{
    gcs_.join(commit_monitor_.last_left());
}


//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
////                           Private
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

/* don't use this directly, use cert_and_catch() instead */
inline
wsrep_status_t galera::ReplicatorSMM::cert(TrxHandleSlave* trx)
{
    assert(trx->state() == TrxHandle::S_REPLICATING);

    assert(trx->local_seqno()     != WSREP_SEQNO_UNDEFINED);
    assert(trx->global_seqno()    != WSREP_SEQNO_UNDEFINED);
    assert(trx->last_seen_seqno() >= 0);
    assert(trx->last_seen_seqno() < trx->global_seqno());

    LocalOrder  lo(*trx);
    ApplyOrder  ao(*trx);
    CommitOrder co(*trx, co_mode_);

    bool interrupted(false);

    try
    {
        trx->set_state(TrxHandle::S_CERTIFYING);
        gu_trace(local_monitor_.enter(lo));
    }
    catch (gu::Exception& e)
    {
        if (e.get_errno() == EINTR) { interrupted = true; }
        else throw;
    }

    wsrep_status_t retval(WSREP_OK);
    bool const applicable(trx->global_seqno() > STATE_SEQNO());

    if (gu_likely (!interrupted))
    {
        switch (cert_.append_trx(trx))
        {
        case Certification::TEST_OK:
            if (gu_likely(applicable))
            {
                if (trx->state() == TrxHandle::S_CERTIFYING)
                {
                    retval = WSREP_OK;
                }
                else
                {
                    // trx was BF'ed during cert, but cert test passed
                    assert(trx->state() == TrxHandle::S_MUST_ABORT);
                    trx->set_state(TrxHandle::S_REPLICATING);
                    trx->set_state(TrxHandle::S_CERTIFYING);
                    retval = WSREP_BF_ABORT;
                }
            }
            else
            {
                // this can happen after SST position has been submitted
                // but not all actions preceding SST initial position
                // have been processed
                trx->set_state(TrxHandle::S_MUST_ABORT);
                retval = WSREP_TRX_FAIL;
            }
            break;
        case Certification::TEST_FAILED:
            if (gu_unlikely(trx->is_toi() && applicable)) // small sanity check
            {
                // may happen on configuration change
                log_warn << "Certification failed for TO isolated action: "
                         << *trx;
                assert(0);
            }
            local_cert_failures_ += trx->is_local();
            if (trx->state() != TrxHandle::S_MUST_ABORT)
                trx->set_state(TrxHandle::S_MUST_ABORT);
            retval = WSREP_TRX_FAIL;
            break;
        }

        if (gu_unlikely(WSREP_TRX_FAIL == retval))
        {
            report_last_committed(cert_.set_trx_committed(trx));
        }

        // at this point we are about to leave local_monitor_. Make sure
        // trx checksum was alright before that.
        trx->verify_checksum();

        // we must do it 'in order' for std::map reasons, so keeping
        // it inside the monitor
        gcache_.seqno_assign (trx->action(),
                              trx->global_seqno(),
                              trx->depends_seqno());

        local_monitor_.leave(lo);
    }
    else
    {
        assert(trx->state() == TrxHandle::S_MUST_ABORT ||
               trx->state() == TrxHandle::S_ABORTING);

        if (trx->state() != TrxHandle::S_MUST_ABORT)
            trx->set_state(TrxHandle::S_MUST_ABORT);

        retval = cert_for_aborted(trx);

        if (WSREP_TRX_FAIL == retval)
        {
            local_monitor_.self_cancel(lo);
            assert(trx->state() == TrxHandle::S_MUST_ABORT);
        }
        else
        {
            assert(WSREP_BF_ABORT == retval);
            assert(trx->state() == TrxHandle::S_REPLICATING);
        }
    }

    if (gu_unlikely(WSREP_TRX_FAIL == retval && applicable))
    {
        // applicable but failed certification: self-cancel monitors
        apply_monitor_.self_cancel(ao);
        if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.self_cancel(co);
    }

    return retval;
}

/* pretty much any exception in cert() is fatal as it blocks local_monitor_ */
wsrep_status_t galera::ReplicatorSMM::cert_and_catch(TrxHandleSlave* trx)
{
    try
    {
        return cert(trx);
    }
    catch (std::exception& e)
    {
        log_fatal << "Certification exception: " << e.what();
    }
    catch (...)
    {
        log_fatal << "Unknown certification exception";
    }
    abort();
}

/* This must be called BEFORE local_monitor_.self_cancel() due to
 * gcache_.seqno_assign() */
wsrep_status_t galera::ReplicatorSMM::cert_for_aborted(TrxHandleSlave* trx)
{
    assert(trx->state() == TrxHandle::S_MUST_ABORT);

    Certification::TestResult const res(cert_.test(trx, false));

    assert(trx->state() == TrxHandle::S_MUST_ABORT);

    switch (res)
    {
    case Certification::TEST_OK:
        trx->set_state(TrxHandle::S_REPLICATING);
        return WSREP_BF_ABORT;

    case Certification::TEST_FAILED:
        assert(trx->state() == TrxHandle::S_MUST_ABORT);
        // Mext step will be monitors release. Make sure that ws was not
        // corrupted and cert failure is real before proceeding with that.
        trx->verify_checksum();
        gcache_.seqno_assign (trx->action(), trx->global_seqno(), -1);
        return WSREP_TRX_FAIL;

    default:
        log_fatal << "Unexpected return value from Certification::test(): "
                  << res;
        abort();
    }
}


void
galera::ReplicatorSMM::update_state_uuid (const wsrep_uuid_t& uuid)
{
    if (state_uuid_ != uuid)
    {
        *(const_cast<wsrep_uuid_t*>(&state_uuid_)) = uuid;

        std::ostringstream os; os << state_uuid_;

        strncpy(const_cast<char*>(state_uuid_str_), os.str().c_str(),
                sizeof(state_uuid_str_));
    }

    st_.set(uuid, WSREP_SEQNO_UNDEFINED);
}

void
galera::ReplicatorSMM::abort()
{
    gcs_.close();
    gu_abort();
}
