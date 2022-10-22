/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t offset;
    size_t length;
    int    last;
} OCCURRENCE;

typedef void (* REPORT_LITERAL)(void *cookie, const char *buf, size_t pos, size_t size);
typedef void (* REPORT_MATCH  )(void *cookie, const char *buf, size_t pos, OCCURRENCE occurrence);

int find_repeats(const char    *buf,
                 size_t         size,
                 REPORT_LITERAL report_literal,
                 REPORT_MATCH   report_match,
                 void          *cookie);
