/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "exe_pe.h"
#include "buffer.h"
#include "lza_compress.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t bytes[2];
} uint16_le;

typedef struct {
    uint8_t bytes[4];
} uint32_le;

typedef struct {
    uint8_t bytes[8];
} uint64_le;

uint16_t get_uint16_le(uint16_le data)
{
    return (uint16_t)((uint32_t)data.bytes[0] + ((uint32_t)data.bytes[1] << 8));
}

uint32_t get_uint32_le(uint32_le data)
{
    return (uint32_t)data.bytes[0] +
           ((uint32_t)data.bytes[1] << 8) +
           ((uint32_t)data.bytes[2] << 16) +
           ((uint32_t)data.bytes[3] << 24);
}

uint64_t get_uint64_le(uint64_le data)
{
    return (uint64_t)data.bytes[0] +
           ((uint64_t)data.bytes[1] << 8) +
           ((uint64_t)data.bytes[2] << 16) +
           ((uint64_t)data.bytes[3] << 24) +
           ((uint64_t)data.bytes[4] << 32) +
           ((uint64_t)data.bytes[5] << 40) +
           ((uint64_t)data.bytes[6] << 48) +
           ((uint64_t)data.bytes[7] << 56);
}

typedef struct {
    uint16_le mz_signature;
    uint8_t   garbage[0x3A];
    uint32_le pe_offset;
} DOS_HEADER;

typedef struct {
    uint32_le pe_signature;
    uint16_le machine;
    uint16_le number_of_sections;
    uint32_le time_date_stamp;
    uint32_le symbol_table_offset;
    uint32_le number_of_symbols;
    uint16_le optional_hdr_size;
    uint16_le flags;
} PE_HEADER;

typedef struct {
    uint32_le virtual_address;
    uint32_le size;
} DATA_DIRECTORY;

typedef struct {
    uint16_le pe_format; /* PE_FORMAT_PE32=0x010B */
    uint8_t   linker_ver_major;
    uint8_t   linker_ver_minor;
    uint32_le size_of_code;
    uint32_le size_of_data;
    uint32_le size_of_uninitialized_data;
    uint32_le address_of_entry_point;
    uint32_le base_of_code;
    uint32_le base_of_data;
    uint32_le image_base;
    uint32_le section_alignment;
    uint32_le file_alignment;
    uint16_le min_os_ver_major;
    uint16_le min_os_ver_minor;
    uint16_le image_ver_major;
    uint16_le image_ver_minor;
    uint16_le subsystem_ver_major;
    uint16_le subsystem_ver_minor;
    uint32_le reserved_win32_version;
    uint32_le size_of_image;
    uint32_le size_of_headers;
    uint32_le checksum;
    uint16_le subsystem;
    uint16_le dll_flags;
    uint32_le size_of_stack_reserve;
    uint32_le size_of_stack_commit;
    uint32_le size_of_head_reserve;
    uint32_le size_of_heap_commit;
    uint32_le reserved_loader_flags;
    uint32_le number_of_rva_and_sizes;
    DATA_DIRECTORY rva_and_sizes[1];
} PE32_HEADER;

typedef struct {
    uint16_le pe_format; /* PE_FORMAT_PE32_PLUS=0x020B */
    uint8_t   linker_ver_major;
    uint8_t   linker_ver_minor;
    uint32_le size_of_code;
    uint32_le size_of_data;
    uint32_le size_of_uninitialized_data;
    uint32_le address_of_entry_point;
    uint32_le base_of_code;
    uint64_le image_base;
    uint32_le section_alignment;
    uint32_le file_alignment;
    uint16_le min_os_ver_major;
    uint16_le min_os_ver_minor;
    uint16_le image_ver_major;
    uint16_le image_ver_minor;
    uint16_le subsystem_ver_major;
    uint16_le subsystem_ver_minor;
    uint32_le reserved_win32_version;
    uint32_le size_of_image;
    uint32_le size_of_headers;
    uint32_le checksum;
    uint16_le subsystem;
    uint16_le dll_flags;
    uint64_le size_of_stack_reserve;
    uint64_le size_of_stack_commit;
    uint64_le size_of_head_reserve;
    uint64_le size_of_heap_commit;
    uint32_le reserved_loader_flags;
    uint32_le number_of_rva_and_sizes;
    DATA_DIRECTORY rva_and_sizes[1];
} PE32_PLUS_HEADER;

