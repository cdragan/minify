/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#pragma once

#include <stddef.h>

size_t arith_encode(void *dest, size_t max_dest_size, const void *src, size_t size);
