/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "find_repeats.h"
#include "bit_ops.h"
#include "lza_defines.h"

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
    uint32_t       first_free_chunk_id;     /* For allocating new chunks */
    uint32_t       last_pair_index;         /* To avoid storing offsets for subsequent repeated bytes */
    uint32_t       last_pos;                /* For assertions */
    uint8_t        dummy_align[64 - 4 * sizeof(uint32_t)]; /* Align each chunk on cache line boundary */
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
    map->last_pair_index     = ~0U;
    map->last_pos            = ~0U;
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
    const uint32_t  idx = get_map_idx(buf, pos) & 0xFFFFU;
    uint32_t        chunk_id;
    uint32_t        new_id;
    LOCATION_CHUNK *chunk;

#ifndef NDEBUG
    assert(pos == map->last_pos + 1);
    map->last_pos = (uint32_t)pos;
#endif

    /* Performance optimization.  If we encounter two subsequent identical bytes,
     * only store the offset of the first such pair, don't store the offsets
     * for subsequent bytes.
     */
    if (map->last_pair_index == idx) {
        assert(buf[pos] == buf[pos + 1]);
        return;
    }

    map->last_pair_index = idx;

    chunk_id = map->pair_ids[idx];

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
    const uint8_t *left      = &buf[left_pos];
    const uint8_t *right     = &buf[right_pos];
    const uint32_t right_end = (uint32_t)(size - right_pos);
    const uint32_t end       = (right_end > MAX_LZA_SIZE) ? MAX_LZA_SIZE : right_end;
    uint32_t       length    = 2;

    assert(left[0] == right[0]);
    assert(left[1] == right[1]);

    while ((length < end) && (left[length] == right[length]))
        ++length;

    return length;
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

static int calc_cond_longrep_score(int longrep, uint32_t length)
{
    return longrep < 0 ? 0 : calc_longrep_score(longrep, length);
}

static int is_8_byte_aligned(const uint8_t *ptr)
{
    return ! ((uintptr_t)ptr & 7U);
}

/* Determine length of a region filled with the same byte value */
static size_t get_repeated_byte_length(const uint8_t *buf, size_t pos, size_t size)
{
    size_t        length = 1;
    const uint8_t byte   = buf[pos];

    assert(pos + 1 < size);

    buf  += pos + 1;
    size -= pos + 1;

    /* Check the beginning byte by byte until we hit aligned address */
    while (length < size && *buf == byte && ! is_8_byte_aligned(buf)) {
        ++length;
        ++buf;
    }

    /* Check 8 bytes at a time (perf optimization) */
    if (*buf == byte) {
        size_t         next_length = length + 8;
        const uint64_t qword       = 0x0101010101010101ULL * (uint64_t)byte;

        while (next_length <= size && *(uint64_t *)buf == qword) {
            length       = next_length;
            next_length += 8;
            buf         += 8;
        }
    }

    /* Check one byte at a time until we find a byte that does not match */
    while (length < size && *buf == byte) {
        ++length;
        ++buf;
    }

    return length;
}

static OCCURRENCE find_occurrence_at_last_dist(const uint8_t *buf,
                                               size_t         pos,
                                               size_t         size,
                                               const uint32_t last_dist[])
{
    OCCURRENCE occurrence = { 0, 0, -1 };
    int        last;

    for (last = 3; last >= 0; last--) {
        const uint32_t distance = last_dist[last];
        uint32_t       length;

        if ( ! distance)
            continue;

        if (buf[pos - distance] != buf[pos])
            continue;
        if (buf[pos - distance + 1] != buf[pos + 1])
            continue;

        length = compare(buf, pos - distance, pos, size);

        if (length >= occurrence.length) {
            occurrence.distance = distance;
            occurrence.length   = length;
            occurrence.last     = last;
        }
    }

    return occurrence;
}

static OCCURRENCE find_longest_occurrence(const uint8_t    *buf,
                                          size_t            pos,
                                          size_t            size,
                                          const uint32_t    last_dist[],
                                          const OFFSET_MAP *map)
{
    OCCURRENCE   occurrence      = find_occurrence_at_last_dist(buf, pos, size, last_dist);
    int          score           = calc_cond_longrep_score(occurrence.last, occurrence.length);
    const size_t repeated_length = get_repeated_byte_length(buf, pos, size);

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

            length   = compare(buf, old_pos, pos, size);
            distance = (uint32_t)pos - old_pos;

            /* For repeated bytes, we only store the offset of the first pair,
             * so try to find shorter distance.
             */
            if (length <= repeated_length && distance > 1) {
                const size_t max_len = get_repeated_byte_length(buf, pos - distance, size);

                if (max_len > length) {
                    const uint32_t diff = (uint32_t)(max_len - length);

                    if (diff < distance)
                        distance -= diff;
                    else
                        distance = 1;

                    /* Recalculate length in case other bytes after the repeated
                     * span can match.
                     */
                    length = compare(buf, pos - distance, pos, size);
                }
            }

            for (last = 3; last >= 0; last--)
                if (distance == last_dist[last])
                    break;

            /* Last distances already processed */
            if (last >= 0)
                continue;

            cur_score = calc_match_score(distance, length);

            if (cur_score <= score)
                continue;

            if (cur_score < 2)
                continue;

            if (length == 3 && distance > (1U << 11))
                continue;

            if (length == 4 && distance > (1U << 13))
                continue;

            occurrence.length   = length;
            occurrence.distance = distance;
            occurrence.last     = last;
            score               = cur_score;
        }

        chunk_id = chunk->next_id;
    }

    return occurrence;
}

static void report_literal_or_single_match(const uint8_t *buf,
                                           size_t         pos,
                                           size_t         size,
                                           uint32_t       last_dist,
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
    uint32_t    last_dist[4] = { 0, 0, 0, 0 };

    if ( ! size)
        return 0;

    map = alloc_offset_map(size);
    if ( ! map)
        return 1;

    /* Find subsequent matches as long as we have at least two consecutive bytes */
    while (pos + 1 < size) {
        OCCURRENCE occurrence = find_longest_occurrence(buf, pos, size, last_dist, map);
        size_t     i;

        if ( ! occurrence.length) {
            set_offset(buf, pos, map);
            ++pos;
            ++num_literal;
            continue;
        }

        /* See if we can find a better match */
        if ((occurrence.last < 0) && (pos + 2 < size)) {

            const OCCURRENCE next_occurrence = find_occurrence_at_last_dist(buf, pos + 1, size, last_dist);

            if (next_occurrence.last >= 0) {
                const int cur_score  = calc_match_score(occurrence.distance, occurrence.length);
                const int next_score = calc_longrep_score(next_occurrence.last, next_occurrence.length);

                /* If it is beneficial, report the current byte as LIT(eral) and start match from
                 * the next byte */
                if (next_score >= cur_score) {
                    set_offset(buf, pos, map);
                    ++pos;
                    ++num_literal;

                    occurrence = next_occurrence;
                }
            }
        }

        if (num_literal) {
            report_literal_or_single_match(buf, pos - num_literal, num_literal, last_dist[0],
                                           report_literal, report_match, cookie);
            num_literal = 0;
        }

        assert(occurrence.distance > 0);

        report_match(cookie, buf, pos, occurrence);

        /* Append distance to the list of last 4 distances, without duplicates */
        for (i = 0; i < 3; i++) {
            if (last_dist[i] == occurrence.distance)
                break;
        }
        for (; i > 0; i--)
            last_dist[i] = last_dist[i - 1];
        last_dist[0]    = occurrence.distance;
        occurrence.last = 0;

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
