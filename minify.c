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
    emitter->data  = 0x100;
}

static void emit_bits(EMITTER *emitter, size_t value, int bits)
{
    assert(bits);

    do {
        const uint8_t bit = (uint8_t)((value >> --bits) & 1U);

        emitter->data = (emitter->data >> 1) | (bit << 8);

        if (emitter->data & 1) {
            assert(emitter->buf < emitter->end);

            *(emitter->buf++) = (char)(uint8_t)(emitter->data >> 1);
            emitter->data     = 0x100;
        }
    } while (bits);
}

static void emit_tail(EMITTER *emitter)
{
    /* Duplicate last bit */
    const uint32_t last_bit = ((emitter->data >> 8) & 1U) * 0x7FU;

    /* Emit 7 bits, which is enough to force out the last byte, but won't emit a byte unnecessarily */
    emit_bits(emitter, last_bit, 7);
}

typedef struct {
    EMITTER  type_emitter;
    EMITTER  literal_emitter;
    EMITTER  size_emitter;
    EMITTER  offset_emitter;

    size_t   type_buf_size;
    size_t   literal_buf_size;
    size_t   size_buf_size;
    size_t   offset_buf_size;

    unsigned stats_lit;
    unsigned stats_match;
    unsigned stats_shortrep;
    unsigned stats_longrep[4];
} COMPRESS;

static void init_compress(COMPRESS *compress, char *buf, size_t size)
{
    memset(compress, 0, sizeof(*compress));

    init_emitter(&compress->type_emitter,    buf,                size / 4);
    init_emitter(&compress->literal_emitter, buf + size / 4,     size / 4);
    init_emitter(&compress->size_emitter,    buf + size / 2,     size / 4);
    init_emitter(&compress->offset_emitter , buf + 3 * size / 4, size / 4);
}

