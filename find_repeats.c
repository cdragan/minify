/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "find_repeats.h"
#include "bit_ops.h"

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
    uint8_t        dummy_align[64 - 2 * sizeof(uint32_t)]; /* Align each chunk on cache line boundary */
    LOCATION_CHUNK chunks[1];
} OFFSET_MAP;

static uint32_t estimate_chunks(size_t file_size)
{
    const uint32_t est_chunk_count = ((uint32_t)file_size / MAX_OFFSETS) * 2;

    return (est_chunk_count < 0x10000U) ? 0x10000U : est_chunk_count;
}

static size_t calc_offset_map_size(uint32_t num_chunks)
{
    assert(num_chunks > 1);
    return sizeof(OFFSET_MAP) + sizeof(LOCATION_CHUNK) * (num_chunks - 1);
}

static void init_offset_map(OFFSET_MAP *map, uint32_t num_chunks)
{
    memset(map, 0xFF, calc_offset_map_size(num_chunks));

    map->num_chunks          = num_chunks;
    map->first_free_chunk_id = 0;
}

static OFFSET_MAP *alloc_offset_map(size_t file_size)
{
    const uint32_t num_chunks = estimate_chunks(file_size);

    OFFSET_MAP *const map = (OFFSET_MAP *)malloc(calc_offset_map_size(num_chunks));
    if (map)
        init_offset_map(map, num_chunks);
    else
        perror(NULL);

    return map;
}

static uint32_t get_free_chunk(OFFSET_MAP *map)
{
    assert(map->first_free_chunk_id < map->num_chunks);

    return map->first_free_chunk_id++;
}

static uint32_t get_map_idx(const uint8_t *buf, size_t pos)
{
    const uint32_t b0  = buf[pos];
    const uint32_t b1  = buf[pos + 1];
    const uint32_t idx = b0 | (b1 << 8);
    return idx;
}

static void set_offset(const uint8_t *buf, size_t pos, OFFSET_MAP *map)
{
    const uint32_t  idx      = get_map_idx(buf, pos) & 0xFFFFU;
    uint32_t        chunk_id = map->pair_ids[idx];
    uint32_t        new_id;
    LOCATION_CHUNK *chunk;

    if (chunk_id != INVALID_ID) {
        chunk = &map->chunks[chunk_id];

        if (chunk->offset[0] == INVALID_ID) {
            uint32_t i;

            for (i = 1; i < MAX_OFFSETS; i++) {
                if (chunk->offset[i] != INVALID_ID) {
                    assert(pos > chunk->offset[i]);
                    break;
                }
            }
            --i;

            chunk->offset[i] = (uint32_t)pos;
            return;
        }
    }

    new_id = get_free_chunk(map);

    chunk                          = &map->chunks[new_id];
    chunk->next_id                 = chunk_id;
    chunk->offset[MAX_OFFSETS - 1] = (uint32_t)pos;

    map->pair_ids[idx] = new_id;
}

static uint32_t compare(const uint8_t *buf, size_t left_pos, size_t right_pos, size_t size)
{
    const uint8_t       *left  = buf + left_pos  + 2;
    const uint8_t       *right = buf + right_pos + 2;
    const uint8_t *const end   = buf + size;

    assert(buf[left_pos]     == buf[right_pos]);
    assert(buf[left_pos + 1] == buf[right_pos + 1]);

    while ((right < end) && (*left == *right)) {
        ++left;
        ++right;
    }

    return (uint32_t)(left - (buf + left_pos));
}

static int calc_match_score(uint32_t distance, uint32_t length)
{
    /* Number of bits if this was emitted as LIT packets (literals) */
    const int lit_bits = 9 * (int)length;

    /* Number of bits if this was emitted as MATCH packet */
    const int match_hdr_bits = 2;
    const int length_bits    = (length <= 9) ? 4 : (length <= 17) ? 5 : 10;
    const int distance_bits  = (distance < 2) ? 6 : (36 - count_leading_zeroes(distance));
    const int match_bits     = match_hdr_bits + length_bits + distance_bits;

    return lit_bits - match_bits;
}

static int calc_longrep_score(int longrep, uint32_t length)
{
    /* Number of bits if this was emitted as LIT packets (literals) */
    const int lit_bits = 9 * (int)length;

    /* Number of bits if this was emitted as LONGREP* packet */
    const int longrep_hdr_bits = (longrep < 2) ? 4 : 5;
    const int length_bits      = (length <= 9) ? 4 : (length <= 17) ? 5 : 10;
    const int longrep_bits     = longrep_hdr_bits + length_bits;

    return lit_bits - longrep_bits;
}

