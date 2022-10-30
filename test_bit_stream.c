/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "bit_emit.h"
#include "bit_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(expr) do { if ( ! (expr)) { report_error(#expr, __LINE__); ++num_failed; } } while (0)

static void report_error(const char *desc, int line)
{
    fprintf(stderr, "test_bit_stream.c:%d: failed test: %s\n",
            line, desc);
}

int main(void)
{
    unsigned num_failed = 0;

    {
        BIT_STREAM    stream;
        const uint8_t bits[] = { 0x80U, 0x01U, 0x40U };

        init_bit_stream(&stream, bits, sizeof(bits));

        TEST(get_one_bit(&stream) == 1);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);

        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 1);

        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 1);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);

        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
    }

    {
        BIT_STREAM    stream;
        const uint8_t bits[] = { 0x01U };

        init_bit_stream(&stream, bits, sizeof(bits));

        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 0);
        TEST(get_one_bit(&stream) == 1);

        TEST(get_one_bit(&stream) == 1);
        TEST(get_one_bit(&stream) == 1);
        TEST(get_one_bit(&stream) == 1);
        TEST(get_one_bit(&stream) == 1);
    }

    {
        BIT_STREAM    stream;
        const uint8_t bits[] = { 0x80U, 0x01U, 0x40U };

        init_bit_stream(&stream, bits, sizeof(bits));

        TEST(get_bits(&stream, 8) == 0x80U);
        TEST(get_bits(&stream, 8) == 0x01U);
        TEST(get_bits(&stream, 8) == 0x40U);
    }

    {
        BIT_EMITTER emitter;
        uint8_t     bits[] = { 0, 0, 0 };

        init_bit_emitter(&emitter, bits, sizeof(bits));

        emit_bit(&emitter, 1);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);

        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 1);

        emit_bit(&emitter, 0);
        emit_bit(&emitter, 1);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);
        emit_bit(&emitter, 0);

        TEST(bits[0] == 0x80U);
        TEST(bits[1] == 0x01U);
        TEST(bits[2] == 0x40U);
    }

    {
        BIT_EMITTER emitter;
        uint8_t     bits[] = { 0, 0, 0 };

        init_bit_emitter(&emitter, bits, sizeof(bits));

        emit_bits(&emitter, 0x800140U, 24);

        emit_tail(&emitter);

        TEST(bits[0] == 0x80U);
        TEST(bits[1] == 0x01U);
        TEST(bits[2] == 0x40U);
    }

    return num_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
