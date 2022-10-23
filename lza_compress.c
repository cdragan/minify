/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_compress.h"

#include "arith_encode.h"
#include "find_repeats.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    uint8_t *begin;
    uint8_t *end;
    uint32_t data;
} EMITTER;

static void init_emitter(EMITTER *emitter, uint8_t *buf, size_t size)
{
    emitter->buf   = buf;
    emitter->begin = buf;
    emitter->end   = buf + size;
    emitter->data  = 0x100;
}

static void emit_bits(EMITTER *emitter, size_t value, int bits)
{
    assert(bits);

    do {
        const uint8_t bit = (uint8_t)((value >> --bits) & 1U);

        emitter->data = (emitter->data >> 1) | ((uint32_t)bit << 8);

        if (emitter->data & 1) {
            assert(emitter->buf < emitter->end);

            *(emitter->buf++) = (uint8_t)(emitter->data >> 1);
            emitter->data     = 0x100;
        }
    } while (bits);
}

static size_t emit_tail(EMITTER *emitter)
{
    /* Duplicate last bit */
    const uint32_t last_bit = ((emitter->data >> 8) & 1U) * 0x7FU;

    /* Emit 7 bits, which is enough to force out the last byte, but won't emit a byte unnecessarily */
    emit_bits(emitter, last_bit, 7);

    return (size_t)(emitter->buf - emitter->begin);
}

typedef struct {
    EMITTER          type_emitter;
    EMITTER          literal_emitter;
    EMITTER          size_emitter;
    EMITTER          offset_emitter;
    COMPRESSED_SIZES sizes;
} COMPRESS;

static void init_compress(COMPRESS *compress, void *buf, size_t size)
{
    memset(compress, 0, sizeof(*compress));

    init_emitter(&compress->type_emitter,    (uint8_t *)buf,                size / 4);
    init_emitter(&compress->literal_emitter, (uint8_t *)buf + size / 4,     size / 4);
    init_emitter(&compress->size_emitter,    (uint8_t *)buf + size / 2,     size / 4);
    init_emitter(&compress->offset_emitter , (uint8_t *)buf + 3 * size / 4, size / 4);
}

static void finish_compress(COMPRESS *compress)
{
    uint8_t *buf;

    compress->sizes.types    = emit_tail(&compress->type_emitter);
    compress->sizes.literals = emit_tail(&compress->literal_emitter);
    compress->sizes.sizes    = emit_tail(&compress->size_emitter);
    compress->sizes.offsets  = emit_tail(&compress->offset_emitter);

    buf = compress->type_emitter.begin;

    buf += compress->sizes.types;
    memmove(buf, compress->literal_emitter.begin, compress->sizes.literals);

    buf += compress->sizes.literals;
    memmove(buf, compress->size_emitter.begin, compress->sizes.sizes);

    buf += compress->sizes.sizes;
    memmove(buf, compress->offset_emitter.begin, compress->sizes.offsets);

    compress->sizes.total = compress->sizes.types +
                            compress->sizes.literals +
                            compress->sizes.sizes +
                            compress->sizes.offsets;
}

/* LZMA packets
 * 0 + byte             LIT         A single literal/original byte.
 * 1+0 + size + offset  MATCH       Repeated sequence with size and offset.
 * 1+1+0+0              SHORTREP    Repeated sequence, size=1, offset equal to the last used offset.
 * 1+1+0+1 + size       LONGREP[0]  Repeated sequence, offset is equal to the last used offset.
 * 1+1+1+0 + size       LONGREP[1]  Repeated sequence, offset is equal to the second last used offset.
 * 1+1+1+1+0 + size     LONGREP[2]  Repeated sequence, offset is equal to the third last used offset.
 * 1+1+1+1+1 + size     LONGREP[3]  Repeated sequence, offset is equal to the fourth last used offset.
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

    emit_bits(&compress->type_emitter, (size_t)type, type_size);
}

static void emit_size(EMITTER *emitter, size_t size)
{
    /* Statistically, size tends to be mostly below 256, so few bits are needed to encode it
     *
     * LZ77 length encoding:
     * 0+ 3 bits        Size encoded using 3 bits, gives the sizes range from 2 to 9.
     * 1+0+ 3 bits      Size encoded using 3 bits, gives the sizes range from 10 to 17.
     * 1+1+ 8 bits      Size encoded using 8 bits, gives the sizes range from 18 to 273.
     */

    assert(size >= 2);
    assert(size <= 273);

    /* Total 11795 packets.
     *
     * Packets with size:
     *
     * Size    Count
     *  2       4986
     *  3       2656
     *  4       1554
     */

    if (size <= 9) {
        emit_bits(emitter, 0, 1);
        emit_bits(emitter, size - 2, 3);
    }
    else if (size <= 17) {
        emit_bits(emitter, 2, 2);
        emit_bits(emitter, size - 10, 3);
    }
    else {
        emit_bits(emitter, 3, 2);
        emit_bits(emitter, size - 18, 8);
    }
}

