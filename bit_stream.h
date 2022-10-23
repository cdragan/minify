/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *buf;
    const uint8_t *end;
    uint32_t       data;
} BIT_STREAM;

void init_bit_stream(BIT_STREAM *stream, const void *buf, size_t size);

uint32_t get_bits(BIT_STREAM *stream, int bits);

uint32_t get_one_bit(BIT_STREAM *stream);
