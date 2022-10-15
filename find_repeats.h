/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

typedef void (* REPORT_UNIQUE_BYTES)(void *cookie, const char *buf, size_t pos, size_t size);
typedef void (* REPORT_REPEAT      )(void *cookie, const char *buf, size_t pos, size_t offset, size_t size);

int find_repeats(const char         *buf,
                 size_t              size,
                 REPORT_UNIQUE_BYTES report_unique_bytes,
                 REPORT_REPEAT       report_repeat,
                 void               *cookie);
