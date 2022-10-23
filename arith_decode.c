/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_decode.h"
#include "bit_stream.h"

#include <assert.h>
#include <string.h>

void init_model(MODEL *model, uint32_t window_size)
{
    assert(window_size <= MAX_WINDOW_SIZE);

    model->prob[0]      = 1;
    model->prob[1]      = 1;
    model->history_prev = 0;
    model->history_next = 0;
    model->window_size  = window_size;
}

void update_model(MODEL *model, uint32_t bit)
{
    const uint32_t window_size = model->window_size;
    uint32_t       history_size;

    assert(bit <= 1);

    ++model->prob[bit];

    model->history[model->history_next++] = (uint8_t)bit;

    model->history_next %= MAX_WINDOW_SIZE * 2;

    assert(model->history_next != model->history_prev);
    history_size = model->history_next - model->history_prev;
    if (model->history_next < model->history_prev)
        history_size += MAX_WINDOW_SIZE * 2;

    if (history_size > window_size) {
        --model->prob[model->history[model->history_prev]];
        model->history_prev = (model->history_prev + 1) % (MAX_WINDOW_SIZE * 2);
    }
}

typedef struct {
    MODEL      model;
    BIT_STREAM stream;
    uint32_t   low;
    uint32_t   high;
    uint32_t   value;
} DECODER;

static void init_decoder(DECODER *decoder, const void *src, size_t src_size, uint32_t window_size)
{
    init_model(&decoder->model, window_size);
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
    const uint32_t mid   = (uint32_t)((range * prob0) / (prob0 + prob1));

    const uint8_t out_bit = decoder->value >= decoder->low + mid;

    update_model(&decoder->model, out_bit);

    if (out_bit)
        decoder->low += mid;
    else
        decoder->high = decoder->low + mid - 1;

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

void arith_decode(void *dest, size_t dest_size, const void *src, size_t src_size, uint32_t window_size)
{
    DECODER decoder;

    if ( ! dest_size)
        return;

    assert(src_size);

    init_decoder(&decoder, src, src_size, window_size);

    for (; dest_size; --dest_size) {

        uint32_t out_byte = 1;

        do {
            out_byte = (out_byte << 1) + decode_next_bit(&decoder);
        } while (out_byte < 0x100U);

        *(uint8_t *)dest = (uint8_t)out_byte;

        dest = (uint8_t *)dest + 1;
    }
}
