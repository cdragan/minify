/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

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
    uint8_t         *dest;
    uint8_t         *decompressed;
    size_t           compr_buffer_size;
    size_t           decompr_buffer_size;

    if (argc != 2) {
        fprintf(stderr, "Error: Invalid arguments\n");
        fprintf(stderr, "Usage: minify <FILE>\n");
        return EXIT_FAILURE;
    }

    buf = load_file(argv[1]);
    if ( ! buf.size)
        return EXIT_FAILURE;

    compr_buffer_size   = estimate_compress_size(buf.size);
    decompr_buffer_size = buf.size * 3;

    dest = (uint8_t *)malloc(compr_buffer_size + decompr_buffer_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    decompressed = dest + compr_buffer_size;

    compressed = lza_compress(dest, compr_buffer_size, buf.buf, buf.size, 128);

    if ( ! compressed.lz)
        return EXIT_FAILURE;

    lza_decompress(decompressed,
                   buf.size,
                   decompr_buffer_size - buf.size,
                   dest,
                   compressed.compressed);

    if (memcmp(buf.buf, decompressed, buf.size)) {
        fprintf(stderr, "Decompressed output doesn't match input data\n");
        return EXIT_FAILURE;
    }

    printf("Original    %zu bytes\n", buf.size);
    printf("LZMA        %zu bytes\n", compressed.lz);
    printf("Entropy     %zu bytes (%zu%%)\n", compressed.compressed, compressed.compressed * 100 / buf.size);

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
