/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include <stddef.h>

void lza_decompress(void *dest, size_t dest_size, const void *src, size_t src_size);
