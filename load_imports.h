/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include <stdint.h>

typedef void (* FUNCTION_TYPE)(void);
typedef void *MODULE_TYPE;
typedef MODULE_TYPE   (* LOAD_LIBRARY_A)(const char *);
typedef FUNCTION_TYPE (* GET_PROC_ADDRESS)(MODULE_TYPE, const char *);

typedef struct {
    const char      *import_names;
    uint8_t         *base_address;
    LOAD_LIBRARY_A   load_library;
    GET_PROC_ADDRESS get_proc_address;
} IMPORT_ADDRESS_TABLE;

#define IAT_INIT_IMPORT_NAMES     ((uintptr_t)0xCAFE01234560FACEULL)
#define IAT_INIT_BASE_ADDRESS     ((uintptr_t)0xCAFE01234561FACEULL)
#define IAT_INIT_LOAD_LIBRARY     ((uintptr_t)0xCAFE01234562FACEULL)
#define IAT_INIT_GET_PROC_ADDRESS ((uintptr_t)0xCAFE01234563FACEULL)

#define INIT_IMPORT_ADDRESS_TABLE {             \
    (const char *)IAT_INIT_IMPORT_NAMES,        \
    (uint8_t *)IAT_INIT_BASE_ADDRESS,           \
    (LOAD_LIBRARY_A)IAT_INIT_LOAD_LIBRARY,      \
    (GET_PROC_ADDRESS)IAT_INIT_GET_PROC_ADDRESS \
}
