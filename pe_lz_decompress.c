/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "lza_decompress.h"
#include "pe_common.h"

int STDCALL loader(const LIVE_LAYOUT *layout)
{
    lz_decompress(layout->image_base,
                  (uint32_t)(layout->lz77_data - layout->image_base),
                  layout->lz77_data);

    return layout->import_loader(layout);
}
