/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_compress.h"

#include "arith_encode.h"
#include "bit_emit.h"
#include "bit_ops.h"
#include "find_repeats.h"
#include "lza_stream.h"

#include <assert.h>
#include <string.h>

typedef struct {
    BIT_EMITTER      emitter[LZS_NUM_STREAMS];
    COMPRESSED_SIZES sizes;
    uint8_t          prev_lit;
} COMPRESS;

static void init_compress(COMPRESS *compress, void *buf, size_t size)
{
    uint8_t     *dest       = (uint8_t *)buf;
    const size_t chunk_size = size / 4;
    uint32_t     i;

    memset(compress, 0, sizeof(*compress));

    for (i = 0; i < LZS_NUM_STREAMS; i++) {
        init_bit_emitter(&compress->emitter[i], dest, chunk_size);
        dest += chunk_size;
    }
}

static void finish_compress(COMPRESS *compress, size_t stream_sizes[])
{
    uint8_t *buf;
    uint32_t i;
    size_t   total = 0;

    for (i = 0; i < LZS_NUM_STREAMS; i++) {
        const size_t stream_size = emit_tail(&compress->emitter[i]);
        stream_sizes[i]          = stream_size;
        total                   += stream_size;
    }

    buf = compress->emitter[0].begin;

    for (i = 1; i < LZS_NUM_STREAMS; i++) {
        buf += stream_sizes[i - 1];
        memmove(buf, compress->emitter[i].begin, stream_sizes[i]);
    }

    compress->sizes.lz = total;
}

/* LZMA packets
 * 0 + byte                 LIT         A single literal/original byte.
 * 1+0 + length + distance  MATCH       Repeated sequence with length and distance.
 * 1+1+0+0                  SHORTREP    Repeated sequence, length=1, distance equal to the last used distance.
 * 1+1+0+1 + length         LONGREP[0]  Repeated sequence, distance is equal to the last used distance.
 * 1+1+1+0 + length         LONGREP[1]  Repeated sequence, distance is equal to the second last used distance.
 * 1+1+1+1+0 + length       LONGREP[2]  Repeated sequence, distance is equal to the third last used distance.
 * 1+1+1+1+1 + length       LONGREP[3]  Repeated sequence, distance is equal to the fourth last used distance.
 */

enum PACKET_TYPE {
    TYPE_LIT      = 0,      /* 0 */
    TYPE_MATCH    = 2,      /* 10 */
    TYPE_SHORTREP = 0xC,    /* 1100 */
    TYPE_LONGREP0 = 0xD,    /* 1101 */
    TYPE_LONGREP1 = 0xE,    /* 1110 */
    TYPE_LONGREP2 = 0x1E,   /* 11110 */
    TYPE_LONGREP3 = 0x1F    /* 11111 */
};

static void emit_type(COMPRESS *compress, enum PACKET_TYPE type)
{
    int type_size = 1;

    switch (type) {
        case TYPE_MATCH:    type_size = 2; ++compress->sizes.stats_match;         break;
        case TYPE_SHORTREP: type_size = 4; ++compress->sizes.stats_shortrep;      break;
        case TYPE_LONGREP0: type_size = 4; ++compress->sizes.stats_longrep[0];    break;
        case TYPE_LONGREP1: type_size = 4; ++compress->sizes.stats_longrep[1];    break;
        case TYPE_LONGREP2: type_size = 5; ++compress->sizes.stats_longrep[2];    break;
        case TYPE_LONGREP3: type_size = 5; ++compress->sizes.stats_longrep[3];    break;
        default:                           ++compress->sizes.stats_lit;           break;
    }

    emit_bits(&compress->emitter[LZS_TYPE], (size_t)type, type_size);
}

static void emit_length(BIT_EMITTER *emitter, size_t length)
{
    /* Statistically, length tends to be mostly below 256, so few bits are needed to encode it
     *
     * LZ77 length encoding:
     * 0+ 3 bits        Size encoded using 3 bits, gives the sizes range from 2 to 9.
     * 1+0+ 3 bits      Size encoded using 3 bits, gives the sizes range from 10 to 17.
     * 1+1+ 8 bits      Size encoded using 8 bits, gives the sizes range from 18 to 273.
     */

    assert(length >= 2);
    assert(length <= 273);

    if (length <= 9) {
        emit_bits(emitter, 0, 1);
        emit_bits(emitter, length - 2, 3);
    }
    else if (length <= 17) {
        emit_bits(emitter, 2, 2);
        emit_bits(emitter, length - 10, 3);
    }
    else {
        emit_bits(emitter, 3, 2);
        emit_bits(emitter, length - 18, 8);
    }
}

