/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "pe_common.h"

int STDCALL loader(const LIVE_LAYOUT *layout)
{
    const char *import_names = (const char *)layout->iat;

    do {
        const char    *name = import_names++;
        FUNCTION_TYPE *func;
        MODULE_TYPE    module;

        do { } while (*(import_names++));

        module = layout->mini_iat->load_library(name);

        /* Note: assume little-endian */
        func = (FUNCTION_TYPE *)((uintptr_t)(layout->decomp_base) +
                                 (uint32_t)(uint8_t)import_names[0] +
                                 ((uint32_t)(uint8_t)import_names[1] << 8) +
                                 ((uint32_t)(uint8_t)import_names[2] << 16) +
                                 ((uint32_t)(uint8_t)import_names[3] << 24));
        import_names += 4;

        do {
            name = import_names++;

            do { } while (*(import_names++));

            *func = layout->mini_iat->get_proc_address(module, name);

            ++func;
        } while (*import_names);

        ++import_names;
    } while (*import_names);

    return layout->entry_point();
}
