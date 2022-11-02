/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "exe_pe.h"

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

static void append(char *buf, size_t size, const char *str)
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
        append(buf, size, " read");
    if (flags & SECTION_MEM_WRITE)
        append(buf, size, " write");
    if (flags & SECTION_MEM_EXECUTE)
        append(buf, size, " exec");
    if (flags & SECTION_MEM_DISCARDABLE)
        append(buf, size, " discard");
    if (flags & SECTION_CNT_CODE)
        append(buf, size, " code");
    if (flags & SECTION_CNT_INITIALIZED_DATA)
        append(buf, size, " data");
    if (flags & SECTION_CNT_UNINITIALIZED_DATA)
        append(buf, size, " udata");

    flags &= ~(SECTION_MEM_READ | SECTION_MEM_WRITE | SECTION_MEM_EXECUTE | SECTION_MEM_DISCARDABLE |
               SECTION_CNT_CODE | SECTION_CNT_INITIALIZED_DATA | SECTION_CNT_UNINITIALIZED_DATA);

    if (flags)
        append(buf, size, " unknown");
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
    uint8_t                *mem_image = NULL; /* Image of the program loaded in memory */
    uint32_t                machine;
    uint32_t                dir_size;
    const uint32_t          pe_offset = get_pe_offset(buf, size);
    uint32_t                sect_offset;
    uint32_t                num_sections;
    uint32_t                va_start  = ~0U;
    uint32_t                va_end    = 0;
    uint32_t                alloc_size;
    unsigned int            i;
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
        printf("        pointer_to_relocations   %u\n", get_uint32_le(section_header[i].pointer_to_relocations));
        printf("        pointer_to_line_numbers  %u\n", get_uint32_le(section_header[i].pointer_to_line_numbers));
        printf("        number_of_relocations    %u\n", get_uint16_le(section_header[i].number_of_relocations));
        printf("        number_of_line_numbers   %u\n", get_uint16_le(section_header[i].number_of_line_numbers));
        printf("        flags                    0x%x%s\n", get_uint32_le(section_header[i].flags), str_flags);
    }

    /* Process and print data directory entries */
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
        return 1;
    }

    for (i = 0; i < dir_size; i++) {
        const DATA_DIRECTORY *dir = &data_dir[i];

        const uint32_t virtual_address = get_uint32_le(dir->virtual_address);
        const uint32_t entry_size      = get_uint32_le(dir->size);

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

        if (entry_size == 0) {
            if (virtual_address)
                fprintf(stderr, "Warning: Unexpected virtual address 0x%x for section %u of size 0\n",
                        virtual_address, i);
            continue;
        }

        printf("dir %u (%s): va=0x%x size=0x%x\n",
               i,
               (i < sizeof(dir_names) / sizeof(dir_names[0])) ? dir_names[i] : "unknown",
               virtual_address,
               entry_size);
    }

    /* Allocate memory for the exe image as it is loaded in memory
     * - begin from VA 0
     * - align up the end of reserved space to 4KB
     * - multiply by 3x, so give 2x as much space
     * The first portion up to aligned va_end is the original data with the original
     * layout, which we will compress here and which will be decompressed at run time.
     * This will not be written to the compressed exe.  After that we will have the
     * decompressor code and compressed data.  The compressed data will not take
     * as much as 2x space, but we just allocate extra memory to be sure to have
     * enough wiggle room.
     */
    alloc_size = align_up(va_end, 0x1000) * 3;
    mem_image  = (uint8_t *)calloc(alloc_size, 1);

    /* Load all sections into the right places */
    for (i = 0; i < num_sections; i++) {
        const uint32_t    pointer_to_raw_data = get_uint32_le(section_header[i].pointer_to_raw_data);
        const uint32_t    size_of_raw_data    = get_uint32_le(section_header[i].size_of_raw_data);
        const uint32_t    virtual_address     = get_uint32_le(section_header[i].virtual_address);
        const uint32_t    virtual_size        = get_uint32_le(section_header[i].virtual_size);
        const void *const src                 = at_offset(buf, pointer_to_raw_data);
        uint8_t    *const dest                = mem_image + virtual_address;
        const uint32_t    copy_size           = (size_of_raw_data < virtual_size) ? size_of_raw_data : virtual_size;

        memcpy(dest, src, copy_size);
    }

    /*
     * https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
     */
    /* Hints on minimizing PE executables:
     *
     * - Put PE right after MZ, i.e. PE offset 0x04
     * - Use 1 section only
     * - Minimize optional header size, it may be possible to use size 4
     */

    free(mem_image);

    return 0;
}
