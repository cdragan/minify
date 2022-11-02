/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_decompress.h"

#include "arith_decode.h"
#include "bit_stream.h"
#include "lza_stream.h"

#include <assert.h>
#include <stdint.h>

static uint32_t decode_length(BIT_STREAM *stream)
{
    uint32_t data  = get_one_bit(stream);
    uint32_t value = 2;
    int      bits  = 3;

    if (data) {
        data   = get_one_bit(stream);
        value += 8;

        if (data) {
            bits   = 8;
            value += 8;
        }
    }

    return value + get_bits(stream, bits);
}

static uint32_t decode_distance(BIT_STREAM *stream)
{
    uint32_t data = get_bits(stream, 6);
    uint32_t bits;

    if (data < 2)
        return data + 1;

    bits = (data >> 1) - 1;

    return (((data & 1) + 2) << bits) + get_bits(stream, (int)bits) + 1;
}

void lz_decompress(void       *input_dest,
                   size_t      dest_size,
                   const void *input_src)
{
    BIT_STREAM     stream[LZS_NUM_STREAMS];
    uint32_t       stream_size[LZS_NUM_STREAMS];
    uint32_t       last_dist[4] = { 0, 0, 0, 0 };
    uint8_t       *dest         = (uint8_t *)input_dest;
#ifndef NDEBUG
    uint8_t *const begin        = dest;
#endif
    uint8_t *const end          = dest + dest_size;
    const uint8_t *input        = (const uint8_t *)input_src;
    uint32_t       i_stream;
    uint8_t        prev_lit     = 0;

    assert(dest_size);

    /* Load sizes of each stream from input */
    init_bit_stream(&stream[0], input, dest_size);
    for (i_stream = 0; i_stream < LZS_NUM_STREAMS; i_stream++)
        stream_size[i_stream] = decode_distance(&stream[0]);

    /* Prepare input streams */
    input = stream[0].buf;
    for (i_stream = 0; i_stream < LZS_NUM_STREAMS; i_stream++) {
        const uint32_t size = stream_size[i_stream];
        init_bit_stream(&stream[i_stream], input, size);
        input += size;
    }

    do {
        /* Decode packet type */
        uint32_t data = get_one_bit(&stream[LZS_TYPE]);

        if (data) {
            uint32_t distance;
            uint32_t length;
            uint32_t i;

            data = get_one_bit(&stream[LZS_TYPE]);

            /* *REP */
            if (data) {
                data = get_bits(&stream[LZS_TYPE], 2);

                /* LONGREP* */
                if (data) {
                    --data;
                    if (data > 1)
                        data += get_one_bit(&stream[LZS_TYPE]);

                    distance = last_dist[data];
                    length = decode_length(&stream[LZS_SIZE]);
                }
                /* SHORTREP */
                else {
                    distance = last_dist[0];
                    length = 1;
                }
            }
            /* MATCH */
            else {
                length   = decode_length(&stream[LZS_SIZE]);
                distance = decode_distance(&stream[LZS_OFFSET]);
            }

            /* Put distance on the list of last distances and deduplicate the list */
            for (i = 0; i < 3; ++i)
                if (last_dist[i] == distance)
                    break;
            for (; i; --i)
                last_dist[i] = last_dist[i - 1];
            last_dist[0] = distance;

            assert(dest + length <= end);
            assert(distance <= dest - begin);
            for (i = 0; i < length; ++i, ++dest)
                *dest = *(dest - distance);
        }
        /* LIT */
        else {
            uint8_t lit = (uint8_t)((get_one_bit(&stream[LZS_LITERAL_MSB]) << 7) ^ prev_lit) & 0x80U;

            lit += get_bits(&stream[LZS_LITERAL], 7);

            *(dest++) = lit;
            prev_lit  = lit;
        }
    } while (dest < end);
}

void lza_decompress(void       *input_dest,
                    size_t      dest_size,
                    size_t      scratch_size,
                    const void *compressed,
                    size_t      compressed_size)
{
    uint8_t *const dest  = (uint8_t *)input_dest;
    uint8_t *const input = (uint8_t *)dest + dest_size;
    uint32_t       window_size;

    assert(dest_size);
    assert(compressed_size > 2);

    window_size = *(uint16_t *)compressed;
    arith_decode(input, scratch_size, (const uint8_t *)compressed + 2, compressed_size - 2, window_size);

    lz_decompress(input_dest, dest_size, input);
}
