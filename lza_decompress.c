/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#include "lza_decompress.h"

#include "arith_model.h"
#include "bit_stream.h"
#include "lza_context.h"

#include <stdint.h>

typedef struct {
    MODEL      ctx[NUM_CONTEXT_BITS];
    BIT_STREAM stream;
    uint32_t   low;
    uint32_t   high;
    uint32_t   value;
} DECODER;

static uint32_t decode_bit(DECODER *decoder, uint32_t ci)
{
    MODEL *const   model = &decoder->ctx[ci];
    const uint32_t prob0 = model->prob[0];
    const uint32_t prob1 = model->prob[1];
    const uint64_t range = (uint64_t)decoder->high - (uint64_t)decoder->low + 1;
    const uint32_t mid   = decoder->low + (uint32_t)((range * prob0) / (prob0 + prob1));
    const uint32_t bit   = decoder->value >= mid;

    update_model(model, bit);

    if (bit)
        decoder->low = mid;
    else
        decoder->high = mid - 1;

    for (;;) {
        if (decoder->high < 0x80000000U || decoder->low >= 0x80000000U) {
        }
        else if (decoder->low >= 0x40000000U && decoder->high < 0xC0000000U) {
            decoder->value -= 0x40000000U;
            decoder->low   &= ~0x40000000U;
            decoder->high  |= 0x40000000U;
        }
        else
            break;

        decoder->low   = decoder->low << 1;
        decoder->high  = (decoder->high << 1) + 1;
        decoder->value = (decoder->value << 1) + get_one_bit(&decoder->stream);
    }

    return bit;
}

static uint32_t decode_context_tree(DECODER *decoder, uint32_t base, uint32_t nbits)
{
    uint32_t node = 1;
    uint32_t i;

    for (i = 0; i < nbits; i++)
        node = (node << 1) | decode_bit(decoder, base + node);

    return node - (1U << nbits);
}

static uint32_t decode_pos(DECODER *decoder, uint32_t base, uint32_t nbits)
{
    uint32_t value = 0;
    uint32_t i;

    for (i = 0; i < nbits; i++)
        value = (value << 1) | decode_bit(decoder, base + i);

    return value;
}

static uint32_t decode_length(DECODER *decoder)
{
    if ( ! decode_bit(decoder, CTX_LEN_SEL_POS))
        return 2 + decode_context_tree(decoder, CTX_LEN_SHORT_POS, 3);

    if ( ! decode_bit(decoder, CTX_LEN_SEL_POS + 1))
        return 10 + decode_context_tree(decoder, CTX_LEN_MID_POS, 3);

    return 18 + decode_pos(decoder, CTX_LEN_TAIL_POS, 11);
}

static uint32_t decode_distance(DECODER *decoder)
{
    const uint32_t slot = decode_context_tree(decoder, CTX_DIST_SLOT_POS, 6);
    uint32_t       mant_bits;

    if (slot < 2)
        return slot + 1;

    mant_bits = (slot >> 1) - 1;

    return (((slot & 1) + 2) << mant_bits) + decode_pos(decoder, CTX_DIST_MANTISSA_POS, mant_bits) + 1;
}

void lza_decompress(void *input_dest, size_t dest_size, const void *input_src, size_t src_size)
{
    DECODER         decoder_storage;
    DECODER *const  decoder       = &decoder_storage;
    uint32_t        last_dist[4]  = { 0, 0, 0, 0 };
    uint8_t        *dest          = (uint8_t *)input_dest;
    uint8_t  *const end           = dest + dest_size;
    uint32_t        i;

    for (i = 0; i < NUM_CONTEXT_BITS; i++)
        init_model(&decoder->ctx[i]);

    init_bit_stream(&decoder->stream, (const uint8_t *)input_src, src_size);
    decoder->low   = 0;
    decoder->high  = ~0U;
    decoder->value = get_bits(&decoder->stream, 32);

    do {
        if ( ! decode_bit(decoder, CTX_TYPE_POS)) {
            *dest++ = (uint8_t)decode_context_tree(decoder, CTX_LIT_POS, 8);
        }
        else {
            uint32_t distance;
            uint32_t length;

            if ( ! decode_bit(decoder, CTX_TYPE_POS + 1)) {
                length   = decode_length(decoder);
                distance = decode_distance(decoder);
            }
            else {
                const uint32_t selector = (decode_bit(decoder, CTX_TYPE_POS + 2) << 1) |
                                          decode_bit(decoder, CTX_TYPE_POS + 3);

                if (selector == 0) {
                    distance = last_dist[0];
                    length   = 1;
                }
                else {
                    const uint32_t idx = (selector < 3) ? (selector - 1)
                                                        : (2U + decode_bit(decoder, CTX_TYPE_POS + 4));

                    distance = last_dist[idx];
                    length   = decode_length(decoder);
                }
            }

            for (i = 0; i < 3; i++)
                if (last_dist[i] == distance)
                    break;
            for (; i; i--)
                last_dist[i] = last_dist[i - 1];
            last_dist[0] = distance;

            for (i = 0; i < length; i++, dest++)
                *dest = *(dest - distance);
        }
    } while (dest < end);
}
