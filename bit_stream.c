/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "bit_stream.h"
#include <assert.h>

#ifndef NDEBUG
void init_bit_stream(BIT_STREAM *stream, const char *buf, size_t size)
{
    stream->buf  = buf;
    stream->end  = buf + size;
    stream->data = 1;
}
#endif

uint32_t get_bits(BIT_STREAM *stream, int bits)
{
    uint32_t value = 0;
    uint32_t data  = stream->data;

    while (bits) {
        if (data == 1) {
            assert(stream->buf < stream->end);
            data = 0x100 + (uint8_t)*(stream->buf++);
        }

        value = (value << 1) | (data & 1);
        data >>= 1;
        --bits;
    }

    stream->data = data;

    return value;
}

uint32_t get_one_bit(BIT_STREAM *stream)
{
    return get_bits(stream, 1);
}
