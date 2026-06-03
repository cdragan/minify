/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "lza_compress.h"

#include "arith_model.h"
#include "bit_emit.h"
#include "bit_ops.h"
#include "find_repeats.h"
#include "lza_context.h"

#include <stdint.h>
#include <stdlib.h>

size_t estimate_compress_size(size_t src_size)
{
    if (src_size < 4096)
        src_size = 4096;

    return src_size * 2;
}

typedef struct {
    MODEL             ctx[NUM_CONTEXT_BITS];
    BIT_EMITTER       emitter;
    uint32_t          low;
    uint32_t          high;
    uint32_t          num_pending;
    SYMBOL_BIT_COUNT *symbols;
    size_t            cur_symbol_idx;
    uint64_t          num_in_bits;
    uint64_t          num_out_bits;
    COMPRESSED_SIZES  sizes;
} ENCODER;

static void track_symbol_bits(ENCODER *encoder, size_t pos, uint64_t old_in_bits, uint64_t old_out_bits)
{
    SYMBOL_BIT_COUNT *const symbols = encoder->symbols;

    if ( ! symbols)
        return;

    while (encoder->cur_symbol_idx + 1 < symbols->num_symbols &&
           pos >= symbols->symbol_starts[encoder->cur_symbol_idx + 1])
        ++encoder->cur_symbol_idx;

    symbols->symbol_in_bit_counts[encoder->cur_symbol_idx]  += (size_t)(encoder->num_in_bits - old_in_bits);
    symbols->symbol_out_bit_counts[encoder->cur_symbol_idx] += (size_t)(encoder->num_out_bits - old_out_bits);
}

static void encode_bit(ENCODER *encoder, uint32_t ctx_idx, uint32_t bit)
{
    MODEL *const   model = &encoder->ctx[ctx_idx];
    const uint32_t prob0 = model->prob[0];
    const uint32_t prob1 = model->prob[1];
    const uint64_t range = (uint64_t)encoder->high - (uint64_t)encoder->low + 1;
    const uint32_t mid   = (uint32_t)((range * prob0) / (prob0 + prob1));

    ++encoder->num_in_bits;

    update_model(model, bit);

    if (bit)
        encoder->low += mid;
    else
        encoder->high = encoder->low + mid - 1;

    for (;;) {
        if (encoder->high < 0x80000000U || encoder->low >= 0x80000000U) {
            uint32_t out_bit = encoder->low >> 31;

            emit_bit(&encoder->emitter, out_bit);

            out_bit ^= 1;
            while (encoder->num_pending) {
                emit_bit(&encoder->emitter, out_bit);
                --encoder->num_pending;
            }
        }
        else if (encoder->low >= 0x40000000U && encoder->high < 0xC0000000U) {
            ++encoder->num_pending;
            encoder->low  &= ~0x40000000U;
            encoder->high |= 0x40000000U;
        }
        else
            break;

        /* Each renorm shift commits exactly one output bit: either an emitted
         * bit, or a pending bit whose value a later bit will resolve.  Counting
         * iterations therefore equals emitted_bits + num_pending at all times.
         */
        ++encoder->num_out_bits;

        encoder->low  = encoder->low << 1;
        encoder->high = (encoder->high << 1) + 1;
    }
}

/* Code an n-bit value, MSB first, through a bitwise context tree: a prefix-indexed
 * context model where the context for each bit is the tree node reached by the
 * higher bits already coded.  This lets a low bit's model depend on the high bits,
 * capturing intra-field patterns.
 */
static void encode_context_tree(ENCODER *encoder, uint32_t base, uint32_t value, uint32_t nbits)
{
    uint32_t node = 1;

    for ( ; nbits; nbits--) {
        const uint32_t bit = (value >> (nbits - 1)) & 1U;

        encode_bit(encoder, base + node, bit);
        node = (node << 1) | bit;
    }
}

/* Code an n-bit value MSB first with one context per bit position.  Used for
 * the near-random tails (distance mantissa, long-length tail) where the higher
 * bits do not predict the lower ones, so a context tree would not help.
 */
static void encode_pos(ENCODER *encoder, uint32_t base, uint32_t value, uint32_t nbits)
{
    uint32_t i;

    for (i = 0; i < nbits; i++)
        encode_bit(encoder, base + i, (value >> (nbits - 1 - i)) & 1U);
}

static void encode_length(ENCODER *encoder, uint32_t length)
{
    if (length <= 9) {
        encode_bit(encoder, CTX_LEN_SEL_POS, 0);
        encode_context_tree(encoder, CTX_LEN_SHORT_POS, length - 2, 3);
    }
    else if (length <= 17) {
        encode_bit(encoder, CTX_LEN_SEL_POS, 1);
        encode_bit(encoder, CTX_LEN_SEL_POS + 1, 0);
        encode_context_tree(encoder, CTX_LEN_MID_POS, length - 10, 3);
    }
    else {
        encode_bit(encoder, CTX_LEN_SEL_POS, 1);
        encode_bit(encoder, CTX_LEN_SEL_POS + 1, 1);
        encode_pos(encoder, CTX_LEN_TAIL_POS, length - 18, 11);
    }
}

