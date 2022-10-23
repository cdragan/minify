/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_encode.h"
#include "bit_stream.h"
#include "find_repeats.h"
#include "lza_compress.h"
#include "lza_decompress.h"
#include "load_file.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    COMPRESSED_SIZES compressed;
    BUFFER           buf;
    char            *dest;
    char            *decompressed;
    char            *entropy;
    size_t           alloc_size;
    size_t           dest_size;
    size_t           entropy_size;

    if (argc != 2) {
        fprintf(stderr, "Error: Invalid arguments\n");
        fprintf(stderr, "Usage: minify <FILE>\n");
        return EXIT_FAILURE;
    }

    buf = load_file(argv[1]);
    if ( ! buf.size)
        return EXIT_FAILURE;

    dest_size  = (buf.size < 4096 ? 4096 : buf.size) * 110 / 100;
    alloc_size = dest_size + buf.size * (400 + 100) / 100;
    dest = (char *)malloc(alloc_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    entropy      = dest + dest_size;
    decompressed = dest + dest_size + buf.size * 400 / 100;

    compressed = compress(dest, dest_size, buf.buf, buf.size);

    if ( ! compressed.total)
        return EXIT_FAILURE;

    dest_size = compressed.total;

    decompress(decompressed,
               buf.size,
               dest,
               compressed.types,
               compressed.literals,
               compressed.sizes,
               compressed.offsets);

    if (memcmp(buf.buf, decompressed, buf.size)) {
        fprintf(stderr, "Decompressed output doesn't match input data\n");
        return EXIT_FAILURE;
    }

    entropy_size = arith_encode(entropy, buf.size * 400 / 100, dest, dest_size, 128);

    printf("Original    %zu bytes\n", buf.size);
    printf("LZMA        %zu bytes\n", dest_size);
    printf("Entropy     %zu bytes (%zu%%)\n", entropy_size, entropy_size * 100 / buf.size);

    printf("LIT         %zu\n", compressed.stats_lit);
    printf("MATCH       %zu\n", compressed.stats_match);
    printf("SHORTREP    %zu\n", compressed.stats_shortrep);
    printf("LONGREP0    %zu\n", compressed.stats_longrep[0]);
    printf("LONGREP1    %zu\n", compressed.stats_longrep[1]);
    printf("LONGREP2    %zu\n", compressed.stats_longrep[2]);
    printf("LONGREP3    %zu\n", compressed.stats_longrep[3]);

    free(dest);
    free(buf.buf);

    return EXIT_SUCCESS;
}
