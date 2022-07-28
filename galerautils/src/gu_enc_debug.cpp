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
#include <time.h>
#include <unistd.h>
#include <cstdio>

/* Server's message format is like:
2022-07-13T13:14:22.591390Z 4143 [Note] [MY-000000] [Galera] Actual message goes here
*/
static const char* date_time_format = "%Y-%m-%dT%X.000000Z ";
static const char* date_time_default = "0000-00-00T00:00:00.000000Z ";
static const size_t date_time_len = strlen(date_time_default);
static const char* note_prefix_format = "0 [Note] [MY-000000] [Galera] ";
static const size_t note_prefix_len = strlen(note_prefix_format);
static const char* warning_prefix_format = "0 [Warning] [MY-000000] [Galera] ";
static const size_t warning_prefix_len = strlen(warning_prefix_format);
static const char* error_prefix_format = "0 [ERROR] [MY-000000] [Galera] ";
static const size_t error_prefix_len = strlen(error_prefix_format);

namespace gu {

static const char* get_prefix(DebugLevel level, size_t* prefix_len) {
    const char* format;

    switch (level)
    {
    case ERROR:
        *prefix_len = error_prefix_len;
        format = error_prefix_format;
        break;
    case WARNING:
        *prefix_len = warning_prefix_len;
        format = warning_prefix_format;
        break;
    default:
        *prefix_len = note_prefix_len;
        format = note_prefix_format;
        break;
    }

    return format;
}

void swrite(DebugLevel level, const char* format, ...)
{
    static const size_t buffer_len = 8*1024;
    char buffer[buffer_len] = {0};
    int offset = 0;

    va_list args;
    va_start(args, format);

    // date/time
    time_t     now = time(0);
    struct tm  tstruct;
    struct tm* res = gmtime_r(&now, &tstruct);
    if (res == &tstruct) {
        strftime(buffer, buffer_len, date_time_format, &tstruct);
    } else {
        snprintf(buffer, buffer_len, "%s", date_time_default);
    }

    offset += date_time_len;

    // prefix
    size_t prefix_len;
    const char* prefix = get_prefix(level, &prefix_len);;
    snprintf(buffer + offset, buffer_len - offset, "%s", prefix);
    offset += prefix_len;

    // actual message
    vsnprintf(buffer + offset, buffer_len - offset, format, args);

    write(STDERR_FILENO, buffer, strlen(buffer));
    va_end(args);
}

void dumpMemory(void *ptr, size_t size) {
    S_DEBUG_N("DUMP START x%llX, size: %ld", (unsigned long long)ptr, size);
    unsigned char *p = (unsigned char*)ptr;
    p = p;  // make the compile happy when debug macros are disabled
    for (size_t i = 0; i < size; ++i) {
        if(i%16==0) {
            S_DEBUG_N("\n");
        }
        S_DEBUG_N("%02x ", p[i]);
    }
    S_DEBUG_N("\nDUMP END x%llX, size: %ld\n", (unsigned long long)ptr, size);
}

}  // namespace