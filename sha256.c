/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 *
 * SHA-256 (FIPS 180-4).  Self-contained so the Mach-O ad-hoc signer can run
 * on any host, not just where /usr/bin/codesign exists.
 */

#include "sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr(uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static uint32_t choose(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static uint32_t majority(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t big_sigma0(uint32_t x)
{
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

static uint32_t big_sigma1(uint32_t x)
{
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

static uint32_t small_sigma0(uint32_t x)
{
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static uint32_t small_sigma1(uint32_t x)
{
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

static void sha256_transform(SHA256_CTX *ctx, const uint8_t *block)
{
    uint32_t schedule[64];
    uint32_t work[8];
    uint32_t round_idx;

    for (round_idx = 0; round_idx < 16U; round_idx++) {
        const uint32_t base = round_idx * 4U;
        schedule[round_idx] = ((uint32_t)block[base]     << 24) |
                              ((uint32_t)block[base + 1] << 16) |
                              ((uint32_t)block[base + 2] <<  8) |
                               (uint32_t)block[base + 3];
    }
    for (round_idx = 16U; round_idx < 64U; round_idx++) {
        schedule[round_idx] = small_sigma1(schedule[round_idx - 2]) +
                              schedule[round_idx - 7] +
                              small_sigma0(schedule[round_idx - 15]) +
                              schedule[round_idx - 16];
    }

    for (round_idx = 0; round_idx < 8U; round_idx++) {
        work[round_idx] = ctx->state[round_idx];
    }

    for (round_idx = 0; round_idx < 64U; round_idx++) {
        const uint32_t temp1 = work[7] + big_sigma1(work[4]) +
                               choose(work[4], work[5], work[6]) +
                               round_constants[round_idx] + schedule[round_idx];
        const uint32_t temp2 = big_sigma0(work[0]) + majority(work[0], work[1], work[2]);

        work[7] = work[6];
        work[6] = work[5];
        work[5] = work[4];
        work[4] = work[3] + temp1;
        work[3] = work[2];
        work[2] = work[1];
        work[1] = work[0];
        work[0] = temp1 + temp2;
    }

    for (round_idx = 0; round_idx < 8U; round_idx++) {
        ctx->state[round_idx] += work[round_idx];
    }
}

void sha256_init(SHA256_CTX *ctx)
{
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bit_length = 0;
    ctx->block_used = 0;
}

void sha256_update(SHA256_CTX *ctx, const void *buf, size_t size)
{
    const uint8_t *input = (const uint8_t *)buf;

    ctx->bit_length += (uint64_t)size * 8U;

    while (size > 0U) {
        const size_t room = SHA256_BLOCK_SIZE - ctx->block_used;
        const size_t take = size < room ? size : room;

        memcpy(ctx->block + ctx->block_used, input, take);
        ctx->block_used += (uint32_t)take;
        input += take;
        size  -= take;

        if (ctx->block_used == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
}

void sha256_finish(SHA256_CTX *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    const uint64_t total_bits = ctx->bit_length;
    uint32_t       i;

    /* Append the 0x80 terminator, then zero-pad so that exactly 8 bytes
     * remain in the final block for the 64-bit big-endian length.
     */
    ctx->block[ctx->block_used++] = 0x80U;
    if (ctx->block_used > SHA256_BLOCK_SIZE - 8U) {
        while (ctx->block_used < SHA256_BLOCK_SIZE) {
            ctx->block[ctx->block_used++] = 0;
        }
        sha256_transform(ctx, ctx->block);
        ctx->block_used = 0;
    }
    while (ctx->block_used < SHA256_BLOCK_SIZE - 8U) {
        ctx->block[ctx->block_used++] = 0;
    }
    for (i = 0; i < 8U; i++) {
        ctx->block[SHA256_BLOCK_SIZE - 1U - i] = (uint8_t)(total_bits >> (i * 8U));
    }
    sha256_transform(ctx, ctx->block);

    for (i = 0; i < 8U; i++) {
        digest[i * 4U]      = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >>  8);
        digest[i * 4U + 3U] = (uint8_t)(ctx->state[i]);
    }
}

void sha256(const void *buf, size_t size, uint8_t digest[SHA256_DIGEST_SIZE])
{
    SHA256_CTX ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, buf, size);
    sha256_finish(&ctx, digest);
}
