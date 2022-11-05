/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#define LZA_LENGTH_TAIL_BITS 11
#define MAX_LZA_SIZE (17 + (1 << LZA_LENGTH_TAIL_BITS))

enum LZ_STREAM {
    LZS_TYPE,
    LZS_LITERAL_MSB,
    LZS_LITERAL,
    LZS_SIZE,
    LZS_OFFSET,

    LZS_NUM_STREAMS
};
