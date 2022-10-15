/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>

typedef struct {
    char  *buf;
    size_t size;
} BUFFER;

BUFFER load_file(const char *filename);