static void emit_distance(BIT_EMITTER *emitter, size_t distance)
{
    /* LZ77 variable-length distance encoding:
     * - 6-bit distance slot
     * - Followed by a variable number of bits, depending on the value of the slot
     *
     * 6-bit distance slot  Highest 2 bits  Context encoded bits
     * 0                    00              0
     * 1                    01              0
     * 2–62 (even)          10              ((slot / 2) − 1)
     * 3–63 (odd)           11              (((slot − 1) / 2) − 1)
     *
     * Bits   6-bit distance slot   Context encoded bits
     * 2      00001x                0
     * 3      00010x                1
     * 4      00011x                2
     * 5      00100x                3
     * 6      00101x                4
     * :      :::                   :
     * 32     11111x                30
     */

    assert(distance > 0);

    --distance;

    if (distance < 2)
        emit_bits(emitter, distance, 6);
    else {
        const int bits_m1 = 31 - count_leading_zeroes((unsigned int)distance);

        distance &= ~((size_t)1 << bits_m1);

        distance |= (uint32_t)bits_m1 << bits_m1;

        emit_bits(emitter, distance, bits_m1 + 5);
    }
}

static void emit_literal(COMPRESS *compress, const uint8_t *buf, size_t size)
{
    const uint8_t *const end = buf + size;

    do {
        const uint8_t lit = *buf;

        emit_bits(&compress->emitter[LZS_LITERAL_MSB], (lit ^ compress->prev_lit) >> 7, 1);

        compress->prev_lit = lit;

        emit_bits(&compress->emitter[LZS_LITERAL], lit, 7);
    } while (++buf < end);
}

static void report_literal(void *cookie, const uint8_t *buf, size_t pos, size_t size)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    do {
        emit_type(compress, TYPE_LIT);
        emit_literal(compress, &buf[pos], 1);
        ++pos;
        --size;
    } while (size);
}

static void report_match(void *cookie, const uint8_t *buf, size_t pos, OCCURRENCE occurrence)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    assert(occurrence.length <= 273);

    if (occurrence.last < 0) {

        assert(occurrence.length > 1);

        emit_type(compress, TYPE_MATCH);

        emit_length(&compress->emitter[LZS_SIZE], occurrence.length);

        emit_distance(&compress->emitter[LZS_OFFSET], occurrence.distance);
    }
    else if (occurrence.length == 1) {
        assert(occurrence.last == 0);

        emit_type(compress, TYPE_SHORTREP);
    }
    else {
        enum PACKET_TYPE type;

        switch (occurrence.last) {
            case 3: type = TYPE_LONGREP3; break;
            case 2: type = TYPE_LONGREP2; break;
            case 1: type = TYPE_LONGREP1; break;
            default:
                assert(occurrence.last == 0);
                type = TYPE_LONGREP0;
        }

        emit_type(compress, type);

        emit_length(&compress->emitter[LZS_SIZE], occurrence.length);
    }
}

static size_t emit_header(uint8_t *dest, size_t dest_size, const size_t stream_sizes[])
{
    BIT_EMITTER emitter;
    uint32_t    i;

    init_bit_emitter(&emitter, dest, dest_size);

    for (i = 0; i < LZS_NUM_STREAMS; i++)
        emit_distance(&emitter, stream_sizes[i]);

    return emit_tail(&emitter);
}

size_t estimate_compress_size(size_t src_size)
{
    if (src_size < 4096)
        src_size = 4096;

    return src_size * 4;
}

COMPRESSED_SIZES lza_compress(void       *dest,
                              size_t      dest_size,
                              const void *src,
                              size_t      src_size,
                              unsigned    window_size)
{
    COMPRESS       compress;
    size_t         hdr_size;
    size_t         stream_sizes[LZS_NUM_STREAMS];
    const size_t   half_size    = dest_size / 2;
    uint8_t *const arith_input  = (uint8_t *)dest + half_size;
    uint8_t       *arith_output = (uint8_t *)dest;

    assert(half_size >= src_size);

    init_compress(&compress, dest, dest_size);

    if (find_repeats((const uint8_t *)src, src_size, report_literal, report_match, &compress)) {
        memset(&compress.sizes, 0, sizeof(compress.sizes));
        return compress.sizes;
    }

    finish_compress(&compress, stream_sizes);
    assert(compress.sizes.lz <= half_size);

    hdr_size = emit_header(arith_input, half_size, stream_sizes);
    assert(hdr_size + compress.sizes.lz <= half_size);

    memcpy(arith_input + hdr_size, dest, compress.sizes.lz);
    compress.sizes.lz += hdr_size;

    *(uint16_t *)arith_output = (uint16_t)window_size;

    compress.sizes.compressed = arith_encode(arith_output + 2,
                                             half_size - hdr_size,
                                             arith_input,
                                             compress.sizes.lz,
                                             window_size) + 2;

    assert(compress.sizes.compressed <= half_size);

    return compress.sizes;
}
