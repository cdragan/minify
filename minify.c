/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "find_repeats.h"
#include "load_file.h"
#include "arith_encode.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char    *buf;
    char    *begin;
    char    *end;
    uint32_t data;
} EMITTER;

static void init_emitter(EMITTER *emitter, char *buf, size_t size)
{
    emitter->buf   = buf;
    emitter->begin = buf;
    emitter->end   = buf + size;
    emitter->data  = 1;
}

static void emit_bits(EMITTER *emitter, size_t value, int bits)
{
    assert(bits);

    do {
        const uint8_t bit = (uint8_t)((value >> --bits) & 1U);

        emitter->data = (emitter->data << 1) | bit;

        if (emitter->data > 0xFFU) {
            assert(emitter->buf < emitter->end);

            *(emitter->buf++) = (char)(uint8_t)emitter->data;
            emitter->data     = 1;
        }
    } while (bits);
}

static void emit_tail(EMITTER *emitter)
{
    /* Duplicate last bit */
    const uint32_t last_bit = (emitter->data & 1U) * 0x7FU;

    /* Emit 7 bits, which is enough to force out the last byte, but won't emit a byte unnecessarily */
    emit_bits(emitter, last_bit, 7);
}

typedef struct {
    EMITTER  type_emitter;
    EMITTER  unique_emitter;
    EMITTER  size_emitter;
    EMITTER  offset_emitter;
    unsigned stats_lit;
    unsigned stats_match;
    unsigned stats_shortrep;
    unsigned stats_longrep[4];
} COMPRESS;

static void init_compress(COMPRESS *compress, char *buf, size_t size)
{
    init_emitter(&compress->type_emitter,   buf,                size / 4);
    init_emitter(&compress->unique_emitter, buf + size / 4,     size / 4);
    init_emitter(&compress->size_emitter,   buf + size / 2,     size / 4);
    init_emitter(&compress->offset_emitter, buf + 3 * size / 4, size / 4);

    compress->stats_lit        = 0;
    compress->stats_match      = 0;
    compress->stats_shortrep   = 0;
    compress->stats_longrep[0] = 0;
    compress->stats_longrep[1] = 0;
    compress->stats_longrep[2] = 0;
    compress->stats_longrep[3] = 0;
}

static size_t finish_compress(COMPRESS *compress)
{
    char  *buf;
    size_t type_buf_size;
    size_t unique_buf_size;
    size_t size_buf_size;
    size_t offset_buf_size;

    emit_tail(&compress->type_emitter);
    emit_tail(&compress->unique_emitter);
    emit_tail(&compress->size_emitter);
    emit_tail(&compress->offset_emitter);

    type_buf_size   = (size_t)(compress->type_emitter.buf   - compress->type_emitter.begin);
    unique_buf_size = (size_t)(compress->unique_emitter.buf - compress->unique_emitter.begin);
    size_buf_size   = (size_t)(compress->size_emitter.buf   - compress->size_emitter.begin);
    offset_buf_size = (size_t)(compress->offset_emitter.buf - compress->offset_emitter.begin);

    buf = compress->type_emitter.begin;

    memmove(buf + type_buf_size, compress->unique_emitter.begin, unique_buf_size);
    memmove(buf + type_buf_size + unique_buf_size, compress->size_emitter.begin, size_buf_size);
    memmove(buf + type_buf_size + unique_buf_size + size_buf_size, compress->offset_emitter.begin, offset_buf_size);

    return type_buf_size + unique_buf_size + size_buf_size + offset_buf_size;
}

