/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "map_exe.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int map_text_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

void map_text_skip_spaces(const char **buf, const char *end)
{
    while (*buf < end && map_text_is_space(**buf))
        ++*buf;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* Parse a run of hex digits (a leading "0x" is consumed if present).  Advances
 * *buf and returns the number of value digits consumed; value in *out. */
unsigned int map_text_parse_hex(const char **buf, const char *end, uint64_t *out)
{
    const char  *pos    = *buf;
    unsigned int digits = 0;
    uint64_t     value  = 0;

    if (pos + 1 < end && pos[0] == '0' && (pos[1] == 'x' || pos[1] == 'X'))
        pos += 2;

    while (pos < end) {
        const int nibble = hex_value(*pos);

        if (nibble < 0)
            break;

        value = (value << 4) | (uint64_t)nibble;
        ++pos;
        ++digits;
    }

    *buf = pos;
    *out = value;

    return digits;
}

void map_table_init(MAP_TABLE *table)
{
    table->items    = NULL;
    table->count    = 0;
    table->capacity = 0;
}

static char *dup_string(const char *str)
{
    size_t len;
    char  *copy;

    if ( ! str)
        return NULL;

    len  = strlen(str) + 1;
    copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, str, len);

    return copy;
}

int map_table_add(MAP_TABLE *table, const char *name, uint64_t src_addr, size_t input_offset, size_t orig_size)
{
    MAP_ITEM *item;

    if (table->count == table->capacity) {
        const size_t new_capacity = table->capacity ? table->capacity * 2 : 64;
        MAP_ITEM    *grown        = (MAP_ITEM *)realloc(table->items,
                                                        new_capacity * sizeof(*grown));
        if ( ! grown)
            return -1;

        table->items    = grown;
        table->capacity = new_capacity;
    }

    item = &table->items[table->count];

    item->name = dup_string(name);
    if (name && ! item->name)
        return -1;

    item->src_addr     = src_addr;
    item->input_offset = input_offset;
    item->orig_size    = orig_size;

    table->count++;

    return 0;
}

void map_table_free(MAP_TABLE *table)
{
    size_t i;

    for (i = 0; i < table->count; i++)
        free(table->items[i].name);

    free(table->items);

    map_table_init(table);
}

static int compare_by_offset(const void *left, const void *right)
{
    const MAP_ITEM *const item_left  = (const MAP_ITEM *)left;
    const MAP_ITEM *const item_right = (const MAP_ITEM *)right;

    if (item_left->input_offset < item_right->input_offset)
        return -1;
    if (item_left->input_offset > item_right->input_offset)
        return 1;
    return 0;
}

void map_table_sort(MAP_TABLE *table)
{
    if (table->count > 1)
        qsort(table->items, table->count, sizeof(*table->items), compare_by_offset);
}

int map_table_fill_gaps(MAP_TABLE *table, size_t src_size)
{
    MAP_TABLE filled;
    size_t    covered = 0;
    size_t    i;

    map_table_init(&filled);

    for (i = 0; i < table->count; i++) {
        const MAP_ITEM *const item = &table->items[i];
        size_t                item_end;

        if (item->input_offset > covered) {
            if (map_table_add(&filled, NULL, MAP_NO_SRC, covered,
                              item->input_offset - covered)) {
                map_table_free(&filled);
                return -1;
            }
        }

        if (map_table_add(&filled, item->name, item->src_addr,
                          item->input_offset, item->orig_size)) {
            map_table_free(&filled);
            return -1;
        }

        item_end = item->input_offset + item->orig_size;
        if (item_end > covered)
            covered = item_end;
    }

    if (covered < src_size) {
        if (map_table_add(&filled, NULL, MAP_NO_SRC, covered, src_size - covered)) {
            map_table_free(&filled);
            return -1;
        }
    }

    map_table_free(table);
    *table = filled;

    return 0;
}

int map_table_make_tracker(const MAP_TABLE *table, size_t src_size, SYMBOL_BIT_COUNT *bit_count)
{
    size_t *symbol_starts;
    size_t *symbol_bit_counts;
    size_t  i;

    (void)src_size;

    if ( ! table->count)
        return -1;

    symbol_starts     = (size_t *)malloc(table->count * sizeof(*symbol_starts));
    symbol_bit_counts = (size_t *)calloc(table->count, sizeof(*symbol_bit_counts));
    if ( ! symbol_starts || ! symbol_bit_counts) {
        free(symbol_starts);
        free(symbol_bit_counts);
        return -1;
    }

    for (i = 0; i < table->count; i++)
        symbol_starts[i] = table->items[i].input_offset;

    bit_count->symbol_starts     = symbol_starts;
    bit_count->symbol_bit_counts = symbol_bit_counts;
    bit_count->num_symbols       = table->count;

    return 0;
}

void map_free_tracker(SYMBOL_BIT_COUNT *bit_count)
{
    /* symbol_starts is const in the public struct but owned by us. */
    free((void *)bit_count->symbol_starts);
    free(bit_count->symbol_bit_counts);

    bit_count->symbol_starts     = NULL;
    bit_count->symbol_bit_counts = NULL;
    bit_count->num_symbols       = 0;
}

static void print_row(const char *out_offset, uint64_t src_addr,
                      size_t orig_size, size_t new_size, const char *what)
{
    char src_text[24];
    char ratio_text[12];

    if (src_addr == MAP_NO_SRC)
        snprintf(src_text, sizeof(src_text), "%s", "-");
    else
        snprintf(src_text, sizeof(src_text), "0x%" PRIx64, src_addr);

    if (orig_size)
        snprintf(ratio_text, sizeof(ratio_text), "%zu%%", new_size * 100 / orig_size);
    else
        snprintf(ratio_text, sizeof(ratio_text), "%s", "-");

    printf("  %-12s %-18s %12zu %12zu %8s  %s\n",
           out_offset, src_text, orig_size, new_size, ratio_text, what);
}

void map_print_report(const MAP_OUT_REGION   *regions,
                      size_t                  region_count,
                      const MAP_TABLE        *table,
                      const SYMBOL_BIT_COUNT *bit_count)
{
    size_t region_index;

    printf("Output layout:\n");
    printf("  %-12s %-18s %12s %12s %8s  %s\n",
           "out_offset", "src_offset", "orig", "new", "ratio", "what");

    for (region_index = 0; region_index < region_count; region_index++) {
        const MAP_OUT_REGION *const region = &regions[region_index];
        char                        out_text[24];

        snprintf(out_text, sizeof(out_text), "0x%zx", region->out_offset);

        if ( ! region->is_payload) {
            print_row(out_text, MAP_NO_SRC, region->size, region->size, region->name);
            continue;
        }

        /* The payload row, then one row per source unit. */
        print_row(out_text, MAP_NO_SRC, region->size, region->size, region->name);

        {
            size_t i;

            for (i = 0; i < table->count; i++) {
                const MAP_ITEM *const item     = &table->items[i];
                const size_t          new_size = (bit_count->symbol_bit_counts[i] + 7) / 8;
                const char           *what     = item->name ? item->name : "(padding)";

                print_row("", item->src_addr, item->orig_size, new_size, what);
            }
        }
    }
}
