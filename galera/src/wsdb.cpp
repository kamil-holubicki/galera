/*
 * Copyright (C) 2010-2014 Codership Oy <info@codership.com>
 */

#include "wsdb.hpp"
#include "trx_handle.hpp"
#include "write_set.hpp"

#include "gu_lock.hpp"
#include "gu_throw.hpp"


void galera::Wsdb::print(std::ostream& os) const
{
    os << "trx map:\n";
    for (galera::Wsdb::TrxMap::const_iterator i = trx_map_.begin();
         i != trx_map_.end();
         ++i)
    {
        os << i->first << " " << *i->second << "\n";
    }
    os << "conn query map:\n";
    for (galera::Wsdb::ConnMap::const_iterator i = conn_map_.begin();
         i != conn_map_.end();
         ++i)
    {
        os << i->first << " ";
    }
    os << "\n";
}


galera::Wsdb::Wsdb()
    :
    trx_pool_  (TrxHandle::LOCAL_STORAGE_SIZE(), 512, "LocalTrxHandle"),
    trx_map_   (),
    trx_mutex_ (),
    conn_map_  (),
    conn_mutex_()
{}


galera::Wsdb::~Wsdb()
{
    log_info << "wsdb trx map usage " << trx_map_.size()
             << " conn query map usage " << conn_map_.size();
    log_info << trx_pool_;

    // With debug builds just print trx and query maps to stderr
    // and don't clean up to let valgrind etc to detect leaks.
#ifndef NDEBUG
    std::cerr << *this;
#else
    for_each(trx_map_.begin(), trx_map_.end(), Unref2nd<TrxMap::value_type>());
#endif // !NDEBUG
}


inline galera::TrxHandlePtr
galera::Wsdb::create_trx(const TrxHandle::Params& params,
                         const wsrep_uuid_t&  source_id,
                         wsrep_trx_id_t const trx_id)
{
    TrxHandlePtr trx(new_trx(params, source_id, trx_id));

    std::pair<TrxMap::iterator, bool> i(trx_map_.insert(std::make_pair(trx_id, trx)));
    if (gu_unlikely(i.second == false)) gu_throw_fatal;

    return i.first->second;
}


galera::TrxHandlePtr
galera::Wsdb::get_trx(const TrxHandle::Params& params,
                      const wsrep_uuid_t&      source_id,
                      wsrep_trx_id_t const     trx_id,
                      bool const               create)
{
    gu::Lock lock(trx_mutex_);
    TrxMap::iterator const i(trx_map_.find(trx_id));
    if (i == trx_map_.end() && create)
    {
        return create_trx(params, source_id, trx_id);
    }
    else if (i == trx_map_.end())
    {
        return TrxHandlePtr();
    }

    return i->second;
}


galera::Wsdb::Conn*
galera::Wsdb::get_conn(wsrep_conn_id_t const conn_id, bool const create)
{
    gu::Lock lock(conn_mutex_);

    ConnMap::iterator i(conn_map_.find(conn_id));

    if (conn_map_.end() == i)
    {
        if (create == true)
        {
            std::pair<ConnMap::iterator, bool> p
                (conn_map_.insert(std::make_pair(conn_id, Conn(conn_id))));

            if (gu_unlikely(p.second == false)) gu_throw_fatal;

            return &p.first->second;
        }

        return 0;
    }

    return &(i->second);
}


galera::TrxHandlePtr
galera::Wsdb::get_conn_query(const TrxHandle::Params& params,
                             const wsrep_uuid_t&      source_id,
                             wsrep_trx_id_t const     conn_id,
                             bool const               create)
{
    Conn* const conn(get_conn(conn_id, create));

    if (0 == conn)
    {
        throw gu::NotFound();
    }

    if (conn->get_trx() == 0 && create == true)
    {
        TrxHandlePtr trx
            (TrxHandle::New(trx_pool_, params, source_id, conn_id, -1),
             TrxHandleDeleter());
        conn->assign_trx(trx);
    }

    return conn->get_trx();
}


void galera::Wsdb::discard_trx(wsrep_trx_id_t trx_id)
{
    gu::Lock lock(trx_mutex_);
    TrxMap::iterator i;
    if ((i = trx_map_.find(trx_id)) != trx_map_.end())
    {
        trx_map_.erase(i);
    }
}


void galera::Wsdb::discard_conn_query(wsrep_conn_id_t conn_id)
{
    gu::Lock lock(conn_mutex_);
    ConnMap::iterator i;
    if ((i = conn_map_.find(conn_id)) != conn_map_.end())
    {
        i->second.reset_trx();
    }
}

void galera::Wsdb::discard_conn(wsrep_conn_id_t conn_id)
{
    gu::Lock lock(conn_mutex_);
    ConnMap::iterator i;
    if ((i = conn_map_.find(conn_id)) != conn_map_.end())
    {
        conn_map_.erase(i);
    }
}
