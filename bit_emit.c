/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "bit_emit.h"
#include <assert.h>

void init_bit_emitter(BIT_EMITTER *emitter, uint8_t *buf, size_t size)
{
    emitter->buf   = buf;
    emitter->begin = buf;
    emitter->end   = buf + size;
    emitter->data  = 1;
}

void emit_bit(BIT_EMITTER *emitter, uint32_t bit)
{
    assert(bit <= 1);

    emitter->data = (emitter->data << 1) | (bit & 1U);

    if (emitter->data > 0xFFU) {
        assert(emitter->buf < emitter->end);

        *(emitter->buf++) = (uint8_t)emitter->data;
        emitter->data     = 1;
    }
}

void emit_bits(BIT_EMITTER *emitter, size_t value, int bits)
{
    assert(bits);

    if (bits < (int)sizeof(value) * 8)
        value <<= (int)sizeof(value) * 8 - bits;

    do {
        const uint32_t bit = (uint32_t)((value >> (sizeof(value) * 8 - 1)) & 1U);

        emit_bit(emitter, bit);

        value <<= 1;
    } while (--bits);
}

size_t emit_tail(BIT_EMITTER *emitter)
{
    /* Duplicate last bit */
    const uint32_t last_bit = (emitter->data & 1U) * 0x7FU;

    /* Emit 7 bits, which is enough to force out the last byte, but won't emit a byte unnecessarily */
    emit_bits(emitter, last_bit, 7);

    return (size_t)(emitter->buf - emitter->begin);
}
