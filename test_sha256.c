/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int check(const char *label, const void *buf, size_t size, const char *expect_hex)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    char    got_hex[SHA256_DIGEST_SIZE * 2U + 1U];
    uint32_t i;

    sha256(buf, size, digest);
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        snprintf(got_hex + i * 2U, 3U, "%02x", digest[i]);
    }
    if (strcmp(got_hex, expect_hex) != 0) {
        fprintf(stderr, "FAIL %s:\n  got      %s\n  expected %s\n", label, got_hex, expect_hex);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const char long_msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    SHA256_CTX ctx;
    uint8_t    digest[SHA256_DIGEST_SIZE];
    char       got_hex[SHA256_DIGEST_SIZE * 2U + 1U];
    uint32_t   i;
    int        failures = 0;

    failures += check("empty", "", 0,
                      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    failures += check("abc", "abc", 3,
                      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    failures += check("two-block", long_msg, strlen(long_msg),
                      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* Incremental update must match the one-shot result for "abc". */
    sha256_init(&ctx);
    sha256_update(&ctx, "a", 1);
    sha256_update(&ctx, "b", 1);
    sha256_update(&ctx, "c", 1);
    sha256_finish(&ctx, digest);
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        snprintf(got_hex + i * 2U, 3U, "%02x", digest[i]);
    }
    if (strcmp(got_hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        fprintf(stderr, "FAIL incremental abc:\n  got %s\n", got_hex);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "test_sha256: %d failure(s)\n", failures);
        return 1;
    }

    return 0;
}
