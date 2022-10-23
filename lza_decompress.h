/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

void decompress(void       *dest,
                size_t      dest_size,
                size_t      scratch_size,
                const void *compressed,
                uint32_t    window_size,
                size_t      type_buf_size,
                size_t      literal_buf_size,
                size_t      size_buf_size,
                size_t      offset_buf_size);