typedef struct {
    char      name[8];
    uint32_le virtual_size;
    uint32_le virtual_address;
    uint32_le size_of_raw_data;
    uint32_le pointer_to_raw_data;
    uint32_le pointer_to_relocations;
    uint32_le pointer_to_line_numbers;
    uint16_le number_of_relocations;
    uint16_le number_of_line_numbers;
    uint32_le flags;
} SECTION_HEADER;

typedef struct {
    uint32_le import_lookup_table_rva;
    uint32_le time_stamp;
    uint32_le forwarder_chain;
    uint32_le name_rva;
    uint32_le import_address_table_rva;
} IMPORT_DIR_ENTRY;

#define PE_MACHINE_X86_32  0x014C
#define PE_MACHINE_X86_64  0x8664
#define PE_MACHINE_AARCH64 0xAA64

#define CHAR_RELOCS_STRIPPED     0x0001
#define CHAR_EXECUTABLE_IMAGE    0x0002
#define CHAR_LARGE_ADDRESS_AWARE 0x0020
#define CHAR_32BIT_MACHINE       0x0100
#define CHAR_DLL                 0x2000
#define CHAR_UNSUPPORTED         0xD09C

#define PE_FORMAT_PE32      0x010B
#define PE_FORMAT_PE32_PLUS 0x020B

#define SUBSYSTEM_GUI 0x02
#define SUBSYSTEM_CUI 0x03

#define DIR_EXPORT_TABLE            0
#define DIR_IMPORT_TABLE            1
#define DIR_RESOURCE_TABLE          2
#define DIR_EXCEPTION_TABLE         3
#define DIR_CERTIFICATE_TABLE       4
#define DIR_BASE_RELOCATION_TABLE   5
#define DIR_DEBUG                   6
#define DIR_ARCHITECTURE            7
#define DIR_GLOBAL_PTR              8
#define DIR_TLS_TABLE               9
#define DIR_LOAD_CONFIG_TABLE       10
#define DIR_BOUND_IMPORT            11
#define DIR_IAT                     12
#define DIR_DELAY_IMPORT_DESCRIPTOR 13
#define DIR_CLR_RUNTIME_HEADER      14
#define DIR_RESERVED                15

#define SECTION_TYPE_NO_PAD            0x00000008U
#define SECTION_CNT_CODE               0x00000020U
#define SECTION_CNT_INITIALIZED_DATA   0x00000040U
#define SECTION_CNT_UNINITIALIZED_DATA 0x00000080U
#define SECTION_LNK_INFO               0x00000200U
#define SECTION_LNK_REMOVE             0x00000800U
#define SECTION_LNK_COMDAT             0x00001000U
#define SECTION_GPREL                  0x00008000U
#define SECTION_ALIGN_1BYTES           0x00100000U
#define SECTION_ALIGN_2BYTES           0x00200000U
#define SECTION_ALIGN_4BYTES           0x00300000U
#define SECTION_ALIGN_8BYTES           0x00400000U
#define SECTION_ALIGN_16BYTES          0x00500000U
#define SECTION_ALIGN_32BYTES          0x00600000U
#define SECTION_ALIGN_64BYTES          0x00700000U
#define SECTION_ALIGN_128BYTES         0x00800000U
#define SECTION_ALIGN_256BYTES         0x00900000U
#define SECTION_ALIGN_512BYTES         0x00A00000U
#define SECTION_ALIGN_1024BYTES        0x00B00000U
#define SECTION_ALIGN_2048BYTES        0x00C00000U
#define SECTION_ALIGN_4096BYTES        0x00D00000U
#define SECTION_ALIGN_8192BYTES        0x00E00000U
#define SECTION_LNK_NRELOC_OVFL        0x01000000U
#define SECTION_MEM_DISCARDABLE        0x02000000U
#define SECTION_MEM_NOT_CACHED         0x04000000U
#define SECTION_MEM_NOT_PAGED          0x08000000U
#define SECTION_MEM_SHARED             0x10000000U
#define SECTION_MEM_EXECUTE            0x20000000U
#define SECTION_MEM_READ               0x40000000U
#define SECTION_MEM_WRITE              0x80000000U

