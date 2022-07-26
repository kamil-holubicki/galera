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

#include "gu_enc_debug.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace gu {
void swrite(const char* format, ...)
{
    static const size_t buffer_len = 8*1024;
    char buffer[buffer_len] = {0};

    va_list args;
    va_start(args, format);

#if 0
    sprintf(buffer, "ENC: ");
    int offset = strlen(buffer);
#else
    int offset = 0;
#endif
    vsnprintf (buffer + offset, buffer_len-offset, format, args);

    write(STDERR_FILENO, buffer, strlen(buffer));
    va_end(args);
}

void dumpMemory(void *ptr, size_t size) {
    S_DEBUG("DUMP START x%llX, size: %ld", (unsigned long long)ptr, size);
    unsigned char *p = (unsigned char*)ptr;
    p = p;  // make the compile happy when debug macros are disabled
    for (size_t i = 0; i < size; ++i) {
        if(i%16==0) {
            S_DEBUG("\n");
        }
        S_DEBUG("%02x ", p[i]);
    }
    S_DEBUG("\nDUMP END x%llX, size: %ld\n", (unsigned long long)ptr, size);
}

}  // namespace