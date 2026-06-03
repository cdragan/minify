/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include <stddef.h>

typedef struct {
    size_t compressed;          /* Final compressed size       */

    size_t stats_lit;           /* Number of LIT packets       */
    size_t stats_match;         /* Number of MATCH packets     */
    size_t stats_shortrep;      /* Number of SHORTREP packets  */
    size_t stats_longrep[4];    /* Number of LONGREP* packets  */
} COMPRESSED_SIZES;

/* Tracks per-symbol coder cost during compression: the binary decisions that
 * went in, and the bits the range coder committed to the stream.
 */
typedef struct {
    const size_t *symbol_starts;         /* Input array, sorted symbol pointers in src       */
    size_t       *symbol_in_bit_counts;  /* Output: decisions coded per symbol (pre-entropy) */
    size_t       *symbol_out_bit_counts; /* Output: bits emitted per symbol (post-entropy)   */
    size_t        num_symbols;           /* Number of items in the above arrays              */
} SYMBOL_BIT_COUNT;

/* Returns size of the working buffer needed by lza_compress for the input size */
size_t estimate_compress_size(size_t src_size);

COMPRESSED_SIZES lza_compress(void             *dest,
                              size_t            dest_size,
                              const void       *src,
                              size_t            src_size,
                              SYMBOL_BIT_COUNT *symbols);
