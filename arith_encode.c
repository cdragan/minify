/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_encode.h"
#include "arith_decode.h"
#include "bit_stream.h"

#include <assert.h>
#include <string.h>

typedef struct {
    uint8_t *dest;
    uint8_t *end;
    uint32_t data;
} EMIT;

static void init_emit(EMIT *emit, void *dest, size_t size)
{
    emit->dest = (uint8_t *)dest;
    emit->end  = emit->dest + size;
    emit->data = 0x100;
}

static void emit_bit(EMIT *emit, uint32_t bit)
{
    assert(bit <= 1);

    emit->data = (emit->data >> 1) | (bit << 8);

    if (emit->data & 1) {
        assert(emit->dest < emit->end);
        *(emit->dest++) = (uint8_t)(emit->data >> 1);
        emit->data      = 0x100;
    }
}

static size_t get_dest_size(const EMIT *emit, void *begin)
{
    return (size_t)(emit->dest - (uint8_t *)begin);
}

typedef struct {
    MODEL    model;
    EMIT     emit;
    uint32_t low;
    uint32_t high;
    uint32_t num_pending;
} ENCODER;

static void init_encoder(ENCODER *encoder, void *dest, size_t size, uint32_t window_size)
{
    init_model(&encoder->model, window_size);
    init_emit(&encoder->emit, dest, size);

    encoder->low         = 0;
    encoder->high        = ~0U;
    encoder->num_pending = 0;
}

static void encode_bit(ENCODER *encoder, uint32_t bit)
{
    const uint32_t prob0 = encoder->model.prob[0];
    const uint32_t prob1 = encoder->model.prob[1];

    const uint64_t range = (uint64_t)encoder->high - (uint64_t)encoder->low + 1;
    const uint32_t mid   = (uint32_t)((range * prob0) / (prob0 + prob1));

    update_model(&encoder->model, bit);

    if (bit)
        encoder->low += mid;
    else
        encoder->high = encoder->low + mid - 1;

    for (;;) {
        if (encoder->high < 0x80000000U || encoder->low >= 0x80000000U) {
            uint32_t out_bit = encoder->low >> 31;

            emit_bit(&encoder->emit, out_bit);

            out_bit ^= 1;
            while (encoder->num_pending) {
                emit_bit(&encoder->emit, out_bit);
                --encoder->num_pending;
            }
        }
        else if (encoder->low >= 0x40000000U && encoder->high < 0xC0000000U) {
            ++encoder->num_pending;
            encoder->low  &= ~0x40000000U;
            encoder->high |= 0x40000000U;
        }
        else
            break;

        encoder->low  = encoder->low << 1;
        encoder->high = (encoder->high << 1) + 1;
    }
}

static void emit_tail(ENCODER *encoder)
{
    uint32_t out_bit = (encoder->low >= 0x40000000U) ? 1 : 0;

    emit_bit(&encoder->emit, out_bit);

    out_bit ^= 1;

    /* Emit only one bit, decoder will duplicate last bit */
    emit_bit(&encoder->emit, out_bit);

    /* Emit last byte */
    while (encoder->emit.data != 0x100)
        emit_bit(&encoder->emit, out_bit);
}

size_t arith_encode(void *dest, size_t max_dest_size, const void *src, size_t size, uint32_t window_size)
{
    ENCODER              encoder;
    const uint8_t       *src_byte    = (const uint8_t *)src;
    const uint8_t *const src_end     = src_byte + size;

    if ( ! size)
        return 0;

    init_encoder(&encoder, dest, max_dest_size, window_size);

    for (; src_byte < src_end; ++src_byte) {
        uint32_t input_byte = *src_byte | 0x100U;
        do {
            encode_bit(&encoder, input_byte & 1);

            input_byte >>= 1;
        } while (input_byte != 1);
    }

    emit_tail(&encoder);

    return get_dest_size(&encoder.emit, dest);
}
