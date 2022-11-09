/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stdint.h>

#ifdef _WIN32
#define STDCALL __stdcall
#else
#define STDCALL
#endif

typedef void (* FUNCTION_TYPE)(void);
typedef void *MODULE_TYPE;
typedef MODULE_TYPE   (* LOAD_LIBRARY_A)(const char *);
typedef FUNCTION_TYPE (* GET_PROC_ADDRESS)(MODULE_TYPE, const char *);

typedef struct {
    LOAD_LIBRARY_A   load_library;
    GET_PROC_ADDRESS get_proc_address;
} MINI_IAT;

typedef struct LIVE_LAYOUT_STRUCT LIVE_LAYOUT;

typedef int (* ENTRY_POINT)(void);
typedef int (* LOADER)(const LIVE_LAYOUT *layout);

/* This structure is used by various loaders to locate specific blocks.  It is stored
 * in the executable file uncompressed and it is referenced directly.
 */
struct LIVE_LAYOUT_STRUCT {
    uint8_t       *image_base;
    ENTRY_POINT    entry_point;
    uint8_t       *iat;
    LOADER         import_loader;
    uint8_t       *lz77_data;
    LOADER         lz77_decomp;
    const uint8_t *comp_data;
    MINI_IAT      *mini_iat;
    uint32_t       lz77_data_size;
    uint32_t       comp_data_size;
};
