/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#include "find_repeats.h"
#include "bit_ops.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LZA_LENGTH_TAIL_BITS 11
#define MAX_LZA_SIZE (17 + (1 << LZA_LENGTH_TAIL_BITS))
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
    const uint32_t byte_lo = buf[pos];
    const uint32_t byte_hi = buf[pos + 1];
    return byte_lo | (byte_hi << 8);
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

/* Append distance to the list of last 4 distances, without duplicates. */
static void update_last4(uint32_t last_dist[], uint32_t distance)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (last_dist[i] == distance)
            break;
    }

    for (; i > 0; i--)
        last_dist[i] = last_dist[i - 1];

    last_dist[0] = distance;
}

static uint32_t calc_match_length(const uint8_t *buf, size_t left, size_t right, size_t size)
{
    size_t   max = size - right;
    uint32_t match_length = 0;

    if (max > MAX_LZA_SIZE)
        max = MAX_LZA_SIZE;

    while (match_length < max && buf[left + match_length] == buf[right + match_length])
        ++match_length;

    return match_length;
}

static uint32_t calc_length_bits(uint32_t length)
{
    if (length <= 9)
        return 1 + 3;

    if (length <= 17)
        return 2 + 3;

    return 2 + LZA_LENGTH_TAIL_BITS;
}

static uint32_t calc_distance_bits(uint32_t distance)
{
    --distance;

    if (distance < 2)
        return 6;

    return (31U - (uint32_t)count_leading_zeroes(distance)) + 5;
}

/* Static per-packet bit costs. */
#define LIT_BITS      9     /* 1 type bit + 1 literal MSB bit + 7 literal bits */
#define MATCH_BITS    2
#define SHORTREP_BITS 4
#define INFINITE_COST (~0U)

/* Maximum number of subsequent lengths attempted to be encoded when searching
 * for the optimal packet layout to minimize the output.
 * Longer lengths typically buy us nothing except slowing down compression.
 */
#define MAX_FINE_LENGTH 128

/* Limit for searching for matches.  Increasing this could gain a few bytes
 * in the compressed output trading off increased compression time.
 */
#define MAX_CHAIN_DEPTH 256

enum PACKET_KIND {
    PACKET_LIT,
    PACKET_MATCH,
    PACKET_REP
};

/* We model the compression as a graph.  Each node is a byte position in the input.
 * Edges are different compressed packets (LIT, MATCH, REP).  We can then optimize
 * the compression and find the best combination of packets which yields the smallest
 * compressed output.  To do this, we remember bit cost to reach each node, so we can
 * select better combinations of packets when we find them.
 */
typedef struct {
    uint32_t cost;         /* min bits to encode the input up to this position        */
    uint32_t from_pos;     /* start of the packet that reaches here (backtrack)       */
    uint32_t next_pos;     /* next position on the chosen path (set in backtrack)     */
    uint32_t packet_dist;  /* distance of that packet (for PACKET_MATCH/PACKET_REP)   */
    uint32_t reps[4];      /* the 4 most recent distances along the best path         */
    uint32_t run_length;   /* run of identical bytes starting here (precomputed)      */
    uint8_t  kind;         /* enum PACKET_KIND of that packet                         */
    uint8_t  last;         /* rep index 0..3 for PACKET_REP (matches OCCURRENCE.last) */
} NODE;

static void consider_packet(NODE           *nodes,
                            size_t          to,
                            uint32_t        cost,
                            size_t          from,
                            uint8_t         kind,
                            uint32_t        dist,
                            uint8_t         last,
                            const uint32_t  new_reps[4])
{
    NODE *const node = &nodes[to];

    if (cost >= node->cost)
        return;

    node->cost        = cost;
    node->from_pos    = (uint32_t)from;
    node->packet_dist = dist;
    node->kind        = kind;
    node->last        = last;

    node->reps[0] = new_reps[0];
    node->reps[1] = new_reps[1];
    node->reps[2] = new_reps[2];
    node->reps[3] = new_reps[3];
}

