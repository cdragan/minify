/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_decode.h"
#include "arith_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(expr) do { if ( ! (expr)) { report_error(#expr, __LINE__); ++num_failed; } } while (0)

static void report_error(const char *desc, int line)
{
    fprintf(stderr, "test_arith_encode.c:%d: failed test: %s\n",
            line, desc);
}

static uint32_t lcg(uint32_t *state)
{
    const uint32_t prev_state = *state;
    const uint32_t value      = prev_state & 0x7FFFFFFFU;

    *state = prev_state * 0x8088406U + 1U;

    return value;
}

int main(void)
{
    unsigned num_failed = 0;

    {
        const size_t out_size = arith_encode(NULL, 0, NULL, 0, 256);
        TEST(out_size == 0);
    }

    {
        const uint8_t input    = 0;
        uint8_t       output   = 0xAA;
        uint8_t       decoded  = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0x0F);

        arith_decode(&decoded, 1, &output, 1, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input    = 0xFF;
        uint8_t       output   = 0xAA;
        uint8_t       decoded  = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0xEF);

        arith_decode(&decoded, 1, &output, 1, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input     = 0x0F;
        uint8_t       output[2] = { 0xAA, 0xAA };
        uint8_t       decoded   = 0xAA;
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x32);
        TEST(output[1] == 0xFF);

        arith_decode(&decoded, 1, output, 2, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input     = 0xF0;
        uint8_t       output[2] = { 0xAA, 0xAA };
        uint8_t       decoded   = 0xAA;
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0xCC);
        TEST(output[1] == 0xFF);

        arith_decode(&decoded, 1, output, 2, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input     = 0x55;
        uint8_t       output[2] = { 0xAA, 0xAA };
        uint8_t       decoded   = 0xAA;
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x65);
        TEST(output[1] == 0x3F);

        arith_decode(&decoded, 1, output, 2, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input     = 0xAA;
        uint8_t       output[2] = { 0xAA, 0xAA };
        uint8_t       decoded   = 0xAA;
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x9A);
        TEST(output[1] == 0xBF);

        arith_decode(&decoded, 1, output, 2, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input    = 0x7F;
        uint8_t       output   = 0xAA;
        uint8_t       decoded  = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0x7D);

        arith_decode(&decoded, 1, &output, 1, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input    = 0x80;
        uint8_t       output   = 0xAA;
        uint8_t       decoded  = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0x81);

        arith_decode(&decoded, 1, &output, 1, 256);
        TEST(decoded == input);
    }

    {
        const uint8_t input[3]   = { 0x00, 0x00, 0x00 };
        uint8_t       output     = 0xAA;
        uint8_t       decoded[3] = { 0xAA, 0xAA, 0xAA };
        const size_t  out_size   = arith_encode(&output, 1, input, 3, 256);
        TEST(out_size == 1);
        TEST(output == 0x07);

        arith_decode(decoded, 3, &output, 1, 256);
        TEST(decoded[0] == input[0]);
        TEST(decoded[1] == input[1]);
        TEST(decoded[2] == input[2]);
    }

    {
        const uint8_t input[3]   = { 0xFF, 0xFF, 0xFF };
        uint8_t       output     = 0xAA;
        uint8_t       decoded[3] = { 0xAA, 0xAA, 0xAA };
        const size_t  out_size   = arith_encode(&output, 1, input, 3, 256);
        TEST(out_size == 1);
        TEST(output == 0xF8);

        arith_decode(decoded, 3, &output, 1, 256);
        TEST(decoded[0] == input[0]);
        TEST(decoded[1] == input[1]);
        TEST(decoded[2] == input[2]);
    }

    {
        const uint8_t input[3]   = { 0x40, 0x00, 0x00 };
        uint8_t       output[2]  = { 0xAA, 0xAA };
        uint8_t       decoded[3] = { 0xAA, 0xAA, 0xAA };
        const size_t  out_size   = arith_encode(output, 2, input, 3, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x55);
        TEST(output[1] == 0x80);

        arith_decode(decoded, 3, output, 2, 256);
        TEST(decoded[0] == input[0]);
        TEST(decoded[1] == input[1]);
        TEST(decoded[2] == input[2]);
    }

    {
        const uint8_t input[3]   = { 0xFF, 0xFF, 0x00 };
        uint8_t       output[4]  = { 0xAA, 0xAA, 0xAA, 0xAA };
        uint8_t       decoded[3] = { 0xAA, 0xAA, 0xAA };
        const size_t  out_size   = arith_encode(output, 4, input, 3, 256);
        TEST(out_size == 4);
        TEST(output[0] == 0xF0);
        TEST(output[1] == 0xF0);
        TEST(output[2] == 0xF1);
        TEST(output[3] == 0x00);

        arith_decode(decoded, 3, output, 4, 256);
        TEST(decoded[0] == input[0]);
        TEST(decoded[1] == input[1]);
        TEST(decoded[2] == input[2]);
    }

    {
        const uint8_t input[3]   = { 0xFF, 0xFF, 0x00 };
        uint8_t       output[2]  = { 0xAA, 0xAA };
        uint8_t       decoded[3] = { 0xAA, 0xAA, 0xAA };
        const size_t  out_size   = arith_encode(output, 2, input, 3, 8);
        TEST(out_size == 2);
        TEST(output[0] == 0xF3);
        TEST(output[1] == 0xC1);

        arith_decode(decoded, 3, output, 2, 8);
        TEST(decoded[0] == input[0]);
        TEST(decoded[1] == input[1]);
        TEST(decoded[2] == input[2]);
    }

    {
        const uint8_t input[3]   = { 0x55, 0xAA, 0x55 };
        uint8_t       output[4]  = { 0xAA, 0xAA, 0xAA, 0xAA };
        uint8_t       decoded[3] = { 0xAA, 0xAA, 0xAA };
        const size_t  out_size   = arith_encode(output, 4, input, 3, 64);
        TEST(out_size == 4);
        TEST(output[0] == 0x65);
        TEST(output[1] == 0x57);
        TEST(output[2] == 0x9E);
        TEST(output[3] == 0x7F);

        arith_decode(decoded, 3, output, 4, 64);
        TEST(decoded[0] == input[0]);
        TEST(decoded[1] == input[1]);
        TEST(decoded[2] == input[2]);
    }

    /* Encode high entropy data */
    {
        const uint8_t input[16]   = { 0xB2, 0x3D, 0x55, 0x0D, 0xCC, 0x4B, 0x63, 0x04,
                                      0x0B, 0xCD, 0xE2, 0x68, 0x9C, 0xFE, 0xCC, 0x2B };
        uint8_t       output[17]  = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                      0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                      0xAA };
        uint8_t       decoded[16] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                      0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
        const size_t  out_size    = arith_encode(output, 17, input, 16, 128);
        TEST(out_size == 17);

        arith_decode(decoded, 16, output, 17, 128);
        TEST(decoded[0]  == input[0]);
        TEST(decoded[1]  == input[1]);
        TEST(decoded[2]  == input[2]);
        TEST(decoded[3]  == input[3]);
        TEST(decoded[4]  == input[4]);
        TEST(decoded[5]  == input[5]);
        TEST(decoded[6]  == input[6]);
        TEST(decoded[7]  == input[7]);
        TEST(decoded[8]  == input[8]);
        TEST(decoded[9]  == input[9]);
        TEST(decoded[10] == input[10]);
        TEST(decoded[11] == input[11]);
        TEST(decoded[12] == input[12]);
        TEST(decoded[13] == input[13]);
        TEST(decoded[14] == input[14]);
        TEST(decoded[15] == input[15]);
    }

    /* Encode executable */
    {
        const uint8_t input[16]   = { 0xCF, 0xFA, 0xED, 0xFE, 0x07, 0x00, 0x00, 0x01,
                                      0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
        uint8_t       output[14]  = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                      0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
        uint8_t       decoded[16] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                      0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
        const size_t  out_size    = arith_encode(output, 14, input, 16, 128);
        TEST(out_size == 14);

        arith_decode(decoded, 16, output, 14, 128);
        TEST(decoded[0]  == input[0]);
        TEST(decoded[1]  == input[1]);
        TEST(decoded[2]  == input[2]);
        TEST(decoded[3]  == input[3]);
        TEST(decoded[4]  == input[4]);
        TEST(decoded[5]  == input[5]);
        TEST(decoded[6]  == input[6]);
        TEST(decoded[7]  == input[7]);
        TEST(decoded[8]  == input[8]);
        TEST(decoded[9]  == input[9]);
        TEST(decoded[10] == input[10]);
        TEST(decoded[11] == input[11]);
        TEST(decoded[12] == input[12]);
        TEST(decoded[13] == input[13]);
        TEST(decoded[14] == input[14]);
        TEST(decoded[15] == input[15]);
    }

    /* Randomly generated data */
    {
        uint32_t lcg_state = 0xBEEFF00D;
        int      step;

        for (step = 0; step < 100; step++) {
            uint8_t  input[1024];
            uint8_t  output[1280];
            uint8_t  decoded[1024];
            uint32_t window_size;
            size_t   in_size;
            size_t   out_size;
            size_t   i;

            for (i = 0; i < sizeof(input); i++)
                input[i] = lcg(&lcg_state) & 0xFFU;

            in_size     = (size_t)(512 + (lcg(&lcg_state) & 511));
            window_size = (uint8_t)(8 + (lcg(&lcg_state) & 0x1FFU));

            memset(output, 0xAA, sizeof(output));
            memset(decoded, 0xAA, sizeof(decoded));

            out_size = arith_encode(output, sizeof(output), input, in_size, window_size);

            arith_decode(decoded, in_size, output, out_size, window_size);

            if (memcmp(input, decoded, in_size)) {
                ++num_failed;
                fprintf(stderr, "Decoded data doesn't match original as step %d!\n", step);
            }
        }
    }

    return num_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
