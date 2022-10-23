/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

void decompress(void       *dest,
                size_t      dest_size,
                size_t      scratch_size,
                const void *compressed,
                size_t      compressed_size);
