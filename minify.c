/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "exe_pe.h"
#include "lza_compress.h"
#include "lza_decompress.h"
#include "load_file.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save_file(const char *filename, BUFFER buf)
{
    static char new_filename[1024];
    static char prefix[] = "mini.";
    FILE       *file;
    const char *slash;
    size_t      len;

    len = strlen(filename);

    slash = strrchr(filename, '/');

#ifdef _WIN32
    {
        const char *const backslash = strrchr(filename, '\\');

        if ( ! slash || (backslash > slash))
            slash = backslash;
    }
#endif

    if (len + sizeof(prefix) > sizeof(new_filename)) {
        fprintf(stderr, "Error: File name %s is too long\n", filename);
        return EXIT_FAILURE;
    }

    snprintf(new_filename, sizeof(new_filename), "%.*s%s%s",
             (int)(slash ? (slash - filename + 1) : 0),
             filename,
             prefix,
             slash ? (slash + 1) : filename);

    file = fopen(new_filename, "wb");
    if ( ! file) {
        perror(new_filename);
        return EXIT_FAILURE;
    }

    len = fwrite(buf.buf, 1, buf.size, file);
    if (len != buf.size && ferror(file)) {
        fprintf(stderr, "Error: Failed to write to file %s\n", new_filename);
        fclose(file);
        return EXIT_FAILURE;
    }

    fclose(file);

    printf("Saved compressed executable in %s\n", new_filename);

    return EXIT_SUCCESS;
}

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

    if (is_pe_file(buf.buf, buf.size)) {
        int    err    = EXIT_SUCCESS;
        BUFFER output = exe_pe(buf.buf, buf.size);
        if ( ! output.buf)
            err = EXIT_FAILURE;
        else
            err = save_file(argv[1], output);

        if ( ! err)
            printf("Compressed %zu -> %zu (%zu %%)\n",
                   buf.size, output.size, output.size * 100 / buf.size);

        if (output.buf)
            free(output.buf);
        free(buf.buf);

        return err;
    }

    compr_buffer_size   = estimate_compress_size(buf.size);
    decompr_buffer_size = buf.size * 3;

    dest = (uint8_t *)malloc(compr_buffer_size + decompr_buffer_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    decompressed = dest + compr_buffer_size;

    compressed = lza_compress(dest, compr_buffer_size, buf.buf, buf.size);

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
    printf("LZ77        %zu bytes\n", compressed.lz);
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
