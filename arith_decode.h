/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#pragma once

#include <stddef.h>

#define MAX_WINDOW_SIZE 2048

void arith_decode(void *dest, size_t dest_size, const void *src, size_t src_size);
