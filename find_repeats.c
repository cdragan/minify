/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "find_repeats.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OFFSETS 15
#define INVALID_ID  (~0U)

typedef struct {
    uint32_t offset[MAX_OFFSETS];
    uint32_t next_id;
} LOCATION_CHUNK;

typedef struct {
    uint32_t       pair_ids[256 * 256];
    uint32_t       num_chunks;
    uint32_t       first_free_chunk_id;
    LOCATION_CHUNK chunks[1];
} OFFSET_MAP;

static size_t estimate_chunks(size_t file_size)
{
    const size_t est_chunk_count = (file_size / MAX_OFFSETS) * 2;

    return (est_chunk_count < 0x10000U) ? 0x10000U : est_chunk_count;
}

static OFFSET_MAP *alloc_offset_map(size_t file_size)
{
    const size_t est_chunk_count = estimate_chunks(file_size);
    const size_t alloc_size      = sizeof(OFFSET_MAP) + sizeof(LOCATION_CHUNK) * est_chunk_count;

    OFFSET_MAP *const map = (OFFSET_MAP *)malloc(alloc_size);
    if (map) {
        memset(map, 0xFF, alloc_size);

        map->num_chunks          = est_chunk_count + 1;
        map->first_free_chunk_id = 0;
    }
    else
        perror(NULL);

    return map;
}

static uint32_t get_free_chunk(OFFSET_MAP *map)
{
    assert(map->first_free_chunk_id < map->num_chunks);

    return map->first_free_chunk_id++;
}

static uint32_t get_map_idx(const char *buf, size_t pos)
{
    const uint32_t b0  = (uint8_t)buf[pos];
    const uint32_t b1  = (uint8_t)buf[pos + 1];
    const uint32_t idx = b0 | (b1 << 8);
    return idx;
}

static void set_offset(const char *buf, size_t pos, OFFSET_MAP *map)
{
    const uint16_t  idx      = get_map_idx(buf, pos);
    uint32_t        chunk_id = map->pair_ids[idx];
    uint32_t        new_id;
    LOCATION_CHUNK *chunk;

    if (chunk_id != INVALID_ID) {
        chunk = &map->chunks[chunk_id];

        if (chunk->offset[MAX_OFFSETS - 1] == INVALID_ID) {
            uint32_t i;
            uint32_t next_id;

            for (i = 0; i < MAX_OFFSETS; i++)
                if (chunk->offset[i] == INVALID_ID)
                    break;
            assert(i < MAX_OFFSETS);

            chunk->offset[i] = pos;
            return;
        }
    }

    new_id = get_free_chunk(map);

    chunk            = &map->chunks[new_id];
    chunk->next_id   = chunk_id;
    chunk->offset[0] = (uint32_t)pos;

    map->pair_ids[idx] = new_id;
}

static uint32_t compare(const char *buf, size_t left_pos, size_t right_pos, size_t size)
{
    const char       *left  = buf + left_pos  + 2;
    const char       *right = buf + right_pos + 2;
    const char *const end   = buf + size;

    assert(buf[left_pos]     == buf[right_pos]);
    assert(buf[left_pos + 1] == buf[right_pos + 1]);

    while ((right < end) && (*left == *right)) {
        ++left;
        ++right;
    }

    return (uint32_t)(left - (buf + left_pos));
}

typedef struct {
    size_t offset;
    size_t length;
} OCCURRENCE;

static OCCURRENCE find_longest_occurrence(const char       *buf,
                                          size_t            pos,
                                          size_t            size,
                                          size_t            last_offs,
                                          const OFFSET_MAP *map)
{
    OCCURRENCE occurrence    = { 0, 0 };
    size_t     same_offs_len = 0;

    uint32_t chunk_id = map->pair_ids[get_map_idx(buf, pos)];

    while (chunk_id != INVALID_ID) {
        const LOCATION_CHUNK *chunk = &map->chunks[chunk_id];
        uint32_t              i;

        for (i = 0; i < MAX_OFFSETS; i++) {
            uint32_t length;
            uint32_t offset;

            const uint32_t old_pos = chunk->offset[i];
            if (old_pos == INVALID_ID)
                break;

            length = compare(buf, old_pos, pos, size);
            offset = pos - old_pos;

            if (length < occurrence.length)
                continue;

            if (offset == last_offs)
                same_offs_len = length;

            if (length == occurrence.length && offset > occurrence.offset)
                continue;

            if (length == 2 && offset >= 256)
                continue;

            occurrence.length = length;
            occurrence.offset = offset;
        }

        chunk_id = chunk->next_id;
    }

    if (same_offs_len == occurrence.length)
        occurrence.offset = last_offs;

    return occurrence;
}

int find_repeats(const char         *buf,
                 size_t              size,
                 REPORT_UNIQUE_BYTES report_unique_bytes,
                 REPORT_REPEAT       report_repeat,
                 void               *cookie)
{
    OFFSET_MAP *map;
    size_t      pos        = 0;
    size_t      num_unique = 0;
    size_t      last_offs  = 0;

    if ( ! size)
        return 0;

    map = alloc_offset_map(size);
    if ( ! map)
        return 1;

    while (pos + 1 < size) {
        const OCCURRENCE occurrence = find_longest_occurrence(buf, pos, size, last_offs, map);
        size_t           i;

        if ( ! occurrence.length) {
            set_offset(buf, pos, map);
            ++pos;
            ++num_unique;
            continue;
        }

        if (num_unique) {
            report_unique_bytes(cookie, buf, pos - num_unique, num_unique);
            num_unique = 0;
        }

        assert(occurrence.offset > 0);

        report_repeat(cookie, buf, pos, occurrence.offset, occurrence.length);

        last_offs = occurrence.offset;

        for (i = 0; i < occurrence.length; ++i) {
            if (pos + 1 < size)
                set_offset(buf, pos, map);
            ++pos;
        }
    }

    if (pos < size) {
        assert(pos + 1 == size);
        ++num_unique;
        ++pos;
    }

    if (num_unique)
        report_unique_bytes(cookie, buf, pos - num_unique, num_unique);

    free(map);

    return 0;
}