static int count_trailing_bits(unsigned int value)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(value);
#elif defined(_MSC_VER)
    unsigned long bit;

    _BitScanReverse(&bit, value);

    return (int)bit;
#else
#error "Not implemented!"
#endif
}

static void emit_offset(EMITTER *emitter, size_t offset)
{
    /* LZ77 variable-length offset encoding:
     * - 6-bit offset slot
     * - Followed by a variable number of bits, depending on the value of the slot
     *
     * 6-bit offset slot  Highest 2 bits  Context encoded bits
     * 0                  00              0
     * 1                  01              0
     * 2–62 (even)        10              ((slot / 2) − 1)
     * 3–63 (odd)         11              (((slot − 1) / 2) − 1)
     *
     * Bits   6-bit offset slot   Context encoded bits
     * 2      00001x              0
     * 3      00010x              1
     * 4      00011x              2
     * 5      00100x              3
     * 6      00101x              4
     * :      :::                 :
     * 32     11111x              30
     */

    assert(offset > 0);

    --offset;

    if (offset < 2)
        emit_bits(emitter, offset, 6);
    else {
        const int bits_m1 = 31 - count_trailing_bits((unsigned int)offset);

        offset &= ~((size_t)1 << bits_m1);

        offset |= (uint32_t)bits_m1 << bits_m1;

        emit_bits(emitter, offset, bits_m1 + 5);
    }
}

static void emit_literal(EMITTER *emitter, const uint8_t *buf, size_t size)
{
    const uint8_t *const end = buf + size;

    do {
        emit_bits(emitter, *buf, 8);
    } while (++buf < end);
}

static void report_literal(void *cookie, const char *buf, size_t pos, size_t size)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    do {
        emit_type(compress, TYPE_LIT);
        emit_literal(&compress->literal_emitter, (const uint8_t *)&buf[pos], 1);
        ++pos;
        --size;
    } while (size);
}

static void report_match(void *cookie, const char *buf, size_t pos, OCCURRENCE occurrence)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    assert(occurrence.length <= 273);

    if (occurrence.last < 0) {

        assert(occurrence.length > 1);

        emit_type(compress, TYPE_MATCH);

        emit_size(&compress->size_emitter, occurrence.length);

        emit_offset(&compress->offset_emitter, occurrence.offset);
    }
    else if (occurrence.length == 1) {
        assert(occurrence.last == 3);

        emit_type(compress, TYPE_SHORTREP);
    }
    else {
        enum PACKET_TYPE type;

        switch (occurrence.last) {
            case 0: type = TYPE_LONGREP3; break;
            case 1: type = TYPE_LONGREP2; break;
            case 2: type = TYPE_LONGREP1; break;
            default:
                assert(occurrence.last == 3);
                type = TYPE_LONGREP0;
        }

        emit_type(compress, type);

        emit_size(&compress->size_emitter, occurrence.length);
    }
}

static size_t emit_header(uint8_t *dest, size_t dest_size, const COMPRESSED_SIZES *sizes)
{
    EMITTER emitter;

    init_emitter(&emitter, dest, dest_size);

    emit_offset(&emitter, sizes->types);
    emit_offset(&emitter, sizes->literals);
    emit_offset(&emitter, sizes->sizes);
    emit_offset(&emitter, sizes->offsets);

    return emit_tail(&emitter);
}

size_t estimate_compress_size(size_t src_size)
{
    if (src_size < 4096)
        src_size = 4096;

    return src_size * 4;
}

COMPRESSED_SIZES compress(void       *dest,
                          size_t      dest_size,
                          const void *src,
                          size_t      src_size,
                          uint32_t    window_size)
{
    COMPRESS       compress;
    size_t         hdr_size;
    const size_t   half_size    = dest_size / 2;
    uint8_t *const arith_input  = (uint8_t *)dest + half_size;
    uint8_t       *arith_output = (uint8_t *)dest;

    assert(half_size >= src_size);

    init_compress(&compress, dest, dest_size);

    if (find_repeats((const char *)src, src_size, report_literal, report_match, &compress)) {
        memset(&compress.sizes, 0, sizeof(compress.sizes));
        return compress.sizes;
    }

    finish_compress(&compress);
    assert(compress.sizes.total <= half_size);

    hdr_size = emit_header(arith_input, half_size, &compress.sizes);
    assert(hdr_size + compress.sizes.total <= half_size);

    memcpy(arith_input + hdr_size, dest, compress.sizes.total);
    compress.sizes.total += hdr_size;

    *(uint16_t *)arith_output = (uint16_t)window_size;

    compress.sizes.compressed = arith_encode(arith_output + 2,
                                             half_size - hdr_size,
                                             arith_input,
                                             compress.sizes.total,
                                             window_size) + 2;

    assert(compress.sizes.compressed <= half_size);

    return compress.sizes;
}
