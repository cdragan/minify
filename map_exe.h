/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#pragma once

#include "buffer.h"
#include "lza_compress.h"

#include <stddef.h>
#include <stdint.h>

#define MAP_NO_SRC (~(uint64_t)0)

/* A map item is one contiguous region, either a symbol or a segment/section */
typedef struct {
    char    *name;          /* item name, owned by the struct, NULL for gaps       */
    uint64_t src_addr;      /* source address in process's VA space, or MAP_NO_SRC */
    size_t   input_offset;  /* offset within the lz_compress input buffer          */
    size_t   orig_size;     /* original size, in bytes                             */
} MAP_ITEM;

typedef struct {
    MAP_ITEM *items;
    size_t    count;
    size_t    capacity;
} MAP_TABLE;

/* One region of the final output file, in output order. */
typedef struct {
    const char *name;
    size_t      out_offset;
    size_t      size;
    int         is_payload; /* nonzero => expand into per-item rows */
} MAP_OUT_REGION;

void map_table_init(MAP_TABLE *table);
int  map_table_add(MAP_TABLE *table, const char *name, uint64_t src_addr, size_t input_offset, size_t orig_size);
void map_table_free(MAP_TABLE *table);

/* Sort items by input_offset, ascending. */
void map_table_sort(MAP_TABLE *table);

/* Insert padding gap items.  Requires the table sorted.  Returns 0 on success. */
int  map_table_fill_gaps(MAP_TABLE *table, size_t src_size);

/* Build a SYMBOL_BIT_COUNT from a sorted, gap-filled table.  Allocates
 * symbol_starts and symbol_bit_counts (freed with map_free_tracker).
 * symbol_bit_counts[i] corresponds to table->items[i].  Returns 0 on success. */
int  map_table_make_tracker(const MAP_TABLE *table, size_t src_size, SYMBOL_BIT_COUNT *bit_count);
void map_free_tracker(SYMBOL_BIT_COUNT *bit_count);

/* Text-scanning helpers shared by the per-format map parsers. */
int          map_text_is_space(char ch);
void         map_text_skip_spaces(const char **buf, const char *end);
unsigned int map_text_parse_hex(const char **buf, const char *end, uint64_t *out);

/* Print the unified layout report to stdout.  Regions print in array order; the
 * region with is_payload set is expanded into one row per table item, using
 * bit_count->symbol_bit_counts[i] for the per-item compressed size. */
void map_print_report(const MAP_OUT_REGION *regions, size_t region_count,
                      const MAP_TABLE *table, const SYMBOL_BIT_COUNT *bit_count);
