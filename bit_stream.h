/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *buf;
#ifndef NDEBUG
    const char *end;
#endif
    uint32_t    data;
} BIT_STREAM;

#ifdef NDEBUG
static inline void init_bit_stream(BIT_STREAM *stream, const char *buf, size_t)
{
    stream->buf  = buf;
    stream->data = 1;
}
#else
void init_bit_stream(BIT_STREAM *stream, const char *buf, size_t size);
#endif

uint32_t get_bits(BIT_STREAM *stream, int bits);

uint32_t get_one_bit(BIT_STREAM *stream);
