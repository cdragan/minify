/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>

void decompress(void       *dest,
                size_t      dest_size,
                const void *compressed,
                size_t      type_buf_size,
                size_t      literal_buf_size,
                size_t      size_buf_size,
                size_t      offset_buf_size);
