/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "buffer.h"

#include <assert.h>
#include <stdlib.h>

BUFFER buf_alloc(size_t size)
{
    BUFFER buf;

    buf.buf = (uint8_t *)calloc(size, 1);

    if (buf.buf)
        buf.size = size;

    return buf;
}

BUFFER buf_truncate(BUFFER buf, size_t pos)
{
    assert(pos <= buf.size);

    buf.size = pos;

    return buf;
}

BUFFER buf_get_tail(BUFFER buf, size_t pos)
{
    assert(pos <= buf.size);

    buf.buf  += pos;
    buf.size -= pos;

    return buf;
}

BUFFER buf_slice(BUFFER buf, size_t pos, size_t size)
{
    assert(buf_at_offset(buf, pos, size) != NULL);

    buf.buf += pos;
    buf.size = size;

    return buf;
}

uint8_t *buf_at_offset(BUFFER buf, size_t pos, size_t size)
{
    if (pos >= buf.size || size > buf.size || pos + size > buf.size)
        return NULL;

    return buf.buf + pos;
}
