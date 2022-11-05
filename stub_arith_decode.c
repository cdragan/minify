/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_decode.h"

#ifdef _WIN32
#define main __stdcall WinMainCRTStartup
#endif

uint8_t *input;
uint32_t scratch_size;
uint8_t *compressed;
uint32_t compressed_size;

int main(void)
{
    const uint32_t window_size = *(uint16_t *)compressed;
    arith_decode(input, scratch_size, compressed + 2, compressed_size - 2, window_size);

    return 0;
}
