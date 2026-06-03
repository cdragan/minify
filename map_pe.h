/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include "buffer.h"
#include "map_exe.h"

#include <stdint.h>

/* Parse an MSVC link.exe /MAP buffer into per-symbol items.  Only symbols whose
 * RVA falls within [va_start, va_start + input_size) are kept; each gets
 * input_offset = rva - va_start and src_addr = rva + image_base.  Sizes are
 * inferred from the gap to the next symbol by value.  The table must be empty on
 * entry.  Returns 0 on success (including when the map yields no symbols). */
int pe_parse_map(BUFFER map, uint32_t va_start, uint32_t input_size,
                 uint64_t image_base, MAP_TABLE *out);