/* Considers a found match, to see if it can produce shorter output
 * (shorter path through position node graph).  Multiple lengths are considered
 * starting at length 2 and up, because sometimes sacrificing one length can
 * lead to shorter rep packets afterwards.
 */
static void consider_match(NODE           *nodes,
                           size_t          pos,
                           uint32_t        base,
                           uint32_t        max_len,
                           uint32_t        type_bits,
                           uint32_t        dist_bits,
                           uint8_t         kind,
                           uint32_t        dist,
                           uint8_t         last,
                           const uint32_t  new_reps[4])
{
    uint32_t length;
    uint32_t fine = (max_len > MAX_FINE_LENGTH) ? MAX_FINE_LENGTH : max_len;

    for (length = 2; length <= fine; length++)
        consider_packet(nodes,
                        pos + length,
                        base + type_bits + calc_length_bits(length) + dist_bits,
                        pos, kind, dist, last, new_reps);

    if (max_len > MAX_FINE_LENGTH)
        consider_packet(nodes,
                        pos + max_len,
                        base + type_bits + calc_length_bits(max_len) + dist_bits,
                        pos, kind, dist, last, new_reps);
}

static void push_rep_distance(const uint32_t cur_reps[4], uint32_t dist, uint32_t out[4])
{
    out[0] = cur_reps[0];
    out[1] = cur_reps[1];
    out[2] = cur_reps[2];
    out[3] = cur_reps[3];

    update_last4(out, dist);
}

/* Compresses buf into the smallest sequence of LIT/MATCH/REP packets and
 * reports each one through the callbacks.
 */