static void append_str(char *buf, size_t size, const char *str)
{
    const size_t buf_len  = strlen(buf);
    const size_t str_len  = strlen(str) + 1; /* Include terminating NUL */
    const size_t num_left = size - buf_len;

    if (num_left >= str_len)
        memcpy(buf + buf_len, str, str_len);
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return ((value - 1) / align + 1) * align;
}

static void decode_section_flags(char *buf, size_t size, uint32_t flags)
{
    buf[0] = 0;

    if (flags & SECTION_MEM_READ)
        append_str(buf, size, " read");
    if (flags & SECTION_MEM_WRITE)
        append_str(buf, size, " write");
    if (flags & SECTION_MEM_EXECUTE)
        append_str(buf, size, " exec");
    if (flags & SECTION_MEM_DISCARDABLE)
        append_str(buf, size, " discard");
    if (flags & SECTION_CNT_CODE)
        append_str(buf, size, " code");
    if (flags & SECTION_CNT_INITIALIZED_DATA)
        append_str(buf, size, " data");
    if (flags & SECTION_CNT_UNINITIALIZED_DATA)
        append_str(buf, size, " udata");

    flags &= ~(SECTION_MEM_READ | SECTION_MEM_WRITE | SECTION_MEM_EXECUTE | SECTION_MEM_DISCARDABLE |
               SECTION_CNT_CODE | SECTION_CNT_INITIALIZED_DATA | SECTION_CNT_UNINITIALIZED_DATA);

    if (flags)
        append_str(buf, size, " unknown");
}

static const void *at_offset(const void *buf, uint32_t offset)
{
    return (const uint8_t *)buf + offset;
}

static uint32_t get_pe_offset(const void *buf, size_t size)
{
    const DOS_HEADER *dos_header;
    const PE_HEADER  *pe_header;
    uint32_t          pe_offset;

    if (size < sizeof(DOS_HEADER))
        return 0;

    dos_header = (const DOS_HEADER *)buf;

    /* "MZ" signature */
    if (get_uint16_le(dos_header->mz_signature) != 0x5A4D)
        return 0;

    pe_offset = get_uint32_le(dos_header->pe_offset);

    if (pe_offset + sizeof(PE_HEADER) > size)
        return 0;

    pe_header = (const PE_HEADER *)at_offset(buf, pe_offset);

    /* "PE" signature */
    if (get_uint32_le(pe_header->pe_signature) != 0x4550)
        return 0;

    return pe_offset;
}

static size_t push_iat_data(BUFFER *buf, const void *data, size_t size)
{
    if (size <= buf->size) {
        memcpy(buf->buf, data, size);

        buf->buf  += size;
        buf->size -= size;
    }

    return size;
}

static size_t push_iat_string(BUFFER *buf, const char *str)
{
    return push_iat_data(buf, str, strlen(str) + 1);
}

static size_t push_iat_uint32(BUFFER *buf, uint32_t value)
{
    /* Note: assumes little-endian! */
    return push_iat_data(buf, &value, 4);
}

static size_t push_iat_byte(BUFFER *buf, uint8_t byte)
{
    return push_iat_data(buf, &byte, 1);
}

