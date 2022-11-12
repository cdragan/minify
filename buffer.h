/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t   size;
} BUFFER;

BUFFER buf_alloc(size_t size);

BUFFER buf_truncate(BUFFER buf, size_t pos);

BUFFER buf_get_tail(BUFFER buf, size_t pos);

BUFFER buf_slice(BUFFER buf, size_t pos, size_t size);

uint8_t *buf_at_offset(BUFFER buf, size_t pos, size_t size);
