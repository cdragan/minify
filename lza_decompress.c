/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_decompress.h"
#include "arith_decode.h"

#include <assert.h>
#include <stdint.h>

void lza_decompress(void       *input_dest,
                    size_t      dest_size,
                    size_t      scratch_size,
                    const void *compressed,
                    size_t      compressed_size)
{
    uint8_t *const dest  = (uint8_t *)input_dest;
    uint8_t *const input = (uint8_t *)dest + dest_size;
    uint32_t       window_size;

    assert(dest_size);
    assert(compressed_size > 2);

    window_size = *(uint16_t *)compressed;
    arith_decode(input, scratch_size, (const uint8_t *)compressed + 2, compressed_size - 2, window_size);

    lz_decompress(input_dest, dest_size, input);
}