static void clear_address_table(BUFFER process_va, uint32_t rva, int is_64bit)
{
    const uint32_t entry_size = is_64bit ? 8 : 4;

    for (;;) {
        uint64_t value;
        uint8_t *entry = buf_at_offset(process_va, rva, entry_size);

        if (is_64bit)
            value = get_uint64_le(*(const uint64_le *)entry);
        else
            value = get_uint32_le(*(const uint32_le *)entry);

        memset(entry, 0, entry_size);
        rva += entry_size;

        if ( ! value)
            break;

        entry = buf_at_offset(process_va, value, 3);

        memset(entry, 0, 2 + strlen((char *)entry + 2));
    }
}

static int process_import_table(BUFFER  process_va,
                                BUFFER  import_table,
                                BUFFER *iat_data,
                                int     is_64bit)
{
    IMPORT_DIR_ENTRY       *import_dir_entry = (IMPORT_DIR_ENTRY *)import_table.buf;
    IMPORT_DIR_ENTRY *const import_table_end = (IMPORT_DIR_ENTRY *)(import_table.buf + import_table.size);
    BUFFER                  iat_buf_left     = *iat_data;
    size_t                  iat_size         = 0;

    /* Process each DLL */
    for ( ; import_dir_entry + 1 <= import_table_end; import_dir_entry++) {
        const uint32_t forwarder_chain          = get_uint32_le(import_dir_entry->forwarder_chain);
        const uint32_t name_rva                 = get_uint32_le(import_dir_entry->name_rva);
        const uint32_t import_address_table_rva = get_uint32_le(import_dir_entry->import_address_table_rva);
        const uint32_t import_lookup_table_rva  = get_uint32_le(import_dir_entry->import_lookup_table_rva);
        char    *const dll_name                 = (char *)buf_at_offset(process_va, name_rva, 1);
        uint32_t       rva;

        /* Last entry */
        if ( ! import_address_table_rva)
            break;

        if (forwarder_chain) {
            fprintf(stderr, "Error: Forwarder chain is non-zero for %s, which is not supported\n",
                    dll_name);
            return 1;
        }

        printf("import %s\n", (const char *)dll_name);

        /* We will compress the IAT, so the executable will have to fill out the
         * IAT on its own after it is loaded.
         */
        iat_size += push_iat_string(&iat_buf_left, dll_name);
        iat_size += push_iat_uint32(&iat_buf_left, import_address_table_rva);

        rva = import_address_table_rva;
        for (;;) {
            uint64_t    value;
            const char *fun_name;
            uint32_t    by_ordinal;
            uint32_t    fun_name_rva;
            uint16_t    hint;

            if (is_64bit) {
                value      = get_uint64_le(*(const uint64_le *)buf_at_offset(process_va, rva, 8));
                rva       += 8;
                by_ordinal = (uint32_t)(value >> 63);
            }
            else {
                value      = get_uint32_le(*(const uint32_le *)buf_at_offset(process_va, rva, 4));
                rva       += 4;
                by_ordinal = (uint32_t)(value >> 31);
            }

            if ( ! value)
                break;

            if (by_ordinal) {
                printf("        ord                      %u\n", (uint32_t)value & 0xFFFFU);
                fprintf(stderr, "Error: Importing functions by ordinal number is not supported\n");
                return 1;
            }

            fun_name_rva = (uint32_t)value & 0x7FFFFFFFU;
            fun_name     = (const char *)buf_at_offset(process_va, fun_name_rva, 3) + 2;
            hint         = get_uint16_le(*(uint16_le *)buf_at_offset(process_va, fun_name_rva, 2));

            printf("        name                     %s (hint %u)\n", fun_name, hint);

            iat_size += push_iat_string(&iat_buf_left, fun_name);
        }

        /* 0 (empty string) indicates end of symbols for this DLL */
        iat_size += push_iat_byte(&iat_buf_left, 0);
        if (iat_size >= iat_data->size)
            break;

        /* Clear the library name in the original executable, since the executable
         * will have to load it manually.
         */
        memset(dll_name, 0, strlen(dll_name));

        /* Clear the address tables */
        clear_address_table(process_va, import_address_table_rva, is_64bit);
        clear_address_table(process_va, import_lookup_table_rva, is_64bit);
    }

    /* 0 (empty string) indicates end of symbols for this DLL */
    iat_size += push_iat_byte(&iat_buf_left, 0);

    if (iat_size > iat_data->size) {
        fprintf(stderr, "Error: Failed to accommodate import address table\n");
        return 1;
    }

    iat_data->size = iat_size;

    /* Clear the import table */
    memset(import_table.buf, 0, import_table.size);

    return 0;
}

