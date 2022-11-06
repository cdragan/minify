/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stddef.h>
#include <stdint.h>

#define MAX_WINDOW_SIZE 2048

typedef struct {
    uint32_t prob[2];
    uint64_t history;
} MODEL;

void init_model(MODEL *model);
void update_model(MODEL *model, uint32_t bit);
void arith_decode(void *dest, size_t dest_size, const void *src, size_t src_size);
