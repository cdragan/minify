/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>

typedef struct {
    size_t total;
    size_t types;
    size_t literals;
    size_t sizes;
    size_t offsets;

    size_t stats_lit;
    size_t stats_match;
    size_t stats_shortrep;
    size_t stats_longrep[4];
} COMPRESSED_SIZES;

COMPRESSED_SIZES compress(void       *dest,
                          size_t      dest_size,
                          const void *src,
                          size_t      src_size);
