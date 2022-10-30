/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "bit_stream.h"
#include <assert.h>
#include <stdio.h>

void init_bit_stream(BIT_STREAM *stream, const void *buf, size_t size)
{
    assert(size > 0);

    stream->buf  = (const uint8_t *)buf;
    stream->end  = (const uint8_t *)buf + size;
    stream->data = 0;
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
        if ( ! (uint8_t)data) {
            if (stream->buf < stream->end)
                data = ((uint32_t)*(stream->buf++) << 1) | 1U;
            else
                data >>= 1;
        }

        value = (value << 1) | ((data >> 8) & 1U);
        data <<= 1;
        --bits;
    }

    stream->data = data;

    return value;
}
