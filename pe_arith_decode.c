/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_decode.h"
#include "pe_common.h"

const LIVE_LAYOUT *live_layout = (LIVE_LAYOUT *)(uintptr_t)0xFACECAFEBEEFF00D;

int STDCALL loader(void)
{
    arith_decode(live_layout->lz77_data, live_layout->lz77_data_size,
                 live_layout->comp_data, live_layout->comp_data_size);

    return live_layout->lz77_decomp(live_layout);
}