int find_repeats(const uint8_t *buf,
                 size_t         size,
                 REPORT_LITERAL report_literal,
                 REPORT_MATCH   report_match,
                 void          *cookie)
{
    OFFSET_MAP *map;
    NODE       *nodes = NULL;  /* one search node per byte position */
    size_t      pos;
    int         ret = 1;

    if ( ! size)
        return 0;

    map = alloc_offset_map(size);
    if ( ! map)
        return 1;

    nodes = (NODE *)malloc((size + 1) * sizeof(NODE));
    if ( ! nodes) {
        perror(NULL);
        goto cleanup;
    }

    /* All positions except 0 are initialized to INFINITE_COST (unreachable) */
    memset(nodes, 0xFF, (size + 1) * sizeof(NODE));
    nodes[0].cost    = 0;
    nodes[0].reps[0] = 0;
    nodes[0].reps[1] = 0;
    nodes[0].reps[2] = 0;
    nodes[0].reps[3] = 0;

    /* Precompute number of consecutive identical bytes at each position */
    nodes[size - 1].run_length = 1;
    for (pos = size - 1; pos-- > 0; )
        nodes[pos].run_length = (buf[pos] == buf[pos + 1]) ? nodes[pos + 1].run_length + 1 : 1;

    /* Find optimal layout of compression packets, which is an optimal path through
     * the graph of position nodes.
     */
    for (pos = 0; pos < size; pos++) {
        const uint32_t  base     = nodes[pos].cost;
        const uint32_t *cur_reps = nodes[pos].reps;
        uint32_t        new_reps[4];
        uint32_t        chunk_id;
        uint32_t        depth;
        int             rep_idx;

        assert(base != INFINITE_COST);

        consider_packet(nodes, pos + 1, base + LIT_BITS, pos, PACKET_LIT, 0, 0, cur_reps);

        /* Encode reps for the 4 most recent distances */
        for (rep_idx = 0; rep_idx < 4; rep_idx++) {
            const uint32_t dist = cur_reps[rep_idx];
            uint32_t       match_length;

            if ( ! dist || (size_t)dist > pos)
                continue;

            match_length = calc_match_length(buf, pos - dist, pos, size);
            if ( ! match_length)
                continue;

            push_rep_distance(cur_reps, dist, new_reps);

            /* SHORTREP encodes length 1 against the most recent distance */
            if (rep_idx == 0)
                consider_packet(nodes, pos + 1, base + SHORTREP_BITS, pos, PACKET_REP, dist, 0, new_reps);

            if (match_length >= 2)
                consider_match(nodes, pos, base, match_length,
                               (rep_idx < 2) ? 4 : 5, 0, PACKET_REP, dist, (uint8_t)rep_idx, new_reps);
        }

        /* Last position cannot start a match */
        if (pos + 1 >= size)
            continue;

        /* Try to encode a distance-1 run (repeated byte) as a plain match, unless it is
         * already one of the rep distances (already handled above more cheaply).
         */
        if (pos >= 1 &&
            buf[pos] == buf[pos - 1] &&
            cur_reps[0] != 1 &&
            cur_reps[1] != 1 &&
            cur_reps[2] != 1 &&
            cur_reps[3] != 1) {

            uint32_t match_length = nodes[pos].run_length;
            if (match_length > MAX_LZA_SIZE)
                match_length = MAX_LZA_SIZE;

            push_rep_distance(cur_reps, 1, new_reps);
            consider_match(nodes, pos, base, match_length,
                           MATCH_BITS, calc_distance_bits(1), PACKET_MATCH, 1, 0, new_reps);
        }

        /* Try to encode matches which aren't covered by reps */
        chunk_id = map->pair_ids[get_map_idx(buf, pos)];
        depth    = 0;

        while (chunk_id != INVALID_ID && depth < MAX_CHAIN_DEPTH) {
            const LOCATION_CHUNK *chunk = &map->chunks[chunk_id];
            uint32_t              j;

            for (j = 0; j < MAX_OFFSETS; j++) {
                const uint32_t old_pos = chunk->offset[j];
                uint32_t       dist;
                uint32_t       match_length;

                if (old_pos == INVALID_ID)
                    continue;

                dist = (uint32_t)pos - old_pos;

                /* Skip reps as they were already considered earlier */
                if (dist == 1 ||
                    dist == cur_reps[0] ||
                    dist == cur_reps[1] ||
                    dist == cur_reps[2] ||
                    dist == cur_reps[3])
                    continue;

                match_length = calc_match_length(buf, old_pos, pos, size);
                if (match_length < 2)
                    continue;

                push_rep_distance(cur_reps, dist, new_reps);
                consider_match(nodes, pos, base, match_length,
                               MATCH_BITS, calc_distance_bits(dist), PACKET_MATCH, dist, 0, new_reps);

                if (++depth >= MAX_CHAIN_DEPTH)
                    break;
            }

            chunk_id = chunk->next_id;
        }

        set_offset(buf, pos, map);
    }

    /* Build a forward chain of packets by reversing the optimal reverse path
     * computed above.
     */
    for (pos = size; pos != 0; ) {
        const size_t from = nodes[pos].from_pos;
        nodes[from].next_pos = (uint32_t)pos;
        pos = from;
    }

    /* Encode packets */
    {
        size_t from      = 0;
        size_t lit_start = 0;
        size_t lit_count = 0;

        while (from != size) {
            const size_t   end          = nodes[from].next_pos;
            const uint32_t match_length = (uint32_t)(end - from);

            if (nodes[end].kind == PACKET_LIT) {
                if ( ! lit_count)
                    lit_start = from;
                lit_count += match_length;
            }
            else {
                OCCURRENCE occurrence;

                if (lit_count) {
                    report_literal(cookie, buf, lit_start, lit_count);
                    lit_count = 0;
                }

                occurrence.distance = nodes[end].packet_dist;
                occurrence.length   = match_length;
                occurrence.last     = (nodes[end].kind == PACKET_REP) ? nodes[end].last : -1;

                report_match(cookie, buf, from, occurrence);
            }

            from = end;
        }

        if (lit_count)
            report_literal(cookie, buf, lit_start, lit_count);
    }

    ret = 0;

cleanup:
    free(nodes);
    free(map);

    return ret;
}
