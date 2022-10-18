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

// LZMA
// 0 + byteCode	        LIT	        A single byte encoded using an adaptive binary range coder.
// 1+0 + len + dist	MATCH	        A typical LZ77 sequence describing sequence length and distance.
// 1+1+0+0	        SHORTREP	A one-byte LZ77 sequence. Distance is equal to the last used LZ77 distance.
// 1+1+0+1 + len	LONGREP[0]	An LZ77 sequence. Distance is equal to the last used LZ77 distance.
// 1+1+1+0 + len	LONGREP[1]	An LZ77 sequence. Distance is equal to the second last used LZ77 distance.
// 1+1+1+1+0 + len	LONGREP[2]	An LZ77 sequence. Distance is equal to the third last used LZ77 distance.
// 1+1+1+1+1 + len	LONGREP[3]	An LZ77 sequence. Distance is equal to the fourth last used LZ77 distance.

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
    EMITTER emitter;
    size_t  last_size;
} COMPRESS;

static void init_compress(COMPRESS *compress, char *buf, size_t size)
{
    init_emitter(&compress->emitter, buf, size);
    compress->last_size = 0;
}

static size_t finish_compress(COMPRESS *compress)
{
    emit_tail(&compress->emitter);

    return (size_t)(compress->emitter.buf - compress->emitter.begin);
}

enum PACKET_TYPE {
    TYPE_UNIQUE           = 0, /* 0 */
    TYPE_REPEAT           = 2, /* 10 */
    TYPE_REPEAT_SAME_SIZE = 3  /* 11 */
};

static void emit_type(COMPRESS *compress, enum PACKET_TYPE type)
{
    emit_bits(&compress->emitter, (size_t)type, (type == TYPE_UNIQUE) ? 1 : 2);
}

static void emit_size(COMPRESS *compress, size_t size)
{
    // Statistically, length tends to be mostly below 256, so few bits are needed to encode it
    //
    // LZ77 length encoding:
    // 0+ 3 bits	The length encoded using 3 bits, gives the lengths range from 2 to 9.
    // 1+0+ 3 bits	The length encoded using 3 bits, gives the lengths range from 10 to 17.
    // 1+1+ 8 bits	The length encoded using 8 bits, gives the lengths range from 18 to 273.

    assert(size >= 2);
    assert(size <= 273);

    if (size <= 9) {
        emit_bits(&compress->emitter, 0, 1);
        emit_bits(&compress->emitter, size - 2, 3);
    }
    else if (size <= 17) {
        emit_bits(&compress->emitter, 2, 2);
        emit_bits(&compress->emitter, size - 10, 3);
    }
    else {
        emit_bits(&compress->emitter, 3, 2);
        emit_bits(&compress->emitter, size - 18, 8);
    }
}

static int count_bits(unsigned int value)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(value);
#elif defined(_MSC_VER)
    unsigned long bit;

    _BitScanReverse(&bit, value);

    return (int)bit;
#endif
}

static void emit_offset(COMPRESS *compress, size_t offset)
{
    // LZ77 distance encoding:
    // * 6-bit distance slot
    // * Followed by a variable number of bits, depending on the value of the slot
    //
    // 6-bit distance slot  Highest 2 bits  Context encoded bits
    // 0	            00	            0
    // 1	            01	            0
    // 2–62 (even)	    10	            ((slot / 2) − 1)
    // 3–63 (odd)	    11	            (((slot − 1) / 2) − 1)
    //
    // Bits   6-bit distance slot   Context encoded bits
    // 2      00001x                0
    // 3      00010x                1
    // 4      00011x                2
    // 5      00100x                3
    // 6      00101x                4
    // :      :::                   :
    // 32     11111x                30

    assert((sizeof(offset) <= 4) || (offset <= ~0U));

    if (offset < 2)
        emit_bits(&compress->emitter, offset, 6);
    else {
        const int bits_m1 = 31 - count_bits((unsigned int)offset);

        offset &= ~((size_t)1 << bits_m1);

        offset |= bits_m1 << bits_m1;

        emit_bits(&compress->emitter, offset, bits_m1 + 5);
    }
}

static void emit_unique_bytes(COMPRESS *compress, const uint8_t *buf, size_t size)
{
    const uint8_t *const end = buf + size;

    do {
        emit_bits(&compress->emitter, *buf, 8);
    } while (++buf < end);
}

static void report_unique_bytes(void *cookie, const char *buf, size_t pos, size_t size)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    do {
        const size_t chunk_size = size > 272 ? 272 : size;

        emit_type(compress, TYPE_UNIQUE);
        emit_size(compress, chunk_size + 1);
        emit_unique_bytes(compress, (const uint8_t *)&buf[pos], chunk_size);

        size -= chunk_size;
    } while (size);
}

static void report_repeat(void *cookie, const char *buf, size_t pos, size_t offset, size_t size)
{
    COMPRESS *const compress = (COMPRESS *)cookie;

    do {
        const size_t chunk_size = size > 273 ? 273 : size;

        emit_type(compress, (chunk_size == compress->last_size) ? TYPE_REPEAT_SAME_SIZE : TYPE_REPEAT);

        if (chunk_size != compress->last_size)
            emit_size(compress, chunk_size);
        compress->last_size = chunk_size;

        emit_offset(compress, offset);

        size -= chunk_size;
    } while (size);
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

    dest_size = buf.size * 231 / 100; /* +10% and then +10% on top of it */
    dest = (char *)malloc(dest_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    entropy = dest + buf.size * 110 / 100;
    dest_size = buf.size * 110 / 100; /* +10% */

    init_compress(&compress, dest, dest_size);

    if (find_repeats(buf.buf, buf.size, report_unique_bytes, report_repeat, &compress))
        return EXIT_FAILURE;

    dest_size = finish_compress(&compress);

    entropy_size = arith_encode(entropy, buf.size * 121 / 100, dest, dest_size, 256);

    printf("Original: %zu bytes\n", buf.size);
    printf("LZ77:     %zu bytes\n", dest_size);
    printf("Entropy:  %zu bytes\n", entropy_size);

    return EXIT_SUCCESS;
}
