/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "find_repeats.h"
#include "load_file.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Arithmetic coding: https://github.com/runestubbe/Crinkler/blob/master/source/Compressor/AritCode.cpp
//
// LZMA
// 0 + byteCode	        LIT	        A single byte encoded using an adaptive binary range coder.
// 1+0 + len + dist	MATCH	        A typical LZ77 sequence describing sequence length and distance.
// 1+1+0+0	        SHORTREP	A one-byte LZ77 sequence. Distance is equal to the last used LZ77 distance.
// 1+1+0+1 + len	LONGREP[0]	An LZ77 sequence. Distance is equal to the last used LZ77 distance.
// 1+1+1+0 + len	LONGREP[1]	An LZ77 sequence. Distance is equal to the second last used LZ77 distance.
// 1+1+1+1+0 + len	LONGREP[2]	An LZ77 sequence. Distance is equal to the third last used LZ77 distance.
// 1+1+1+1+1 + len	LONGREP[3]	An LZ77 sequence. Distance is equal to the fourth last used LZ77 distance.
//
// Streams
// - unique/repeat (1 bit)
// - repeat reuse offset/size (2 bits)
// - unique lengths
// - unique data
// - repeat lengths
// - repeat offsets
//
// Statistically, length tends to be mostly below 256, so few bits are needed to encode it
//
// LZ77 length encoding:
// 0+ 3 bits	The length encoded using 3 bits, gives the lengths range from 2 to 9.
// 1+0+ 3 bits	The length encoded using 3 bits, gives the lengths range from 10 to 17.
// 1+1+ 8 bits	The length encoded using 8 bits, gives the lengths range from 18 to 273.
// 
// LZ77 distance encoding:
// 6-bit distance slot  Highest 2 bits  Fixed 0.5 prob bits     Context encoded bits
// 0	                00	        0	                0
// 1	                01	        0	                0
// 2	                10	        0	                0
// 3	                11	        0	                0
// 4	                10	        0	                1
// 5	                11	        0	                1
// 6	                10	        0	                2
// 7	                11	        0	                2
// 8	                10	        0	                3
// 9	                11	        0	                3
// 10	                10	        0	                4
// 11	                11	        0	                4
// 12	                10	        0	                5
// 13	                11	        0	                5
// 14–62 (even)	        10	        ((slot / 2) − 5)	4
// 15–63 (odd)	        11	        (((slot − 1) / 2) − 5)	4
// 

static size_t total_unique    = 0;
static size_t total_unique_16 = 0;
static size_t unique_seq      = 0;

static void report_unique_bytes(void *cookie, const char *buf, size_t pos, size_t size)
{
    char   sample[15];
    char  *out = sample;
    size_t i;

    if (size == 0)
        *out = 0;
    else {
        *(out++) = ' ';
        *(out++) = '-';
    }

    for (i = 0; i < size && i < 4; i++) {
        int          num_printed;
        const size_t num_left = &sample[sizeof(sample)] - out;
        assert(num_left > 3);
        assert(num_left < sizeof(sample));

        num_printed = snprintf(out, num_left, " %02x", (uint8_t)buf[pos + i]);
        out += num_printed;
    }

    ++unique_seq;
    total_unique += size;
    if (size < 16)
        ++total_unique_16;

    printf("@%zu unique %zu%s\n", pos, size, sample);
}

static size_t repeat_seq = 0;
static size_t offs_1     = 0;
static size_t offs_2     = 0;
static size_t offs_8     = 0;
static size_t offs_16    = 0;
static size_t offs_256   = 0;
static size_t offs_4096  = 0;
static size_t offs_large = 0;
static size_t size_2     = 0;
static size_t size_3     = 0;
static size_t size_8     = 0;
static size_t size_16    = 0;
static size_t size_256   = 0;
static size_t size_4096  = 0;
static size_t size_large = 0;
static size_t same_offs_size = 0;
static size_t same_size  = 0;
static size_t same_offs  = 0;

static void report_repeat(void *cookie, const char *buf, size_t pos, size_t offset, size_t size)
{
    ++repeat_seq;
    if (offset == 0)
        ++offs_1;
    else if (offset == 1)
        ++offs_2;
    else if (offset < 7)
        ++offs_8;
    else if (offset < 15)
        ++offs_16;
    else if (offset < 255)
        ++offs_256;
    else if (offset < 4095)
        ++offs_4096;
    else
        ++offs_large;
    if (size == 2)
        ++size_2;
    else if (size == 3)
        ++size_3;
    else if (size < 8)
        ++size_8;
    else if (size < 16)
        ++size_16;
    else if (size < 256)
        ++size_256;
    else if (size < 4096)
        ++size_4096;
    else
        ++size_large;

    static size_t last_offset = ~0U;
    static size_t last_size   = ~0U;

    if (size == last_size) {
        if (offset == last_offset)
            ++same_offs_size;
        else
            ++same_size;
    }
    else if (offset == last_offset)
        ++same_offs;
    last_offset = offset;
    last_size   = size;

    printf("@%zu repeat %zu offs %zu\n", pos, size, offset + 1);
}

int main(int argc, char *argv[])
{
    BUFFER buf;

    if (argc != 2) {
        fprintf(stderr, "Error: Invalid arguments\n");
        fprintf(stderr, "Usage: minify <FILE>\n");
        return EXIT_FAILURE;
    }

    buf = load_file(argv[1]);
    if ( ! buf.size)
        return EXIT_FAILURE;

    if (find_repeats(buf.buf, buf.size, report_unique_bytes, report_repeat, NULL))
        return EXIT_FAILURE;

    printf("file size                  %zu\n", buf.size);
    printf("total unique bytes         %zu\n", total_unique);
    printf("total unique bytes <16 len %zu\n", total_unique_16);
    printf("unique sequences           %zu\n", unique_seq);
    printf("repeat sequences           %zu\n", repeat_seq);
    printf("offset==1                  %zu\n", offs_1);
    printf("offset==2                  %zu\n", offs_2);
    printf("offset<8                   %zu\n", offs_8);
    printf("offset<16                  %zu\n", offs_16);
    printf("offset<256                 %zu\n", offs_256);
    printf("offset<4096                %zu\n", offs_4096);
    printf("offset>=4096               %zu\n", offs_large);
    printf("size==2                    %zu\n", size_2);
    printf("size==3                    %zu\n", size_3);
    printf("size<8                     %zu\n", size_8);
    printf("size<16                    %zu\n", size_16);
    printf("size<256                   %zu\n", size_256);
    printf("size<4096                  %zu\n", size_4096);
    printf("size>=4096                 %zu\n", size_large);
    printf("same repeat and offs       %zu\n", same_offs_size);
    printf("same repeat                %zu\n", same_size);
    printf("same offs                  %zu\n", same_offs);

    return EXIT_SUCCESS;
}
