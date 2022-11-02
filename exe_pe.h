/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>

int is_pe_file(const void *buf, size_t size);
int exe_pe(const void *buf, size_t size);
