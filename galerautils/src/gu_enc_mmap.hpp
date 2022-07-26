/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef __GCACHE_MMAPENC__
#define __GCACHE_MMAPENC__

#include <signal.h>
#include <queue>
#include <string>
#include <memory>
#include <map>
#include <atomic>
#include "gu_mmap.hpp"
#include "enc_stream_cipher.h"
#include <mutex>
#include <condition_variable>
#include <thread>

namespace gu {

class PPage;
class PMemoryManager;

void dump_mappings();


class EncMMap : public IMMap
{
public:
    EncMMap(const std::string &key, std::shared_ptr<MMap> mmap,
            size_t cachePageSize, size_t cacheSize, bool syncOnDestroy = false,
            size_t encryptionStartOffset = 0);
    ~EncMMap();

    EncMMap(const EncMMap&) = delete;
    EncMMap operator=(const EncMMap&) = delete;

    size_t get_size() const override;
    void*  get_ptr() const override;

    void dont_need() const override;
    void sync(void *addr, size_t length) const override;
    void sync() const override;
    void unmap() override;
    void set_key(const std::string& key) override;
    void set_access_mode(AccessMode mode) override;

    void handle_signal(siginfo_t*);

    bool lock();
    void unlock();

private:
    void encrypt(unsigned char* dst, unsigned char* src, size_t size, size_t pageNumber) const;
    void decrypt(unsigned char* dst, unsigned char* src, size_t size, size_t pageNumber) const;

    friend class EncMMapsRepository;
    void dump_mappings();

    std::shared_ptr<MMap> mmapraw_;
    size_t pageSize_;
    unsigned char* mmaprawPtr_;
    size_t vMemSize_;
    unsigned char* mmap_ptr_;
    unsigned char* base_;
    std::shared_ptr<PMemoryManager> memoryManagerP_;
    PMemoryManager &memoryManager_;
    std::shared_ptr<int> vpage2protectionGuard_;
    // virtual page to its actual mprotect map
    int* vpage2protection_;
    // virtual page to physical page map
    std::map<unsigned char*, std::shared_ptr<PPage>> vpage2ppage_;
    size_t pagesCnt_;
    bool mapped_;
    size_t lastPageSize_;
    size_t encryptionStartOffset_;
    int defaultPageProtection_;
    size_t readAheadCnt_;
    std::atomic_flag lock_;
    mutable Aes_ctr_encryptor encryptor_;
    mutable Aes_ctr_decryptor decryptor_;
    bool syncOnDestroy_;

    inline bool is_last_page(size_t page) const {
        return page == pagesCnt_-1;
    }
    unsigned char* page_start(unsigned long long pageNo) const;
    unsigned char* page_start(unsigned char* addr) const;
    size_t page_number(unsigned char* addr) const;
    void mprotectd(unsigned char *ptr, size_t size, int prot) const;
};

} /* namespace gu */

#endif