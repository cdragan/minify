/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "load_imports.h"

#ifdef _WIN32
#define main __stdcall WinMainCRTStartup
#endif

int main(void)
{
    static IMPORT_ADDRESS_TABLE import_address_table = INIT_IMPORT_ADDRESS_TABLE;

    const char *import_names = import_address_table.import_names;

    do {
        const char    *name = import_names++;
        FUNCTION_TYPE *func;
        MODULE_TYPE    module;

        do { } while (*(import_names++));

        module = import_address_table.load_library(name);

        /* Note: assume little-endian */
        func = (FUNCTION_TYPE *)(import_address_table.base_address +
                                 (uint32_t)(uint8_t)import_names[0] +
                                 ((uint32_t)(uint8_t)import_names[1] << 8) +
                                 ((uint32_t)(uint8_t)import_names[2] << 16) +
                                 ((uint32_t)(uint8_t)import_names[3] << 24));
        import_names += 4;

        do {
            name = import_names++;

            do { } while (*(import_names++));

            *func = import_address_table.get_proc_address(module, name);

            ++func;
        } while (*import_names);

        ++import_names;
    } while (*import_names);
}
