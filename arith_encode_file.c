/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_encode.h"
#include "load_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    BUFFER buf;
    char  *dest;
    char  *end = NULL;
    long   window_size;
    size_t dest_size;
    size_t actual_size;

    if (argc != 3) {
        fprintf(stderr, "Error: Invalid arguments\n");
        fprintf(stderr, "Usage: arith_encode <FILE> <WINDOW_SIZE>\n");
        return EXIT_FAILURE;
    }

    window_size = strtol(argv[2], &end, 0);
    if ( ! end || *end || (window_size == 0 && errno) || (window_size <= 0) || (window_size > MAX_WINDOW_SIZE)) {
        fprintf(stderr, "Invalid window size: %ld\n", window_size);
        return EXIT_FAILURE;
    }

    buf = load_file(argv[1]);
    if ( ! buf.size)
        return EXIT_FAILURE;

    dest_size = buf.size * 2;
    dest      = (char *)malloc(dest_size);
    if ( ! dest) {
        perror(NULL);
        return EXIT_FAILURE;
    }

    actual_size = arith_encode(dest, dest_size, buf.buf, buf.size, (uint32_t)window_size);

    printf("Input:  %zu bytes\n", buf.size);
    printf("Output: %zu bytes\n", actual_size);

    free(buf.buf);

    return EXIT_SUCCESS;
}
