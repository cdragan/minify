/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_encode.h"

#include <stdio.h>
#include <stdlib.h>

#define TEST(expr) do { if ( ! (expr)) { report_error(#expr, __LINE__); ++num_failed; } } while (0)

static void report_error(const char *desc, int line)
{
    fprintf(stderr, "test_arith_encode.c:%d: failed test: %s\n",
            line, desc);
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
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0x0F);
    }

    {
        const uint8_t input    = 0xFF;
        uint8_t       output   = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0xEF);
    }

    {
        const uint8_t input     = 0x0F;
        uint8_t       output[2] = { 0xAA, 0xAA };
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0xCC);
        TEST(output[1] == 0xFF);
    }

    {
        const uint8_t input     = 0xF0;
        uint8_t       output[2] = { 0xAA, 0xAA };
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x32);
        TEST(output[1] == 0xFF);
    }

    {
        const uint8_t input     = 0x55;
        uint8_t       output[2] = { 0xAA, 0xAA };
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x9A);
        TEST(output[1] == 0xBF);
    }

    {
        const uint8_t input     = 0xAA;
        uint8_t       output[2] = { 0xAA, 0xAA };
        const size_t  out_size  = arith_encode(output, 2, &input, 1, 256);
        TEST(out_size == 2);
        TEST(output[0] == 0x65);
        TEST(output[1] == 0x3F);
    }

    {
        const uint8_t input    = 0x7F;
        uint8_t       output   = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0xE0);
    }

    {
        const uint8_t input    = 0x80;
        uint8_t       output   = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 1, 256);
        TEST(out_size == 1);
        TEST(output == 0x1D);
    }

    {
        const uint8_t input[3] = { 0x00, 0x00, 0x00 };
        uint8_t       output   = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 3, 256);
        TEST(out_size == 1);
        TEST(output == 0x07);
    }

    {
        const uint8_t input[3] = { 0xFF, 0xFF, 0xFF };
        uint8_t       output   = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 3, 256);
        TEST(out_size == 1);
        TEST(output == 0xF8);
    }

    {
        const uint8_t input[3] = { 0x40, 0x00, 0x00 };
        uint8_t       output   = 0xAA;
        const size_t  out_size = arith_encode(&output, 1, &input, 3, 256);
        TEST(out_size == 1);
        TEST(output == 0x20);
    }

    {
        const uint8_t input[3]  = { 0xFF, 0xFF, 0x00 };
        uint8_t       output[3] = { 0xAA, 0xAA };
        const size_t  out_size  = arith_encode(&output, 3, &input, 3, 256);
        TEST(out_size == 3);
        TEST(output[0] == 0xF0);
        TEST(output[1] == 0xF0);
        TEST(output[2] == 0xF1);
    }

    {
        const uint8_t input[3]  = { 0xFF, 0xFF, 0x00 };
        uint8_t       output[3] = { 0xAA, 0xAA };
        const size_t  out_size  = arith_encode(&output, 3, &input, 3, 8);
        TEST(out_size == 2);
        TEST(output[0] == 0xF3);
        TEST(output[1] == 0xC1);
    }

    return num_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}