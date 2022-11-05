/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#define MAX_LZA_SIZE 273

enum LZ_STREAM {
    LZS_TYPE,
    LZS_LITERAL_MSB,
    LZS_LITERAL,
    LZS_SIZE,
    LZS_OFFSET,

    LZS_NUM_STREAMS
};
