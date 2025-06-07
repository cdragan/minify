/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_decode.h"
#include "bit_stream.h"

#include <assert.h>
#include <string.h>

void init_model(MODEL *model)
{
    const uint32_t hist_buckets = (uint32_t)(sizeof(model->history) / sizeof(model->history[0]));
    uint32_t       i;

    model->prob[0] = hist_buckets * 16 + 1;
    model->prob[1] = hist_buckets * 16 + 1;

    for (i = 0; i < hist_buckets; i++)
        model->history[i] = 0xAAAAAAAAULL;
}

void update_model(MODEL *model, uint32_t bit)
{
    const uint32_t hist_buckets = (uint32_t)(sizeof(model->history) / sizeof(model->history[0]));
    uint32_t       i;

    assert(bit <= 1);

    ++model->prob[bit];

    for (i = 0; i < hist_buckets; i++) {

        const uint32_t old_bit = model->history[i] >> 31;

        model->history[i] = (model->history[i] << 1) | bit;

        bit = old_bit;
    }

    --model->prob[bit];
}

typedef struct {
    MODEL      model;
    BIT_STREAM stream;
    uint32_t   low;
    uint32_t   high;
    uint32_t   value;
} DECODER;

static void init_decoder(DECODER *decoder, const void *src, size_t src_size)
{
    init_model(&decoder->model);
    init_bit_stream(&decoder->stream, src, src_size);
    decoder->low   = 0;
    decoder->high  = ~0U;
    decoder->value = get_bits(&decoder->stream, 32);
}

static uint8_t decode_next_bit(DECODER *decoder)
{
    const uint32_t prob0 = decoder->model.prob[0];
    const uint32_t prob1 = decoder->model.prob[1];

    const uint64_t range = (uint64_t)decoder->high - (uint64_t)decoder->low + 1;
    const uint32_t mid   = decoder->low + (uint32_t)((range * prob0) / (prob0 + prob1));

    const uint8_t out_bit = decoder->value >= mid;

    update_model(&decoder->model, out_bit);

    if (out_bit)
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

    return out_bit;
}

void arith_decode(void *dest, size_t dest_size, const void *src, size_t src_size)
{
    DECODER decoder;

    assert(src_size);
    assert(dest_size);

    init_decoder(&decoder, src, src_size);

    do {
        uint32_t out_byte = 1;

        do {
            out_byte = (out_byte << 1) + decode_next_bit(&decoder);
        } while (out_byte < 0x100U);

        *(uint8_t *)dest = (uint8_t)out_byte;

        dest = (uint8_t *)dest + 1;
    } while (--dest_size);
}
