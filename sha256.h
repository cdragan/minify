/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32U
#define SHA256_BLOCK_SIZE  64U

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t  block[SHA256_BLOCK_SIZE];
    uint32_t block_used;
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const void *buf, size_t size);
void sha256_finish(SHA256_CTX *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

void sha256(const void *buf, size_t size, uint8_t digest[SHA256_DIGEST_SIZE]);
