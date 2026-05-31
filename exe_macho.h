/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "buffer.h"

int    is_macho_file(const void *buf, size_t size);
BUFFER exe_macho(const void *buf, size_t size, BUFFER map);
int    macho_set_executable(const char *path);
