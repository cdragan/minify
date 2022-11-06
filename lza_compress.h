/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>

typedef struct {
    size_t compressed;          /* Final compressed size       */
    size_t lz;                  /* Total size after LZ77 compression */

    size_t stats_lit;           /* Number of LIT packets       */
    size_t stats_match;         /* Number of MATCH packets     */
    size_t stats_shortrep;      /* Number of SHORTREP packets  */
    size_t stats_longrep[4];    /* Number of LONGREP* packets  */
} COMPRESSED_SIZES;

/* Returns size of the working buffer needed for compress() for the given input size */
size_t estimate_compress_size(size_t src_size);

COMPRESSED_SIZES lza_compress(void       *dest,
                              size_t      dest_size,
                              const void *src,
                              size_t      src_size);
