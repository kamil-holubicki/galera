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

#ifndef __ENC_PMEMORY_MANAGER__
#define __ENC_PMEMORY_MANAGER__

#include <memory>
#include <vector>

namespace gu {

struct PPage {
    int fd_;
    size_t offset_;
    unsigned char* ptr_;
};

/* PMemoryManger acts as the allocator of physical pages (PPage) for
   EncMMap. The size of physical memory is limited, so the client needs
   to handle this fact with proper management of them (flushing/fetching/etc)
*/
class PMemoryManager {
public:
    PMemoryManager(size_t pagesCnt, size_t allocPageSize);
    ~PMemoryManager();
    PMemoryManager(const gu::PMemoryManager&) = delete;
    PMemoryManager operator=(const gu::PMemoryManager&) = delete;

    std::shared_ptr<PPage> alloc();
    void free(std::shared_ptr<PPage> page);
    /* Reset manager to its initial state. All pages are marked as free.
       This is useful for clients who decide to stop usage of physical memory
       without freeing allocated pages (e.g. they no longer care about the data)
    */
    void freeAll();
    void GetCreateParams(size_t* size, size_t* allocPageSize);

private:
    size_t createSize_;
    unsigned char* base_;
    size_t size_;
    std::vector<std::shared_ptr<PPage>> freePages_;
    std::vector<std::shared_ptr<PPage>> myPages_;
    int fd_;
    bool mapped_;
    size_t allocPagesCnt_;
    size_t allocPageSize_;

    bool createTmpFile();
};

}  // namespace
#endif