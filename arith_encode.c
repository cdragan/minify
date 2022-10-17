/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_encode.h"

#include <assert.h>
#include <string.h>

typedef struct {
    uint32_t prob[2];
    uint32_t history_prev;
    uint32_t history_next;
    uint32_t window_size;
    uint8_t  history[MAX_WINDOW_SIZE * 2];
} MODEL;

static void init_model(MODEL *model, uint32_t window_size)
{
    assert(window_size <= MAX_WINDOW_SIZE);

    model->prob[0]      = 1;
    model->prob[1]      = 1;
    model->history_prev = 0;
    model->history_next = 0;
    model->window_size  = window_size;
}

static void update_model(MODEL *model, uint32_t bit)
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
    uint8_t *dest;
    uint8_t *end;
    uint32_t data;
} EMIT;

static void init_emit(EMIT *emit, void *dest, size_t size)
{
    emit->dest = (uint8_t *)dest;
    emit->end  = emit->dest + size;
    emit->data = 1;
}

static void emit_bit(EMIT *emit, uint32_t bit)
{
    assert(bit <= 1);

    emit->data = (emit->data << 1) | bit;

    if (emit->data > 0xFFU) {
        assert(emit->dest < emit->end);
        *(emit->dest++) = (uint8_t)emit->data;
        emit->data      = 1;
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
            uint32_t out_bit = (encoder->low >= 0x80000000U) ? 1 : 0;

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
    while (encoder->emit.data != 1)
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

typedef struct {
    const uint8_t *next_byte;
    size_t         bytes_left;
    uint32_t       data;
} BIT_PULLER;

static void init_bit_puller(BIT_PULLER *bit_puller, const void *src, size_t src_size)
{
    assert(src_size > 0);

    bit_puller->data       = 0x100U | *(const uint8_t *)src;
    bit_puller->next_byte  = (const uint8_t *)src + 1;
    bit_puller->bytes_left = src_size - 1;
}

static uint32_t pull_bits(BIT_PULLER *bit_puller, int bits)
{
    uint32_t out_value = 0;
    uint32_t in_data   = bit_puller->data;

    do {
        out_value = (out_value << 1) | ((in_data >> 7) & 1);
        in_data   <<= 1;
        --bits;

        if (in_data >= 0x10000U) {
            if (bit_puller->bytes_left) {
                in_data = 0x100U | *(bit_puller->next_byte++);
                --bit_puller->bytes_left;
            }
            else
                /* Duplicate last bit */
                in_data >>= 1;
        }
    } while (bits);

    bit_puller->data = in_data;

    return out_value;
}

typedef struct {
    MODEL      model;
    BIT_PULLER bit_puller;
    uint32_t   low;
    uint32_t   high;
    uint32_t   value;
} DECODER;

static void init_decoder(DECODER *decoder, const void *src, size_t src_size, uint32_t window_size)
{
    init_model(&decoder->model, window_size);
    init_bit_puller(&decoder->bit_puller, src, src_size);
    decoder->low   = 0;
    decoder->high  = ~0U;
    decoder->value = pull_bits(&decoder->bit_puller, 32);
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
        decoder->value = (decoder->value << 1) + pull_bits(&decoder->bit_puller, 1);
    }

    return out_bit;
}

void arith_decode(void *dest, size_t dest_size, const void *src, size_t src_size, uint32_t window_size)
{
    BIT_PULLER bit_puller;
    DECODER    decoder;

    if ( ! dest_size)
        return;

    assert(src_size);

    init_decoder(&decoder, src, src_size, window_size);

    for (; dest_size; --dest_size) {

        int     bit;
        uint8_t out_byte = 0;

        for (bit = 0; bit < 8; bit++)
            out_byte |= decode_next_bit(&decoder) << bit;

        *(uint8_t *)dest = out_byte;

        dest = (uint8_t *)dest + 1;
    }
}
