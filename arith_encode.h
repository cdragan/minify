/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

size_t arith_encode(void *dest, size_t max_dest_size, const void *src, size_t size, uint32_t window_size);