static void encode_distance(ENCODER *encoder, uint32_t distance)
{
    const uint32_t dd = distance - 1;

    if (dd < 2)
        encode_context_tree(encoder, CTX_DIST_SLOT_POS, dd, 6);
    else {

        const uint32_t bits_m1   = 31U - (uint32_t)count_leading_zeroes(dd);
        const uint32_t mant_bits = bits_m1 - 1;
        const uint32_t top       = (dd >> mant_bits) & 1U;

        encode_context_tree(encoder, CTX_DIST_SLOT_POS, (bits_m1 << 1) | top, 6);
        encode_pos(encoder, CTX_DIST_MANTISSA_POS, dd & ((1U << mant_bits) - 1U), mant_bits);
    }
}

static void report_literal(void *cookie, const uint8_t *buf, size_t pos, size_t size)
{
    ENCODER *const encoder = (ENCODER *)cookie;

    do {
        const uint64_t old_in_bits  = encoder->num_in_bits;
        const uint64_t old_out_bits = encoder->num_out_bits;

        encode_bit(encoder, CTX_TYPE_POS, 0);
        encode_context_tree(encoder, CTX_LIT_POS, buf[pos], 8);
        track_symbol_bits(encoder, pos, old_in_bits, old_out_bits);
        ++encoder->sizes.stats_lit;
        ++pos;
    } while (--size);
}

static void report_match(void *cookie, const uint8_t *buf, size_t pos, OCCURRENCE occurrence)
{
    ENCODER *const encoder      = (ENCODER *)cookie;
    const uint64_t old_in_bits  = encoder->num_in_bits;
    const uint64_t old_out_bits = encoder->num_out_bits;

    (void)buf;

    encode_bit(encoder, CTX_TYPE_POS, 1);

    if (occurrence.last < 0) {
        encode_bit(encoder, CTX_TYPE_POS + 1, 0);
        encode_length(encoder, occurrence.length);
        encode_distance(encoder, occurrence.distance);
        ++encoder->sizes.stats_match;
    }
    else if (occurrence.length == 1) {
        encode_bit(encoder, CTX_TYPE_POS + 1, 1);
        encode_bit(encoder, CTX_TYPE_POS + 2, 0);
        encode_bit(encoder, CTX_TYPE_POS + 3, 0);
        ++encoder->sizes.stats_shortrep;
    }
    else {
        /* LONGREP0->1 LONGREP1->2 LONGREP2/3->3, with a 3rd bit to pick 2 vs 3 */
        const uint32_t selector = (occurrence.last <= 1) ? (uint32_t)(occurrence.last + 1) : 3U;

        encode_bit(encoder, CTX_TYPE_POS + 1, 1);
        encode_bit(encoder, CTX_TYPE_POS + 2, (selector >> 1) & 1U);
        encode_bit(encoder, CTX_TYPE_POS + 3, selector & 1U);
        if (selector == 3)
            encode_bit(encoder, CTX_TYPE_POS + 4, occurrence.last == 3);
        encode_length(encoder, occurrence.length);
        ++encoder->sizes.stats_longrep[occurrence.last];
    }

    track_symbol_bits(encoder, pos, old_in_bits, old_out_bits);
}

COMPRESSED_SIZES lza_compress(void *dest, size_t dest_size, const void *src, size_t src_size,
                              SYMBOL_BIT_COUNT *symbols)
{
    ENCODER         *encoder = (ENCODER *)malloc(sizeof(*encoder));
    COMPRESSED_SIZES sizes   = { 0, 0, 0, 0, { 0, 0, 0, 0 } };
    uint32_t         i;
    uint32_t         out_bit;

    if ( ! encoder)
        return sizes;

    for (i = 0; i < NUM_CONTEXT_BITS; i++)
        init_model(&encoder->ctx[i]);

    init_bit_emitter(&encoder->emitter, (uint8_t *)dest, dest_size);
    encoder->low            = 0;
    encoder->high           = ~0U;
    encoder->num_pending    = 0;
    encoder->symbols        = symbols;
    encoder->cur_symbol_idx = 0;
    encoder->num_in_bits    = 0;
    encoder->num_out_bits   = 0;
    encoder->sizes          = sizes;

    if (find_repeats((const uint8_t *)src, src_size, report_literal, report_match, encoder)) {
        free(encoder);
        return sizes;
    }

    /* Flush: emit the final bit and one opposite bit; the decoder duplicates the
     * last bit, so a single extra bit is enough to terminate the range.
     */
    out_bit = (encoder->low >= 0x40000000U) ? 1 : 0;
    emit_bit(&encoder->emitter, out_bit);
    emit_bit(&encoder->emitter, out_bit ^ 1);

    sizes            = encoder->sizes;
    sizes.compressed = emit_tail(&encoder->emitter);

    free(encoder);
    return sizes;
}
