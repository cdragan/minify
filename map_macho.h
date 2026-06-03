/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include "buffer.h"
#include "map_exe.h"

#include <stdint.h>

/* Coordinates of the combined/raw buffer [__TEXT][rebases][__DATA] fed to
 * lza_compress, used to translate ld64 map vmaddrs into input offsets. */
typedef struct {
    uint64_t text_vmaddr;
    size_t   text_size;          /* input.text_seg->filesize    */
    size_t   rebase_bytes;       /* synthetic rebase table size */
    uint64_t data_vmaddr;        /* valid only when data_folded */
    size_t   data_content_size;
    int      data_folded;
} MACHO_MAP_CONTEXT;

/* Parse an Apple ld64 -map buffer into per-symbol items.  Symbols inside __TEXT
 * (and __DATA when folded) are translated to combined/raw input offsets and
 * added with their explicit sizes; symbols elsewhere are skipped.  The table
 * must be empty on entry.  Returns 0 on success. */
int macho_parse_map(BUFFER map, const MACHO_MAP_CONTEXT *ctx, MAP_TABLE *out);
