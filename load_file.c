/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "load_file.h"

#include <stdio.h>
#include <stdlib.h>

BUFFER load_file(const char *filename)
{
    FILE  *file;
    BUFFER buf = { NULL, 0 };
    long   size;

    file = fopen(filename, "rb");
    if ( ! file) {
        perror(filename);
        return buf;
    }

    if (fseek(file, 0, SEEK_END)) {
        perror(filename);
        fclose(file);
        return buf;
    }

    size = ftell(file);
    if (size < 0) {
        perror(filename);
        fclose(file);
        return buf;
    }

    if (size == 0) {
        fprintf(stderr, "%s: empty file\n", filename);
        fclose(file);
        return buf;
    }

    if (fseek(file, 0, SEEK_SET)) {
        perror(filename);
        fclose(file);
        return buf;
    }

    buf.buf = (char *)malloc((size_t)size);
    if ( ! buf.buf) {
        perror(NULL);
        fclose(file);
        return buf;
    }

    if (fread(buf.buf, 1, size, file) != size) {
        perror(filename);
        fclose(file);

        free(buf.buf);
        buf.buf = NULL;
        return buf;
    }

    fclose(file);

    buf.size = (size_t)size;

    return buf;
}
