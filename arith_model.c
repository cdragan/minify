/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#include "arith_model.h"

#include <assert.h>

void init_model(MODEL *model)
{
    const uint32_t hist_buckets = (uint32_t)(sizeof(model->history) / sizeof(model->history[0]));
    uint32_t       i;

    model->prob[0] = hist_buckets * 16 + 1;
    model->prob[1] = hist_buckets * 16 + 1;

    for (i = 0; i < hist_buckets; i++)
        model->history[i] = 0xAAAAAAAAU;
}

void update_model(MODEL *model, uint32_t bit)
{
    const uint32_t hist_buckets = (uint32_t)(sizeof(model->history) / sizeof(model->history[0]));
    uint32_t       i;

    assert(bit <= 1);

    ++model->prob[bit];

    for (i = 0; i < hist_buckets; i++) {

        const uint32_t old_bit = model->history[i] >> 31;

        model->history[i] = (model->history[i] << 1) | bit;

        bit = old_bit;
    }

    --model->prob[bit];
}
