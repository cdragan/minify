/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include <stdint.h>

typedef struct {
    uint32_t prob[2];
    uint32_t history[2];
} MODEL;

void init_model(MODEL *model);
void update_model(MODEL *model, uint32_t bit);
