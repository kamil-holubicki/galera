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

#ifndef __ENC_PMEMORY_MANAGER_POOL__
#define __ENC_PMEMORY_MANAGER_POOL__

#include <memory>
#include <mutex>
#include <set>

namespace gu {
class PMemoryManager;
struct PMemoryManagerHolder {
    PMemoryManagerHolder(uint64_t timestamp, std::shared_ptr<PMemoryManager> manager);
    bool operator <(const PMemoryManagerHolder& rhs) const;

    uint64_t timestamp_;
    std::shared_ptr<PMemoryManager> manager_;
    size_t mgrSize_;
    size_t mgrAllocPageSize_;
};

/* PMemoryManagerPool is the pool of physical memory managers used by
   MMapEnc objects. Its purpose is to avoid physical memory
   allocation/deallocation when MMapEnc object is created, which is time consuming
   generic task. Instead of this, we can reuse already configured PMemoryManager
   object
*/
class PMemoryManagerPool {
public:
    PMemoryManagerPool(size_t managersPoolSize);
    PMemoryManagerPool(const PMemoryManagerPool&) = delete;
    PMemoryManagerPool& operator=(const PMemoryManagerPool&) = delete;

    std::shared_ptr<PMemoryManager> allocate(size_t allocPageSize, size_t size);
    void free(std::shared_ptr<PMemoryManager>mgr);

private:
    // we can use mutex for managers_ protection, because it is never
    // accessed from the signal handler context. We access it only
    // when the client request creation of new EncMMap object.
    std::mutex mtx_;
    std::set<PMemoryManagerHolder> managers_;
    size_t poolSizeMax_;
    size_t poolSize_;
};

}  // namespace
#endif /* __ENC_PMEMORY_MANAGER_POOL__ */