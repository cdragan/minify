/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_decompress.h"

#include "arith_decode.h"
#include "bit_stream.h"

#include <assert.h>
#include <stdint.h>

static uint32_t decode_size(BIT_STREAM *stream)
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

static uint32_t decode_offset(BIT_STREAM *stream)
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
    BIT_STREAM     type_stream;
    BIT_STREAM     literal_stream;
    BIT_STREAM     size_stream;
    BIT_STREAM     offset_stream;
    uint32_t       type_buf_size;
    uint32_t       literal_buf_size;
    uint32_t       size_buf_size;
    uint32_t       offset_buf_size;
    uint32_t       last_offs[4] = { 0, 0, 0, 0 };
    uint8_t       *dest         = (uint8_t *)input_dest;
#ifndef NDEBUG
    uint8_t *const begin        = dest;
#endif
    uint8_t *const end          = dest + dest_size;
    const uint8_t *input        = (const uint8_t *)input_src;

    assert(dest_size);

    init_bit_stream(&type_stream, input, dest_size);
    type_buf_size    = decode_offset(&type_stream);
    literal_buf_size = decode_offset(&type_stream);
    size_buf_size    = decode_offset(&type_stream);
    offset_buf_size  = decode_offset(&type_stream);

    input = type_stream.buf;
    init_bit_stream(&type_stream,    input, type_buf_size);
    input += type_buf_size;
    init_bit_stream(&literal_stream, input, literal_buf_size);
    input += literal_buf_size;
    init_bit_stream(&size_stream,    input, size_buf_size);
    input += size_buf_size;
    init_bit_stream(&offset_stream,  input, offset_buf_size);

    do {
        uint32_t data = get_one_bit(&type_stream);

        if (data) {
            uint32_t offset;
            uint32_t length;
            uint32_t i;

            data = get_one_bit(&type_stream);

            /* *REP */
            if (data) {
                data = get_bits(&type_stream, 2);

                /* LONGREP* */
                if (data) {
                    --data;
                    if (data > 1)
                        data += get_one_bit(&type_stream);

                    offset = last_offs[data];
                    length = decode_size(&size_stream);
                }
                /* SHORTREP */
                else {
                    offset = last_offs[0];
                    length = 1;
                }
            }
            /* MATCH */
            else {
                length = decode_size(&size_stream);
                offset = decode_offset(&offset_stream);
            }

            /* Put offset on the list of last offsets and deduplicate the list */
            for (i = 0; i < 3; ++i)
                if (last_offs[i] == offset)
                    break;
            for (; i; --i)
                last_offs[i] = last_offs[i - 1];
            last_offs[0] = offset;

            assert(dest + length <= end);
            assert(offset <= dest - begin);
            for (i = 0; i < length; ++i, ++dest)
                *dest = *(dest - offset);
        }
        /* LIT */
        else
            *(dest++) = (uint8_t)get_bits(&literal_stream, 8);
    } while (dest < end);
}

void decompress(void       *input_dest,
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