int is_pe_file(const void *buf, size_t size)
{
    return get_pe_offset(buf, size) > 0;
}

int exe_pe(const void *buf, size_t size)
{
    const PE_HEADER        *pe_header;
    const PE32_HEADER      *opt32_header;
    const PE32_PLUS_HEADER *opt64_header;
    const DATA_DIRECTORY   *data_dir;
    const SECTION_HEADER   *section_header;
    COMPRESSED_SIZES        compressed;
    BUFFER                  mem_image;  /* Memory allocated to create processsed output */
    BUFFER                  process_va; /* Image of the program loaded in memory */
    BUFFER                  output;
    BUFFER                  iat_data  = { NULL, 0 };
    uint32_t                machine;
    uint32_t                dir_size;
    const uint32_t          pe_offset = get_pe_offset(buf, size);
    uint32_t                sect_offset;
    uint32_t                num_sections;
    uint32_t                va_start  = ~0U;
    uint32_t                va_end    = 0;
    size_t                  alloc_size;
    unsigned int            i;
    int                     error     = 1;
    uint16_t                pe_flags;
    uint16_t                pe_format;

    /* Parse PE headers */
    assert(pe_offset);

    pe_header = (const PE_HEADER *)at_offset(buf, pe_offset);

    machine = get_uint16_le(pe_header->machine);

    switch (machine) {

        case PE_MACHINE_X86_32:
            break;

        case PE_MACHINE_X86_64:
            break;

        case PE_MACHINE_AARCH64:
            fprintf(stderr, "PE format for aarch64 architecture is not supported\n");
            return 1;

        default:
            fprintf(stderr, "Error: Unknown architecture 0x%x in PE format\n", machine);
            return 1;
    }

    if (get_uint32_le(pe_header->symbol_table_offset) || get_uint32_le(pe_header->number_of_symbols)) {
        fprintf(stderr, "Error: Compressing symbol table in PE format is not supported\n");
        return 1;
    }

    pe_flags = get_uint16_le(pe_header->flags);

    if (pe_flags & CHAR_UNSUPPORTED) {
        fprintf(stderr, "Error: Unsupported bits set in characteristics field: 0x%x\n",
                pe_flags & CHAR_UNSUPPORTED);
        return 1;
    }

    if (pe_flags & CHAR_DLL) {
        fprintf(stderr, "Error: Compressing DLLs is not supported\n");
        return 1;
    }

    sect_offset  = pe_offset + (uint32_t)sizeof(PE_HEADER) + get_uint16_le(pe_header->optional_hdr_size);
    num_sections = get_uint16_le(pe_header->number_of_sections);

    if (sect_offset + num_sections * sizeof(SECTION_HEADER) > size) {
        fprintf(stderr, "Error: Optional header size or sections exceed file size\n");
        return 1;
    }

    if (get_uint16_le(pe_header->optional_hdr_size) < sizeof(PE32_HEADER)) {
        fprintf(stderr, "Error: Invalid optional header size\n");
        return 1;
    }

    opt32_header = (const PE32_HEADER *)at_offset(buf, pe_offset + (uint32_t)sizeof(PE_HEADER));
    opt64_header = (const PE32_PLUS_HEADER *)at_offset(buf, pe_offset + (uint32_t)sizeof(PE_HEADER));

    pe_format = get_uint16_le(opt64_header->pe_format);
    if (pe_format != PE_FORMAT_PE32 && pe_format != PE_FORMAT_PE32_PLUS) {

        fprintf(stderr, "Error: Unsupported format of PE optional header: 0x%x\n", pe_format);
        return 1;
    }

    /* Print optional header */
    printf("        PE flags                 0x%x\n",         pe_flags);
    printf("optional header:\n");
    printf("        linker ver               %u.%u\n",        opt64_header->linker_ver_major, opt64_header->linker_ver_minor);
    printf("        code size                0x%x\n",         get_uint32_le(opt64_header->size_of_code));
    printf("        data size                0x%x\n",         get_uint32_le(opt64_header->size_of_data));
    printf("        uninitialized data size  0x%x\n",         get_uint32_le(opt64_header->size_of_uninitialized_data));
    printf("        entry point              0x%x\n",         get_uint32_le(opt64_header->address_of_entry_point));
    printf("        code base                0x%x\n",         get_uint32_le(opt64_header->base_of_code));
    if (pe_format == PE_FORMAT_PE32) {
        printf("        data base                0x%x\n",     get_uint32_le(opt32_header->base_of_data));
        printf("        image base               0x%x\n",     get_uint32_le(opt32_header->image_base));
    }
    else
        printf("        image base               0x%llx\n",   (unsigned long long)get_uint64_le(opt64_header->image_base));
    printf("        section alignment        %u\n",           get_uint32_le(opt64_header->section_alignment));
    printf("        file alignment           %u\n",           get_uint32_le(opt64_header->file_alignment));
    printf("        min os ver               %u.%u\n",        get_uint16_le(opt64_header->min_os_ver_major), get_uint16_le(opt64_header->min_os_ver_minor));
    printf("        image ver                %u.%u\n",        get_uint16_le(opt64_header->image_ver_major), get_uint16_le(opt64_header->image_ver_minor));
    printf("        subsystem ver            %u.%u\n",        get_uint16_le(opt64_header->subsystem_ver_major), get_uint16_le(opt64_header->subsystem_ver_minor));
    printf("        image size               0x%x\n",         get_uint32_le(opt64_header->size_of_image));
    printf("        headers size             0x%x\n",         get_uint32_le(opt64_header->size_of_headers));
    printf("        checksum                 0x%x\n",         get_uint32_le(opt64_header->checksum));
    printf("        subsystem                %u\n",           get_uint16_le(opt64_header->subsystem));
    printf("        dll_flags                0x%x\n",         get_uint16_le(opt64_header->dll_flags));
    if (pe_format == PE_FORMAT_PE32) {
        printf("        size_of_stack_reserve    0x%x\n",     get_uint32_le(opt32_header->size_of_stack_reserve));
        printf("        size_of_stack_commit     0x%x\n",     get_uint32_le(opt32_header->size_of_stack_commit));
        printf("        size_of_head_reserve     0x%x\n",     get_uint32_le(opt32_header->size_of_head_reserve));
        printf("        size_of_heap_commit      0x%x\n",     get_uint32_le(opt32_header->size_of_heap_commit));
    }
    else {
        printf("        size_of_stack_reserve    0x%llx\n",   (unsigned long long)get_uint64_le(opt64_header->size_of_stack_reserve));
        printf("        size_of_stack_commit     0x%llx\n",   (unsigned long long)get_uint64_le(opt64_header->size_of_stack_commit));
        printf("        size_of_head_reserve     0x%llx\n",   (unsigned long long)get_uint64_le(opt64_header->size_of_head_reserve));
        printf("        size_of_heap_commit      0x%llx\n",   (unsigned long long)get_uint64_le(opt64_header->size_of_heap_commit));
    }

    /* Process and print sections */
    section_header = (const SECTION_HEADER *)at_offset(buf, sect_offset);

    for (i = 0; i < num_sections; i++) {
        char           name[9]        = { 0 };
        char           str_flags[128] = { 0 };
        const uint32_t section_va     = get_uint32_le(section_header[i].virtual_address);
        const uint32_t va_size        = get_uint32_le(section_header[i].virtual_size);
        uint32_t       value;

        decode_section_flags(str_flags, sizeof(str_flags), get_uint32_le(section_header[i].flags));

        if (section_va < va_start)
            va_start = section_va;
        if (section_va + va_size > va_end)
            va_end = section_va + va_size;

        memcpy(name, section_header[i].name, 8);
        printf("section %s\n", name);
        printf("        pointer_to_raw_data      0x%x\n", get_uint32_le(section_header[i].pointer_to_raw_data));
        printf("        size_of_raw_data         0x%x\n", get_uint32_le(section_header[i].size_of_raw_data));
        printf("        virtual_address          0x%x\n", get_uint32_le(section_header[i].virtual_address));
        printf("        virtual_size             0x%x\n", get_uint32_le(section_header[i].virtual_size));
        printf("        flags                    0x%x%s\n", get_uint32_le(section_header[i].flags), str_flags);

        value = get_uint32_le(section_header[i].pointer_to_relocations);
        if (value) {
            fprintf(stderr, "Error: pointer_to_relocations is %u in section %u but expected 0\n", value, i);
            return 1;
        }
        value = get_uint32_le(section_header[i].pointer_to_line_numbers);
        if (value) {
            fprintf(stderr, "Error: pointer_to_line_numbers is %u in section %u but expected 0\n", value, i);
            return 1;
        }
        value = get_uint16_le(section_header[i].number_of_relocations);
        if (value) {
            fprintf(stderr, "Error: number_of_relocations is %u in section %u but expected 0\n", value, i);
            return 1;
        }
        value = get_uint16_le(section_header[i].number_of_line_numbers);
        if (value) {
            fprintf(stderr, "Error: number_of_line_numbers is %u in section %u but expected 0\n", value, i);
            return 1;
        }
    }

    /* Here's the layout in memory:
     *
     * +--------------+-----+
     * | process data | iat |
     *
     * - process data - This is the original process image layout in memory, as the executable
     *   expects Windows to load it from file.  These are all the sections loaded from the file.
     *   We make some modifications like zero out some areas which we don't need, which will reduce
     *   its size after compression.  The executable expects Windows to load it at `image_base`
     *   address specified in the optional header (PE32_HEADER or PE32_PLUS_HEADER).  We zero out
     *   stuff like debug section, relocation table (we expect Windows to load it at a fixed
     *   address, so no relocations are needed), and import table, which is not needed (see iat). 
     * - iat - This is import address table which contains information used by the process
     *   after decompression to fill out the real import address table in the original process.
     */
    va_end     = align_up(va_end, 0x1000);
    alloc_size = va_end + estimate_compress_size(va_end);
    mem_image  = buf_alloc(alloc_size);
    process_va = buf_truncate(mem_image, va_end);
    output     = buf_get_tail(mem_image, process_va.size);

    /* Load all sections into the right places */
    for (i = 0; i < num_sections; i++) {
        const uint32_t    pointer_to_raw_data = get_uint32_le(section_header[i].pointer_to_raw_data);
        const uint32_t    size_of_raw_data    = get_uint32_le(section_header[i].size_of_raw_data);
        const uint32_t    virtual_address     = get_uint32_le(section_header[i].virtual_address);
        const uint32_t    virtual_size        = get_uint32_le(section_header[i].virtual_size);
        const void *const src                 = at_offset(buf, pointer_to_raw_data);
        uint8_t    *const dest                = buf_at_offset(process_va, virtual_address, virtual_size);
        const uint32_t    copy_size           = (size_of_raw_data < virtual_size) ? size_of_raw_data : virtual_size;

        memcpy(dest, src, copy_size);
    }

    /* Process data directory entries */
    if (pe_format == PE_FORMAT_PE32) {
        data_dir = opt32_header->rva_and_sizes;
        dir_size = get_uint32_le(opt32_header->number_of_rva_and_sizes);
    }
    else {
        data_dir = opt64_header->rva_and_sizes;
        dir_size = get_uint32_le(opt64_header->number_of_rva_and_sizes);
    }

    if (dir_size == 0 ||
        (dir_size - 1) * sizeof(DATA_DIRECTORY) + sizeof(PE32_PLUS_HEADER) <
            get_uint16_le(pe_header->optional_hdr_size)) {

        fprintf(stderr, "Error: Unexpected optional header size\n");
        goto cleanup;
    }

    for (i = 0; i < dir_size; i++) {
        const DATA_DIRECTORY *const dir = &data_dir[i];

        const uint32_t virtual_address = get_uint32_le(dir->virtual_address);
        const uint32_t entry_size      = get_uint32_le(dir->size);
        const BUFFER   entry_buf       = buf_slice(process_va, virtual_address, entry_size);

        static const char *const dir_names[] = {
            "export table",
            "import table",
            "resource table",
            "exception table",
            "certificate table",
            "base relocation_table",
            "debug",
            "architecture",
            "global ptr",
            "tls table",
            "load config_table",
            "bound import",
            "iat"
        };

        const char *const name = (i < sizeof(dir_names) / sizeof(dir_names[0])) ? dir_names[i] : "unknown";

        if (entry_size == 0) {
            if (virtual_address)
                fprintf(stderr, "Warning: Unexpected virtual address 0x%x for data directory entry %u of size 0\n",
                        virtual_address, i);
            continue;
        }

        if (virtual_address < va_start || virtual_address >= va_end ||
            (virtual_address + entry_size) > va_end) {
            fprintf(stderr, "Error: Invalid data directory %u (%s) with VA 0x%x and size 0x%x\n",
                    i, name, virtual_address, entry_size);
            goto cleanup;
        }

        printf("dir %u (%s): va=0x%x size=0x%x\n",
               i, name, virtual_address, entry_size);

        switch (i) {
            /* We don't care about supporting exceptions, so just fill
             * the exception table with zeroes.
             */
            case DIR_EXCEPTION_TABLE:
            /* Base relocation table is used only if the exe is loaded at
             * a different address than specified in image_base in the optional
             * header.  We don't support that, so fill the base relocation
             * table with zeroes.
             */
            case DIR_BASE_RELOCATION_TABLE:
            /* Debug table is not needed, fill it with zeroes. */
            case DIR_DEBUG:
                memset(entry_buf.buf, 0, entry_buf.size);
                break;

            /* Import address table is redundant, it is pointed to by the import table */
            case DIR_IAT:
                break;

            case DIR_IMPORT_TABLE:
                iat_data = output;
                if (process_import_table(process_va, entry_buf, &iat_data,
                                         pe_format == PE_FORMAT_PE32_PLUS))
                    goto cleanup;
                iat_data.size = align_up((uint32_t)iat_data.size, 0x1000);
                output = buf_get_tail(output, iat_data.size);
                break;

            default:
                fprintf(stderr, "Error: Unsupported data directory table %u (%s)\n", i, name);
                goto cleanup;
        }
    }

    compressed = lza_compress(output.buf, output.size, process_va.buf, /*process_va.size +*/ iat_data.size, 128);

    if ( ! compressed.lz)
        goto cleanup;

    printf("Original PE %zu bytes\n", size);
    printf("Compressed  %zu bytes\n", compressed.lz);

    /*
     * https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
     */
    /* Hints on minimizing PE executables:
     *
     * - Put PE right after MZ, i.e. PE offset 0x04
     * - Use 1 section only
     * - Minimize optional header size, it may be possible to use size 4
     */

    error = 0;

cleanup:
    free(mem_image.buf);

    return error;
}
