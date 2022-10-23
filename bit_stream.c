/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "bit_stream.h"
#include <assert.h>

void init_bit_stream(BIT_STREAM *stream, const void *buf, size_t size)
{
    assert(size > 0);

    stream->buf  = (const uint8_t *)buf;
    stream->end  = (const uint8_t *)buf + size;
    stream->data = 1;
}

uint32_t get_one_bit(BIT_STREAM *stream)
{
    return get_bits(stream, 1);
}

uint32_t get_bits(BIT_STREAM *stream, int bits)
{
    uint32_t value = 0;
    uint32_t data  = stream->data;

    while (bits) {
        if (data == 1) {
            if (stream->buf < stream->end)
                data = 0x100U + *(stream->buf++);
            else
                data = (0x100U + *(stream->buf - 1)) >> 7;
        }

        value = (value << 1) | (data & 1);
        data >>= 1;
        --bits;
    }

    stream->data = data;

    return value;
}