/* LZMA
 * 0 + byte             LIT         A single unique/original byte.
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
    size_t type_size = 1;

    switch (type) {
        case TYPE_MATCH:    type_size = 2; ++compress->stats_match;         break;
        case TYPE_SHORTREP: type_size = 4; ++compress->stats_shortrep;      break;
        case TYPE_LONGREP0: type_size = 4; ++compress->stats_longrep[0];    break;
        case TYPE_LONGREP1: type_size = 4; ++compress->stats_longrep[1];    break;
        case TYPE_LONGREP2: type_size = 5; ++compress->stats_longrep[2];    break;
        case TYPE_LONGREP3: type_size = 5; ++compress->stats_longrep[3];    break;
        default:                           ++compress->stats_lit;           break;
    }

    emit_bits(&compress->type_emitter, (size_t)type, type_size);
}

static void emit_size(COMPRESS *compress, size_t size)
{
    /* Statistically, size tends to be mostly below 256, so few bits are needed to encode it
     *
     * LZ77 length encoding:
     * 0+ 3 bits        Size encoded using 3 bits, gives the sizes range from 2 to 9.
     * 1+0+ 3 bits      Size encoded using 3 bits, gives the sizes range from 10 to 17.
     * 1+1+ 8 bits      Size encoded using 8 bits, gives the sizes range from 18 to 273.
     */

    EMITTER *const emitter = &compress->size_emitter;

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

static void emit_offset(COMPRESS *compress, size_t offset)
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

    EMITTER *const emitter = &compress->offset_emitter;

    assert((sizeof(offset) <= 4) || (offset <= ~0U));

    --offset;

    if (offset < 2)
        emit_bits(emitter, offset, 6);
    else {
        const int bits_m1 = 31 - count_trailing_bits((unsigned int)offset);

        offset &= ~((size_t)1 << bits_m1);

        offset |= bits_m1 << bits_m1;

        emit_bits(emitter, offset, bits_m1 + 5);
    }
}

static void emit_unique_bytes(COMPRESS *compress, const uint8_t *buf, size_t size)
{
    const uint8_t *const end = buf + size;

    EMITTER *const emitter = &compress->unique_emitter;

    do {
        emit_bits(emitter, *buf, 8);
    } while (++buf < end);
}

static void report_unique_bytes(void *cookie, const char *buf, size_t pos, size_t size)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    do {
        emit_type(compress, TYPE_LIT);
        emit_unique_bytes(compress, (const uint8_t *)&buf[pos], 1);
        ++pos;
        --size;
    } while (size);
}

static void report_repeat(void *cookie, const char *buf, size_t pos, OCCURRENCE occurrence)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    assert(occurrence.length <= 273);

    if (occurrence.last < 0) {

        assert(occurrence.length > 1);

        emit_type(compress, TYPE_MATCH);

        emit_size(compress, occurrence.length);

        emit_offset(compress, occurrence.offset);
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

        emit_size(compress, occurrence.length);
    }
}

int main(int argc, char *argv[])
{
    COMPRESS compress;
    BUFFER   buf;
    char    *dest;
    char    *entropy;
    size_t   dest_size;
    size_t   entropy_size;

    if (argc != 2) {
        fprintf(stderr, "Error: Invalid arguments\n");
        fprintf(stderr, "Usage: minify <FILE>\n");
        return EXIT_FAILURE;
    }

    buf = load_file(argv[1]);
    if ( ! buf.size)
        return EXIT_FAILURE;

    dest_size = buf.size * 510 / 100;
    dest = (char *)malloc(dest_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    entropy = dest + buf.size * 110 / 100;
    dest_size = buf.size * 110 / 100;

    init_compress(&compress, dest, dest_size);

    if (find_repeats(buf.buf, buf.size, report_unique_bytes, report_repeat, &compress))
        return EXIT_FAILURE;

    dest_size = finish_compress(&compress);

    entropy_size = arith_encode(entropy, buf.size * 400 / 100, dest, dest_size, 128);

    printf("Original        %zu bytes\n", buf.size);
    printf("LZMA            %zu bytes\n", dest_size);
    printf("Entropy (split) %zu bytes\n", entropy_size);

    printf("LIT             %u\n", compress.stats_lit);
    printf("MATCH           %u\n", compress.stats_match);
    printf("SHORTREP        %u\n", compress.stats_shortrep);
    printf("LONGREP0        %u\n", compress.stats_longrep[0]);
    printf("LONGREP1        %u\n", compress.stats_longrep[1]);
    printf("LONGREP2        %u\n", compress.stats_longrep[2]);
    printf("LONGREP3        %u\n", compress.stats_longrep[3]);

    return EXIT_SUCCESS;
}
