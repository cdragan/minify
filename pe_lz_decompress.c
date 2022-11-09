/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_decompress.h"

#include <stdint.h>

#ifdef _WIN32
#define main __stdcall WinMainCRTStartup
#endif

uint8_t       *output;
uint32_t       output_size;
const uint8_t *input;

int main(void)
{
    lz_decompress(output, output_size, input);

    return 0;
}