static OCCURRENCE find_longest_occurrence(const uint8_t    *buf,
                                          size_t            pos,
                                          size_t            size,
                                          size_t            last_dist[],
                                          const OFFSET_MAP *map)
{
    OCCURRENCE occurrence = { 0, 0, -1 };
    int        score      = 0;

    uint32_t chunk_id = map->pair_ids[get_map_idx(buf, pos)];

    while (chunk_id != INVALID_ID) {
        const LOCATION_CHUNK *chunk = &map->chunks[chunk_id];
        uint32_t              i;

        for (i = 0; i < MAX_OFFSETS; i++) {
            uint32_t length;
            uint32_t distance;
            int      last;
            int      cur_score;

            const uint32_t old_pos = chunk->offset[i];
            if (old_pos == INVALID_ID)
                continue;

            length = compare(buf, old_pos, pos, size);
            distance = (uint32_t)pos - old_pos;

            if (length < occurrence.length)
                continue;

            for (last = 0; last < 4; last++)
                if (distance == last_dist[last])
                    break;

            cur_score = (last == 4) ? calc_match_score(distance, length) : calc_longrep_score(last, length);

            if (cur_score <= score)
                continue;

            if (last == 4) {
                if (length == 2 && distance > (1U << 6))
                    continue;

                if (length == 3 && distance > (1U << 11))
                    continue;

                if (length == 4 && distance > (1U << 13))
                    continue;
            }

            occurrence.length   = length;
            occurrence.distance = distance;
            occurrence.last     = (last == 4) ? -1 : last;
            score               = cur_score;
        }

        chunk_id = chunk->next_id;
    }

    return occurrence;
}

static void report_literal_or_single_match(const uint8_t *buf,
                                           size_t         pos,
                                           size_t         size,
                                           size_t         last_dist,
                                           REPORT_LITERAL report_literal,
                                           REPORT_MATCH   report_match,
                                           void          *cookie)
{
    size_t       num_literal = 0;
    const size_t end         = pos + size;

    for ( ; pos < end; ++pos) {
        if (last_dist && buf[pos] == buf[pos - last_dist]) {
            OCCURRENCE occurrence = { last_dist, 1, 0 };

            if (num_literal) {
                report_literal(cookie, buf, pos - num_literal, num_literal);
                num_literal = 0;
            }

            report_match(cookie, buf, pos, occurrence);
        }
        else
            ++num_literal;
    }

    if (num_literal)
        report_literal(cookie, buf, pos - num_literal, num_literal);
}

int find_repeats(const uint8_t *buf,
                 size_t         size,
                 REPORT_LITERAL report_literal,
                 REPORT_MATCH   report_match,
                 void          *cookie)
{
    OFFSET_MAP *map;
    size_t      pos          = 0;
    size_t      num_literal  = 0;
    size_t      last_dist[4] = { 0, 0, 0, 0 };

    if ( ! size)
        return 0;

    map = alloc_offset_map(size);
    if ( ! map)
        return 1;

    /* Find subsequent matches as long as we have at least two consecutive bytes */
    while (pos + 1 < size) {
        OCCURRENCE occurrence = find_longest_occurrence(buf, pos, size, last_dist, map);
        size_t     i;
        size_t     rel_offs;

        if ( ! occurrence.length) {
            set_offset(buf, pos, map);
            ++pos;
            ++num_literal;
            continue;
        }

        if (num_literal) {
            report_literal_or_single_match(buf, pos - num_literal, num_literal, last_dist[0],
                                           report_literal, report_match, cookie);
            num_literal = 0;
        }

        assert(occurrence.distance > 0);

        rel_offs = 0;
        do {
            const size_t full_length = occurrence.length;
            const size_t num_left    = full_length - rel_offs;

            occurrence.length = (num_left > 273) ? 273 : num_left;

            report_match(cookie, buf, pos + rel_offs, occurrence);

            /* Append distance to the list of last 4 distances, without duplicates */
            for (i = 0; i < 3; i++) {
                if (last_dist[i] == occurrence.distance)
                    break;
            }
            for (; i > 0; i--)
                last_dist[i] = last_dist[i - 1];
            last_dist[0]    = occurrence.distance;
            occurrence.last = 0;

            rel_offs         += occurrence.length;
            occurrence.length = full_length;
        } while (rel_offs < occurrence.length);

        /* Update lookup table with byte pair at every position */
        for (i = 0; i < occurrence.length; ++i) {
            if (pos + 1 < size)
                set_offset(buf, pos, map);
            ++pos;
        }
    }

    if (pos < size) {
        assert(pos + 1 == size);
        ++num_literal;
        ++pos;
    }

    if (num_literal)
        report_literal_or_single_match(buf, pos - num_literal, num_literal, last_dist[0],
                                       report_literal, report_match, cookie);

    free(map);

    return 0;
}