static size_t finish_compress(COMPRESS *compress)
{
    char *buf;

    emit_tail(&compress->type_emitter);
    emit_tail(&compress->literal_emitter);
    emit_tail(&compress->size_emitter);
    emit_tail(&compress->offset_emitter);

    compress->type_buf_size    = (size_t)(compress->type_emitter.buf    - compress->type_emitter.begin);
    compress->literal_buf_size = (size_t)(compress->literal_emitter.buf - compress->literal_emitter.begin);
    compress->size_buf_size    = (size_t)(compress->size_emitter.buf    - compress->size_emitter.begin);
    compress->offset_buf_size  = (size_t)(compress->offset_emitter.buf  - compress->offset_emitter.begin);

    buf = compress->type_emitter.begin;

    memmove(buf + compress->type_buf_size,
            compress->literal_emitter.begin, compress->literal_buf_size);
    memmove(buf + compress->type_buf_size + compress->literal_buf_size,
            compress->size_emitter.begin, compress->size_buf_size);
    memmove(buf + compress->type_buf_size + compress->literal_buf_size + compress->size_buf_size,
            compress->offset_emitter.begin, compress->offset_buf_size);

    return compress->type_buf_size +
           compress->literal_buf_size +
           compress->size_buf_size +
           compress->offset_buf_size;
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
    assert(offset > 0);

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

static void emit_literal(COMPRESS *compress, const uint8_t *buf, size_t size)
{
    const uint8_t *const end = buf + size;

    EMITTER *const emitter = &compress->literal_emitter;

    do {
        emit_bits(emitter, *buf, 8);
    } while (++buf < end);
}

static void report_literal(void *cookie, const char *buf, size_t pos, size_t size)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    do {
        emit_type(compress, TYPE_LIT);
        emit_literal(compress, (const uint8_t *)&buf[pos], 1);
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

typedef struct {
    const char *buf;
    const char *end;
    uint32_t    data;
} BIT_STREAM;

static void init_bit_stream(BIT_STREAM *stream, const char *buf, size_t size)
{
    stream->buf  = buf;
    stream->end  = buf + size;
    stream->data = 1;
}

static uint32_t get_bits(BIT_STREAM *stream, int bits)
{
    uint32_t value = 0;
    uint32_t data  = stream->data;

    while (bits) {
        if (data == 1) {
            assert(stream->buf < stream->end);
            data = 0x100 + (uint8_t)*(stream->buf++);
        }

        value = (value << 1) | (data & 1);
        data >>= 1;
        --bits;
    }

    stream->data = data;

    return value;
}

static uint32_t get_one_bit(BIT_STREAM *stream)
{
    return get_bits(stream, 1);
}

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

    return (((data & 1) + 2) << bits) + get_bits(stream, bits) + 1;
}

static void decompress(char       *dest,
                       size_t      dest_size,
                       const char *compressed,
                       size_t      type_buf_size,
                       size_t      literal_buf_size,
                       size_t      size_buf_size,
                       size_t      offset_buf_size)
{
    BIT_STREAM  type_stream;
    BIT_STREAM  literal_stream;
    BIT_STREAM  size_stream;
    BIT_STREAM  offset_stream;
    uint32_t    last_offs[4] = { 0, 0, 0, 0 };
    char *const begin        = dest;
    char *const end          = dest + dest_size;

    assert(dest_size);

    init_bit_stream(&type_stream,    compressed,                                                    type_buf_size);
    init_bit_stream(&literal_stream, compressed + type_buf_size,                                    literal_buf_size);
    init_bit_stream(&size_stream,    compressed + type_buf_size + literal_buf_size,                 size_buf_size);
    init_bit_stream(&offset_stream,  compressed + type_buf_size + literal_buf_size + size_buf_size, offset_buf_size);

    do {
        uint32_t data = get_one_bit(&type_stream);

        if (data) {
            uint32_t offset;
            uint32_t length;
            uint32_t i;
            int had_match = 0;
            int had_shortrep = 0;
            int had_longrep = -1;

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
                    had_longrep = data;
                }
                /* SHORTREP */
                else {
                    offset = last_offs[0];
                    length = 1;
                    had_shortrep = 1;
                }
            }
            /* MATCH */
            else {
                had_match = 1;
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
            *(dest++) = get_bits(&literal_stream, 8);
    } while (dest < end);
}

int main(int argc, char *argv[])
{
    COMPRESS compress;
    BUFFER   buf;
    char    *dest;
    char    *decompressed;
    char    *entropy;
    size_t   alloc_size;
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

    dest_size  = (buf.size < 4096 ? 4096 : buf.size) * 110 / 100;
    alloc_size = dest_size + buf.size * (400 + 100) / 100;
    dest = (char *)malloc(alloc_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    entropy      = dest + dest_size;
    decompressed = dest + dest_size + buf.size * 400 / 100;

    init_compress(&compress, dest, dest_size);

    if (find_repeats(buf.buf, buf.size, report_literal, report_match, &compress))
        return EXIT_FAILURE;

    dest_size = finish_compress(&compress);

    decompress(decompressed,
               buf.size,
               dest,
               compress.type_buf_size,
               compress.literal_buf_size,
               compress.size_buf_size,
               compress.offset_buf_size);

    if (memcmp(buf.buf, decompressed, buf.size)) {
        fprintf(stderr, "Decompressed output doesn't match input data\n");
        return EXIT_FAILURE;
    }

    entropy_size = arith_encode(entropy, buf.size * 400 / 100, dest, dest_size, 128);

    printf("Original    %zu bytes\n", buf.size);
    printf("LZMA        %zu bytes\n", dest_size);
    printf("Entropy     %zu bytes (%zu%%)\n", entropy_size, entropy_size * 100 / buf.size);

    printf("LIT         %u\n", compress.stats_lit);
    printf("MATCH       %u\n", compress.stats_match);
    printf("SHORTREP    %u\n", compress.stats_shortrep);
    printf("LONGREP0    %u\n", compress.stats_longrep[0]);
    printf("LONGREP1    %u\n", compress.stats_longrep[1]);
    printf("LONGREP2    %u\n", compress.stats_longrep[2]);
    printf("LONGREP3    %u\n", compress.stats_longrep[3]);

    free(dest);
    free(buf.buf);

    return EXIT_SUCCESS;
}
