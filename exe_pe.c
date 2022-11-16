/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "exe_pe.h"
#include "arith_decode.h"
#include "arith_encode.h"
#include "load_file.h"
#include "lza_decompress.h"
#include "lza_compress.h"

#include <assert.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Layout of the program/code/data in memory is represented by the LAYOUT structure
 * as well as the following diagram.  Note that all fields except image_base are
 * Relative Virtual Addresses (rvas), i.e. are relative to image_base.
 *
 * 0 ------------------> +----------------+ <- The beginning of the process's virtual address space
 *                       :                :
 *                       :                :
 * image_base ---------> +----------------+ <- Virtual address at which the process is loaded
 * decomp_base_rva ----> +----------------+ <- Start of first section, this is where data is decompressed
 *                       |    original    |
 *                       |    process     |
 *                       |    data        | <- Original process data is located in this block;
 *                       |                |    in the new exe this area is reserved and this is where
 *                       |                |    the program will be decompressed
 *                       |                |
 * iat_rva ------------> +----------------+ <- New/packed Import Address Table is decompressed here in
 *                       |      iat       |    a format which is compressible, so it occupies less space
 *                       |                |
 * import_loader_rva --> +----------------+ <- Loader for the IAT is decompressed here, this loader will
 *                       |    import      |    load the imported functions from DLLs and put them
 *                       |    loader      |    in the original IAT in original process data
 *                       |                |
 *                       +----------------+
 *                       |     TODO       |
 * lz77_data_rva ------> +----------------+ <- LZ77-compressed data is decoded here by the
 *                       |   LZ77 data    |    arithmetic decoder
 *                       |                |
 * lz77_decomp_rva ----> +----------------+ <- LZ77 decompressor is decoded here by the
 *                       |     LZ77       |    arithmetic decoder
 *                       |  decompressor  |
 *                       |                |
 * comp_data_rva ------> +----------------+ <- This is where the fully compressed program is loaded
 *                       |   compressed   |    by the OS loader from the new executable;
 *                       |    program     |    this is also the begining of the second section
 *                       |                |
 * arith_decoder_rva --> +----------------+ <- Arithmetic decoder is stored here;
 *                       |   arithmetic   |    this rva is also the new entry_point_rva;
 *                       |    decoder     |    address NOT aligned on 4K
 * import_dir_rva -----> +----------------+ <- Used to load this by Windows; address NOT aligned on 4K
 *                       |  mini import   |
 *                       |   directory    |
 * mini_iat_rva -------> +----------------+ <- Import Address Table loaded by the OS, which is used
 *                       |   mini iat     |    by the import loader; address NOT aligned on 4K
 * live_layout_rva ----> +----------------+ <- Live layout structure used by loaders; address NOT
 *                       |  live layout   |    aligned on 4K
 * end_rva ------------> +----------------+ <- End of used address space; address NOT aligned on 4K
 */

typedef struct {
    uint64_t image_base;
    uint32_t decomp_base_rva;
    uint32_t iat_rva;
    uint32_t import_loader_rva;
    uint32_t lz77_data_rva;
    uint32_t lz77_decompressor_rva;
    uint32_t comp_data_rva;
    uint32_t arith_decoder_rva;
    uint32_t import_dir_rva;
    uint32_t mini_iat_rva;
    uint32_t live_layout_rva;
    uint32_t end_rva;
} LAYOUT;

/* Auxiliary endianness-agnostic data types */

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

static uint16_le make_uint16_le(uint16_t value)
{
    uint16_le data;

    data.bytes[0] = (uint8_t)(value & 0xFFU);
    data.bytes[1] = (uint8_t)((value >> 8) & 0xFFU);

    return data;
}

static uint32_le make_uint32_le(uint32_t value)
{
    uint32_le data;

    data.bytes[0] = (uint8_t)(value & 0xFFU);
    data.bytes[1] = (uint8_t)((value >> 8) & 0xFFU);
    data.bytes[2] = (uint8_t)((value >> 16) & 0xFFU);
    data.bytes[3] = (uint8_t)((value >> 24) & 0xFFU);

    return data;
}

static uint64_le make_uint64_le(uint64_t value)
{
    uint64_le data;

    data.bytes[0] = (uint8_t)(value & 0xFFU);
    data.bytes[1] = (uint8_t)((value >> 8) & 0xFFU);
    data.bytes[2] = (uint8_t)((value >> 16) & 0xFFU);
    data.bytes[3] = (uint8_t)((value >> 24) & 0xFFU);
    data.bytes[4] = (uint8_t)((value >> 32) & 0xFFU);
    data.bytes[5] = (uint8_t)((value >> 40) & 0xFFU);
    data.bytes[6] = (uint8_t)((value >> 48) & 0xFFU);
    data.bytes[7] = (uint8_t)((value >> 56) & 0xFFU);

    return data;
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return ((value - 1) / align + 1) * align;
}

/* Structures representing various data structures in PE files */
/* Reference: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format */

/* The MZ (DOS) header is located at offset 0 in a PE file */
typedef struct {
    uint16_le mz_signature;
    uint8_t   garbage[0x3A];
    uint32_le pe_offset;
} DOS_HEADER;

/* The PE header is located at offset `pe_offset` from the MZ header */
typedef struct {
    uint32_le pe_signature;
    uint16_le machine;
    uint16_le number_of_sections; /* Sections follow the optional header */
    uint32_le time_date_stamp;
    uint32_le symbol_table_offset;
    uint32_le number_of_symbols;
    uint16_le optional_hdr_size; /* "Optional" header is the PE32_HEADER */
    uint16_le flags;
} PE_HEADER;

#define PE_MACHINE_X86_32  0x014C
#define PE_MACHINE_X86_64  0x8664
#define PE_MACHINE_AARCH64 0xAA64

#define PE_FLAG_RELOCS_STRIPPED     0x0001
#define PE_FLAG_EXECUTABLE_IMAGE    0x0002
#define PE_FLAG_LARGE_ADDRESS_AWARE 0x0020
#define PE_FLAG_32BIT_MACHINE       0x0100
#define PE_FLAG_DLL                 0x2000
#define PE_FLAG_UNSUPPORTED         0xD09C

typedef struct {
    uint32_le virtual_address;
    uint32_le size;
} DATA_DIRECTORY;

/* The optional header, which immediately follows the PE header.
 * The actual size of this header is specified in the PE header.
 */
typedef struct {
    uint16_le pe_format;
    uint8_t   linker_ver_major;
    uint8_t   linker_ver_minor;
    uint32_le size_of_code;
    uint32_le size_of_data;
    uint32_le size_of_uninitialized_data;
    uint32_le entry_point;
    uint32_le base_of_code;
    union { /* Different layout between 32-bit and 64-bit executable */
        struct {
            uint32_le base_of_data;
            uint32_le image_base;
        } u32;
        struct {
            uint64_le image_base;
        } u64;
    } u1;
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
    union { /* Different layout between 32-bit and 64-bit executable */
        struct {
            uint32_le size_of_stack_reserve;
            uint32_le size_of_stack_commit;
            uint32_le size_of_head_reserve;
            uint32_le size_of_heap_commit;
            uint32_le reserved_loader_flags;
            uint32_le number_of_rva_and_sizes;
            DATA_DIRECTORY rva_and_sizes[1]; /* There is one or more data directories */
        } u32;                               /* at the end of the optional header */
        struct {
            uint64_le size_of_stack_reserve;
            uint64_le size_of_stack_commit;
            uint64_le size_of_head_reserve;
            uint64_le size_of_heap_commit;
            uint32_le reserved_loader_flags;
            uint32_le number_of_rva_and_sizes;
            DATA_DIRECTORY rva_and_sizes[1];
        } u64;
    } u2;
} PE32_HEADER;

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

/* An import directory RVA is specified by DIR_IMPORT_TABLE data directory */
typedef struct {
    uint32_le import_lookup_table_rva;
    uint32_le time_stamp;
    uint32_le forwarder_chain;
    uint32_le name_rva;
    uint32_le import_address_table_rva;
} IMPORT_DIR_ENTRY;

/* Section headers immediately follow the optional header (after the
 * data directory entries).  The number of section headers is specified
 * by the `number_of_sections` member in the PE header.
 */
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

/* Size of the new PE header.  It has to be aligned to file_alignment; minimum file_alignment is
 * 512 bytes.  It cannot be zero.  Therefore it must be exactly 512.
 */
static const uint32_t new_header_size = 512;

/* The new/minimal PE header is a merged MZ, PE and optional header.
 * This is the header we will produce in the compressed executable.
 */
typedef struct {
    uint32_le mz_signature;
    uint32_le unused[3];
    uint32_le pe_signature; /* Exploit the unused portion of the MZ (DOS) header */
    uint16_le machine;
    uint16_le number_of_sections;
    uint32_le time_date_stamp;
    uint32_le symbol_table_offset;
    uint32_le number_of_symbols;
    uint16_le optional_hdr_size;
    uint16_le flags;
    uint16_le pe_format;
    uint8_t   linker_ver_major;
    uint8_t   linker_ver_minor;
    uint32_le size_of_code;
    uint32_le size_of_data;
    uint32_le size_of_uninitialized_data;
    uint32_le entry_point;
    uint32_le pe_offset; /* This overlaps with base_of_code, must be the same value */
    union {
        struct {
            uint32_le base_of_data;
            uint32_le image_base;
        } u32;
        struct {
            uint64_le image_base;
        } u64;
    } u1;
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
    union {
        struct {
            uint32_le size_of_stack_reserve;
            uint32_le size_of_stack_commit;
            uint32_le size_of_head_reserve;
            uint32_le size_of_heap_commit;
            uint32_le reserved_loader_flags;
            uint32_le number_of_rva_and_sizes;
            DATA_DIRECTORY rva_and_sizes[4];
        } u32;
        struct {
            uint64_le size_of_stack_reserve;
            uint64_le size_of_stack_commit;
            uint64_le size_of_head_reserve;
            uint64_le size_of_heap_commit;
            uint32_le reserved_loader_flags;
            uint32_le number_of_rva_and_sizes;
            DATA_DIRECTORY rva_and_sizes[2];
        } u64;
    } u2;
    SECTION_HEADER section_placeholder[2];
    uint8_t        alignment_placeholder[new_header_size];
} MINIMAL_PE_HEADER;

static BUFFER prepare_pe_header(BUFFER             process_va,
                                const PE_HEADER   *pe_header,
                                const PE32_HEADER *opt_header)
{
    SECTION_HEADER      *section_header;
    BUFFER               header_buf   = buf_truncate(process_va, new_header_size);
    MINIMAL_PE_HEADER   *new_header   = (MINIMAL_PE_HEADER *)header_buf.buf;
    static const uint8_t mz32[4]      = { 0x4DU, 0x5AU, 0, 0 };
    const uint32_t       pe_format    = get_uint16_le(opt_header->pe_format);
    const uint32_t       num_sections = (uint32_t)(sizeof(new_header->section_placeholder) / sizeof(new_header->section_placeholder[0]));
    uint32_t             hdr_size;
    static const char    sec_bss[8]   = "unpack";
    static const char    sec_text[8]  = "packed";
    const uint32_t       sec_flags    = SECTION_MEM_EXECUTE | SECTION_MEM_READ | SECTION_MEM_WRITE;

    section_header = (SECTION_HEADER *)((uint8_t *)&new_header->u2 +
            ((pe_format == PE_FORMAT_PE32) ? sizeof(new_header->u2.u32)
                                           : sizeof(new_header->u2.u64)));

    hdr_size = (uint32_t)(uintptr_t)((uint8_t *)&section_header[num_sections] - (uint8_t *)new_header);
    assert(hdr_size <= new_header_size);

    header_buf = buf_truncate(header_buf, hdr_size);

    memcpy(&new_header->mz_signature, mz32, sizeof(mz32));
    new_header->pe_signature            = pe_header->pe_signature;
    new_header->machine                 = pe_header->machine;
    new_header->number_of_sections      = make_uint16_le((uint16_t)num_sections);
    new_header->time_date_stamp         = pe_header->time_date_stamp;
    new_header->symbol_table_offset     = make_uint32_le(0);
    new_header->number_of_symbols       = make_uint32_le(0);
    new_header->optional_hdr_size       = make_uint16_le((uint16_t)(uintptr_t)(
                (uint8_t *)section_header - (uint8_t *)&new_header->pe_format));
    new_header->flags                   = make_uint16_le(
            get_uint16_le(pe_header->flags) | PE_FLAG_RELOCS_STRIPPED);
    new_header->pe_format               = opt_header->pe_format;
    new_header->linker_ver_major        = opt_header->linker_ver_major;
    new_header->linker_ver_minor        = opt_header->linker_ver_minor;
    new_header->pe_offset               = make_uint32_le((uint32_t)(uintptr_t)(
                (uint8_t *)&new_header->pe_signature - (uint8_t *)new_header));

    if (pe_format == PE_FORMAT_PE32)
        new_header->u1.u32.image_base   = opt_header->u1.u32.image_base;
    else
        new_header->u1.u64.image_base   = opt_header->u1.u64.image_base;

    new_header->section_alignment       = opt_header->section_alignment;
    new_header->file_alignment          = make_uint32_le(new_header_size);
    new_header->min_os_ver_major        = opt_header->min_os_ver_major;
    new_header->min_os_ver_minor        = opt_header->min_os_ver_minor;
    new_header->image_ver_major         = opt_header->image_ver_major;
    new_header->image_ver_minor         = opt_header->image_ver_minor;
    new_header->subsystem_ver_major     = opt_header->subsystem_ver_major;
    new_header->subsystem_ver_minor     = opt_header->subsystem_ver_minor;
    new_header->reserved_win32_version  = opt_header->reserved_win32_version;
    new_header->size_of_headers         = make_uint32_le(new_header_size);
    new_header->checksum                = opt_header->checksum;
    new_header->subsystem               = opt_header->subsystem;
    new_header->dll_flags               = opt_header->dll_flags;

    if (pe_format == PE_FORMAT_PE32) {
        memcpy(&new_header->u2.u32, &opt_header->u2.u32, sizeof(opt_header->u2.u32));
        new_header->u2.u32.number_of_rva_and_sizes = make_uint32_le((uint32_t)(
                    sizeof(new_header->u2.u32.rva_and_sizes) / sizeof(new_header->u2.u32.rva_and_sizes[0])));
    }
    else {
        memcpy(&new_header->u2.u64, &opt_header->u2.u64, sizeof(opt_header->u2.u64));
        new_header->u2.u64.number_of_rva_and_sizes = make_uint32_le((uint32_t)(
                    sizeof(new_header->u2.u64.rva_and_sizes) / sizeof(new_header->u2.u64.rva_and_sizes[0])));
    }

    assert(sizeof(section_header[0].name) == sizeof(sec_bss));
    memcpy(section_header[0].name, sec_bss, sizeof(sec_bss));

    assert(sizeof(section_header[1].name) == sizeof(sec_text));
    memcpy(section_header[1].name, sec_text, sizeof(sec_text));

    return header_buf;
}

static void fill_pe_header(BUFFER             process_va,
                           const LAYOUT      *layout,
                           uint32_t           entry_point,
                           const PE32_HEADER *opt_header)
{
    DATA_DIRECTORY    *data_dir;
    SECTION_HEADER    *section_header;
    MINIMAL_PE_HEADER *new_header   = (MINIMAL_PE_HEADER *)process_va.buf;
    const uint32_t     pe_format    = get_uint16_le(opt_header->pe_format);
    const uint32_t     aligned_end  = align_up(layout->end_rva, 0x1000);
    const uint32_t     num_sections = (uint32_t)(sizeof(new_header->section_placeholder) / sizeof(new_header->section_placeholder[0]));
    const uint32_t     sec_flags    = SECTION_MEM_EXECUTE | SECTION_MEM_READ | SECTION_MEM_WRITE;

    section_header = (SECTION_HEADER *)((uint8_t *)&new_header->u2 +
            ((pe_format == PE_FORMAT_PE32) ? sizeof(new_header->u2.u32)
                                           : sizeof(new_header->u2.u64)));

    new_header->size_of_code            = make_uint32_le(aligned_end);
    new_header->entry_point             = make_uint32_le(entry_point);
    new_header->size_of_image           = make_uint32_le(aligned_end);

    if (pe_format == PE_FORMAT_PE32)
        data_dir = new_header->u2.u32.rva_and_sizes;
    else
        data_dir = new_header->u2.u64.rva_and_sizes;

    data_dir[DIR_IMPORT_TABLE].virtual_address = make_uint32_le(layout->import_dir_rva);
    data_dir[DIR_IMPORT_TABLE].size            = make_uint32_le(2 * sizeof(IMPORT_DIR_ENTRY));

    section_header[0].virtual_address     = make_uint32_le(layout->decomp_base_rva);
    section_header[0].virtual_size        = make_uint32_le(layout->comp_data_rva - layout->decomp_base_rva);
    section_header[0].flags               = make_uint32_le(SECTION_CNT_UNINITIALIZED_DATA | sec_flags);
    section_header[0].pointer_to_raw_data = make_uint32_le(new_header_size);

    section_header[1].virtual_address     = make_uint32_le(layout->comp_data_rva);
    section_header[1].virtual_size        = make_uint32_le(aligned_end - layout->comp_data_rva);
    section_header[1].flags               = make_uint32_le(SECTION_CNT_INITIALIZED_DATA | sec_flags);
    section_header[1].size_of_raw_data    = make_uint32_le(layout->end_rva - layout->comp_data_rva);
    section_header[1].pointer_to_raw_data = make_uint32_le(new_header_size);
}

static void append_str(char *buf, size_t size, const char *str)
{
    const size_t buf_len  = strlen(buf);
    const size_t str_len  = strlen(str) + 1; /* Include terminating NUL */
    const size_t num_left = size - buf_len;

    if (num_left >= str_len)
        memcpy(buf + buf_len, str, str_len);
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

        entry = buf_at_offset(process_va, (uint32_t)value, 3);

        memset(entry, 0, 2 + strlen((char *)entry + 2));
    }
}

static int process_import_table(BUFFER   process_va,
                                BUFFER   import_table,
                                uint32_t va_start,
                                BUFFER  *iat_data,
                                int      is_64bit)
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
        iat_size += push_iat_uint32(&iat_buf_left, import_address_table_rva - va_start);

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

        printf("        import address table     0x%x\n", import_address_table_rva);
        printf("        iat size                 0x%x\n", rva - import_address_table_rva);
        printf("        import lookup table      0x%x\n", import_lookup_table_rva);

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

static uint32_t add_loader(BUFFER *output, const char *loader_name, uint32_t machine)
{
    BUFFER                file_buf;
    static char           filename[64];
    const PE_HEADER      *pe_header;
    const PE32_HEADER    *opt_header;
    const SECTION_HEADER *section_header;
    uint32_t              pe_offset;
    uint32_t              sect_offset;
    uint32_t              num_sections;
    uint32_t              text_pos;
    uint32_t              text_virt_size;
    uint32_t              text_file_size;
    uint32_t              text_size;
    uint32_t              text_virt_offs;
    uint32_t              entry_point;
    uint32_t              i;
    uint32_t              entry_point_offs = ~0U;

    assert(machine == PE_MACHINE_X86_32 || machine == PE_MACHINE_X86_64);
    snprintf(filename, sizeof(filename), "loaders/windows/%s/%s.exe",
             (machine == PE_MACHINE_X86_32) ? "x86" : "x64",
             loader_name);

    file_buf = load_file(filename);
    if ( ! file_buf.buf)
        return ~0U;

    pe_offset = get_pe_offset(file_buf.buf, file_buf.size);

    if (pe_offset == 0) {
        fprintf(stderr, "Error: %s is not a PE file\n", filename);
        goto cleanup;
    }

    if (file_buf.size < pe_offset + (uint32_t)sizeof(PE_HEADER)) {
        fprintf(stderr, "Error: Corrupted %s, PE header is outside of the file\n", filename);
        goto cleanup;
    }

    pe_header    = (const PE_HEADER *)at_offset(file_buf.buf, pe_offset);
    opt_header   = (const PE32_HEADER *)at_offset(file_buf.buf, pe_offset + (uint32_t)sizeof(PE_HEADER));
    sect_offset  = pe_offset + (uint32_t)sizeof(PE_HEADER) + get_uint16_le(pe_header->optional_hdr_size);
    num_sections = get_uint16_le(pe_header->number_of_sections);

    if (file_buf.size < sect_offset + num_sections * (uint32_t)sizeof(SECTION_HEADER)) {
        fprintf(stderr, "Error: Corrupted %s, section headers are outside of the file\n", filename);
        goto cleanup;
    }

    section_header = (const SECTION_HEADER *)at_offset(file_buf.buf, sect_offset);

    for (i = 0; i < num_sections; i++) {
        if ( ! strncmp(section_header[i].name, ".text", sizeof(section_header[i].name)))
            break;
    }

    if (i == num_sections) {
        fprintf(stderr, "Error: Failed to find .text section in %s\n", filename);
        goto cleanup;
    }

    text_pos       = get_uint32_le(section_header[i].pointer_to_raw_data);
    text_file_size = get_uint32_le(section_header[i].size_of_raw_data);
    text_virt_size = get_uint32_le(section_header[i].virtual_size);
    text_size      = text_file_size < text_virt_size ? text_file_size : text_virt_size;
    text_virt_offs = get_uint32_le(section_header[i].virtual_address);
    entry_point    = get_uint32_le(opt_header->entry_point);

    if (file_buf.size < text_pos + text_file_size) {
        fprintf(stderr, "Error: Corrupted %s, .text section is outside of the file\n", filename);
        goto cleanup;
    }

    if (text_size > output->size) {
        fprintf(stderr, "Error: Not enough buffer space %zu for %s loader .text section of size %u\n",
                output->size, filename, text_size);
        goto cleanup;
    }

    if (entry_point < text_virt_offs || entry_point >= text_virt_offs + text_size) {
        fprintf(stderr, "Error: Corrupted %s, entry point 0x%x is outside .text section\n",
                filename, entry_point);
        goto cleanup;
    }

    memcpy(output->buf, buf_at_offset(file_buf, text_pos, text_file_size), text_size);

    output->size = text_size;

    entry_point_offs = entry_point - text_virt_offs;

cleanup:
    free(file_buf.buf);

    return entry_point_offs;
}

static uint32_t add_mini_import_dir(BUFFER *output, uint32_t import_dir_rva, uint16_t pe_format)
{
    static const char str_kernel32_dll[]     = "KERNEL32.dll";
    static const char str_load_library_a[]   = "\0\0LoadLibraryA";
    static const char str_get_proc_address[] = "\0\0GetProcAddress";

    IMPORT_DIR_ENTRY *import_dir    = (IMPORT_DIR_ENTRY *)output->buf;
    uint32_le        *iat;
    const uint32_t    iat_offs      = 2 * (uint32_t)sizeof(IMPORT_DIR_ENTRY);
    const uint32_t    iat_size      = 3 * (pe_format == PE_FORMAT_PE32 ? 4 : 8);
    const uint32_t    kernel_offs   = iat_offs + iat_size;
    const uint32_t    load_lib_offs = kernel_offs + (uint32_t)sizeof(str_kernel32_dll);
    const uint32_t    get_proc_offs = align_up(load_lib_offs + (uint32_t)sizeof(str_load_library_a), 2);
    const uint32_t    total_size    = align_up(get_proc_offs + (uint32_t)sizeof(str_get_proc_address), 2);

    if (get_proc_offs + sizeof(str_get_proc_address) > output->size) {
        fprintf(stderr, "Error: Not enough buffer space for import address table\n");
        return 0;
    }

    /* Fill import directory entry */
    memset(import_dir, 0, total_size);

    import_dir->name_rva = make_uint32_le(import_dir_rva + kernel_offs);
    import_dir->import_address_table_rva = make_uint32_le(import_dir_rva + iat_offs);

    /* Fill import address table */
    iat = (uint32_le *)buf_at_offset(*output, iat_offs, iat_size);

    memset(iat, 0, iat_size);

    if (pe_format == PE_FORMAT_PE32) {
        iat[0] = make_uint32_le(import_dir_rva + load_lib_offs);
        iat[1] = make_uint32_le(import_dir_rva + get_proc_offs);
    }
    else {
        iat[0] = make_uint32_le(import_dir_rva + load_lib_offs);
        iat[2] = make_uint32_le(import_dir_rva + get_proc_offs);
    }

    /* Copy strings */
    memcpy(buf_at_offset(*output, kernel_offs,   sizeof(str_kernel32_dll)),     str_kernel32_dll,     sizeof(str_kernel32_dll));
    memcpy(buf_at_offset(*output, load_lib_offs, sizeof(str_load_library_a)),   str_load_library_a,   sizeof(str_load_library_a));
    memcpy(buf_at_offset(*output, get_proc_offs, sizeof(str_get_proc_address)), str_get_proc_address, sizeof(str_get_proc_address));

    /* Update buffer size */
    output->size = total_size;

    /* Return just the import table size */
    return iat_offs;
}

typedef struct {
    uint32_le decomp_base;
    uint32_le entry_point;
    uint32_le iat;
    uint32_le import_loader;
    uint32_le lz77_data;
    uint32_le lz77_decomp;
    uint32_le comp_data;
    uint32_le mini_iat;
    uint32_le lz77_data_size;
    uint32_le comp_data_size;
} FINAL_LAYOUT_32;

typedef struct {
    uint64_le decomp_base;
    uint64_le entry_point;
    uint64_le iat;
    uint64_le import_loader;
    uint64_le lz77_data;
    uint64_le lz77_decomp;
    uint64_le comp_data;
    uint64_le mini_iat;
    uint32_le lz77_data_size;
    uint32_le comp_data_size;
} FINAL_LAYOUT_64;

static int add_live_layout(BUFFER   output,
                           LAYOUT  *layout,
                           uint32_t entry_point,
                           uint16_t pe_format,
                           uint32_t import_loader_offs,
                           uint32_t lz77_decomp_offs,
                           uint32_t lz77_data_size,
                           uint32_t comp_data_size)
{
    if (sizeof(FINAL_LAYOUT_64) > output.size) {
        fprintf(stderr, "Error: Not enough buffer space to store live layout\n");
        return 1;
    }

    if (pe_format == PE_FORMAT_PE32) {
        FINAL_LAYOUT_32 *final_layout = (FINAL_LAYOUT_32 *)output.buf;

        final_layout->decomp_base    = make_uint32_le((uint32_t)layout->image_base + layout->decomp_base_rva);
        final_layout->entry_point    = make_uint32_le((uint32_t)layout->image_base + entry_point);
        final_layout->iat            = make_uint32_le((uint32_t)layout->image_base + layout->iat_rva);
        final_layout->import_loader  = make_uint32_le((uint32_t)layout->image_base + layout->import_loader_rva + import_loader_offs);
        final_layout->lz77_data      = make_uint32_le((uint32_t)layout->image_base + layout->lz77_data_rva);
        final_layout->lz77_decomp    = make_uint32_le((uint32_t)layout->image_base + layout->lz77_decompressor_rva + lz77_decomp_offs);
        final_layout->comp_data      = make_uint32_le((uint32_t)layout->image_base + layout->comp_data_rva);
        final_layout->mini_iat       = make_uint32_le((uint32_t)layout->image_base + layout->mini_iat_rva);
        final_layout->lz77_data_size = make_uint32_le(lz77_data_size);
        final_layout->comp_data_size = make_uint32_le(comp_data_size);

        layout->end_rva = layout->live_layout_rva + (uint32_t)sizeof(FINAL_LAYOUT_32);
    }
    else {
        FINAL_LAYOUT_64 *final_layout = (FINAL_LAYOUT_64 *)output.buf;

        final_layout->decomp_base    = make_uint64_le(layout->image_base + layout->decomp_base_rva);
        final_layout->entry_point    = make_uint64_le(layout->image_base + entry_point);
        final_layout->iat            = make_uint64_le(layout->image_base + layout->iat_rva);
        final_layout->import_loader  = make_uint64_le(layout->image_base + layout->import_loader_rva + import_loader_offs);
        final_layout->lz77_data      = make_uint64_le(layout->image_base + layout->lz77_data_rva);
        final_layout->lz77_decomp    = make_uint64_le(layout->image_base + layout->lz77_decompressor_rva + lz77_decomp_offs);
        final_layout->comp_data      = make_uint64_le(layout->image_base + layout->comp_data_rva);
        final_layout->mini_iat       = make_uint64_le(layout->image_base + layout->mini_iat_rva);
        final_layout->lz77_data_size = make_uint32_le(lz77_data_size);
        final_layout->comp_data_size = make_uint32_le(comp_data_size);

        layout->end_rva = layout->live_layout_rva + (uint32_t)sizeof(FINAL_LAYOUT_64);
    }

    return 0;
}

static int verify_compression(BUFFER   process_va,
                              LAYOUT  *layout,
                              BUFFER   scratch,
                              uint32_t lz77_data_size,
                              uint32_t comp_data_size)
{
    BUFFER arith_output;
    BUFFER decompressed;
    BUFFER comp_data;
    BUFFER orig_lz77_data;

    if (scratch.size < lz77_data_size + layout->lz77_data_rva) {
        fprintf(stderr, "Error: Not enough buffer space to verify compression\n");
        return 1;
    }

    arith_output   = buf_truncate(scratch, lz77_data_size);
    scratch        = buf_get_tail(scratch, lz77_data_size);
    decompressed   = buf_truncate(scratch, layout->lz77_data_rva - layout->decomp_base_rva);
    comp_data      = buf_slice(process_va, layout->comp_data_rva, comp_data_size);
    orig_lz77_data = buf_slice(process_va, layout->lz77_data_rva, lz77_data_size);

    arith_decode(arith_output.buf, arith_output.size,
                 comp_data.buf,    comp_data.size);

    if (memcmp(orig_lz77_data.buf, arith_output.buf, arith_output.size) != 0) {
        fprintf(stderr, "Error: Arithmetic decoding verification failed\n");
        return 1;
    }

    lz_decompress(decompressed.buf, decompressed.size, arith_output.buf);

    if (memcmp(process_va.buf + layout->decomp_base_rva, decompressed.buf, decompressed.size) != 0) {
        fprintf(stderr, "Error: LZ77 decompression verification failed\n");
        return 1;
    }

    return 0;
}

static void patch_32bit_arith_decoder(BUFFER buf, const LAYOUT *layout)
{
    const uint32_t ad_delta = (uint32_t)(layout->arith_decoder_rva - layout->decomp_base_rva);
    const uint32_t base     = (uint32_t)(layout->image_base + layout->decomp_base_rva);
    const uint32_t end      = (uint32_t)(base + buf.size);
    const uint8_t  msb      = (uint8_t)(base >> 24);
    uint8_t       *next     = buf.buf;

    while (buf.size > 3) {
        uint8_t *const found = (uint8_t *)memchr(buf.buf + 3, msb, buf.size - 3);
        size_t         pos;
        uint32_le     *pvalue;
        uint32_t       value;

        if ( ! found)
            break;

        pos    = (size_t)(found - buf.buf);
        pvalue = (uint32_le *)(found - 3);
        value  = get_uint32_le(*pvalue);

        if (value < base || value >= end) {
            buf = buf_get_tail(buf, pos - 2);
            continue;
        }

        *pvalue = make_uint32_le(value + ad_delta);

        buf = buf_get_tail(buf, pos + 1);
    }
}

static int patch_live_layout(BUFFER buf, uint16_t pe_format, const LAYOUT *layout)
{
    static const uint8_t signature[] = { 0x0D, 0xF0, 0xEF, 0xBE, 0xFE, 0xCA, 0xCE, 0xFA };
    const size_t         sig_size    = (pe_format == PE_FORMAT_PE32) ? 4 : 8;
    const uint64_t       layout_va   = layout->image_base + layout->live_layout_rva;

    if (pe_format == PE_FORMAT_PE32)
        patch_32bit_arith_decoder(buf, layout);

    for (;;) {
        uint8_t *const found = (uint8_t *)memchr(buf.buf, 0x0D, buf.size);
        size_t         pos;

        if ( ! found)
            break;

        pos = (size_t)(found - buf.buf);

        if (buf.size - pos < sig_size)
            break;

        if (memcmp(found, signature, sig_size) == 0) {
            const uint64_le out_layout_va = make_uint64_le(layout_va);

            memcpy(found, out_layout_va.bytes, sig_size);
            return 0;
        }

        buf = buf_get_tail(buf, pos + 1);
    }

    fprintf(stderr, "Error: Live layout signature not found in pe_arith_decode loader\n");
    return 1;
}

BUFFER exe_pe(const void *buf, size_t size)
{
    const PE_HEADER      *pe_header;
    const PE32_HEADER    *opt_header;
    const DATA_DIRECTORY *data_dir;
    const SECTION_HEADER *section_header;
    LAYOUT                layout         = { 0 };
    COMPRESSED_SIZES      compressed;
    size_t                alloc_size;
    BUFFER                mem_image;  /* Memory allocated to create processsed output */
    BUFFER                process_va; /* Image of the program loaded in memory */
    BUFFER                output         = { NULL, 0 };
    BUFFER                iat_data       = { NULL, 0 };
    BUFFER                import_loader  = { NULL, 0 };
    BUFFER                lz77_data      = { NULL, 0 };
    BUFFER                lz77_decomp    = { NULL, 0 };
    BUFFER                comp_data      = { NULL, 0 };
    BUFFER                arith_decoder  = { NULL, 0 };
    BUFFER                header_data    = { NULL, 0 }; /* Put some data in the headers, which are loaded at image_base */
    uint32_t              machine;
    uint32_t              dir_size;
    const uint32_t        pe_offset      = get_pe_offset(buf, size);
    uint32_t              pe_hdrs_size;
    uint32_t              sect_offset;
    uint32_t              num_sections;
    uint32_t              va_start       = ~0U;
    uint32_t              va_end         = 0;
    uint32_t              lz77_data_size = 0;
    uint32_t              import_loader_offs;
    uint32_t              lz77_decomp_offs;
    uint32_t              arith_decoder_offs;
    unsigned int          i;
    int                   error          = 1;
    uint16_t              pe_flags;
    uint16_t              pe_format;

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
            return output;

        default:
            fprintf(stderr, "Error: Unknown architecture 0x%x in PE format\n", machine);
            return output;
    }

    if (get_uint32_le(pe_header->symbol_table_offset) || get_uint32_le(pe_header->number_of_symbols)) {
        fprintf(stderr, "Error: Compressing symbol table in PE format is not supported\n");
        return output;
    }

    pe_flags = get_uint16_le(pe_header->flags);

    if (pe_flags & PE_FLAG_UNSUPPORTED) {
        fprintf(stderr, "Error: Unsupported bits set in characteristics field: 0x%x\n",
                pe_flags & PE_FLAG_UNSUPPORTED);
        return output;
    }

    if (pe_flags & PE_FLAG_DLL) {
        fprintf(stderr, "Error: Compressing DLLs is not supported\n");
        return output;
    }

    sect_offset  = pe_offset + (uint32_t)sizeof(PE_HEADER) + get_uint16_le(pe_header->optional_hdr_size);
    num_sections = get_uint16_le(pe_header->number_of_sections);

    if (sect_offset + num_sections * sizeof(SECTION_HEADER) > size) {
        fprintf(stderr, "Error: Optional header size or sections exceed file size\n");
        return output;
    }

    if (get_uint16_le(pe_header->optional_hdr_size) < sizeof(PE32_HEADER)) {
        fprintf(stderr, "Error: Invalid optional header size\n");
        return output;
    }

    opt_header = (const PE32_HEADER *)at_offset(buf, pe_offset + (uint32_t)sizeof(PE_HEADER));

    pe_format = get_uint16_le(opt_header->pe_format);
    if (pe_format != PE_FORMAT_PE32 && pe_format != PE_FORMAT_PE32_PLUS) {

        fprintf(stderr, "Error: Unsupported format of PE optional header: 0x%x\n", pe_format);
        return output;
    }

    if (get_uint32_le(opt_header->section_alignment) != 0x1000) {
        fprintf(stderr, "Error: Unexpected section alignment %u\n",
                get_uint32_le(opt_header->section_alignment));
        return output;
    }

    /* Print optional header */
    printf("        PE flags                 0x%x\n",              pe_flags);
    printf("optional header:\n");
    printf("        linker ver               %u.%u\n",             opt_header->linker_ver_major, opt_header->linker_ver_minor);
    printf("        code size                0x%x\n",              get_uint32_le(opt_header->size_of_code));
    printf("        data size                0x%x\n",              get_uint32_le(opt_header->size_of_data));
    printf("        uninitialized data size  0x%x\n",              get_uint32_le(opt_header->size_of_uninitialized_data));
    printf("        entry point              0x%x\n",              get_uint32_le(opt_header->entry_point));
    printf("        code base                0x%x\n",              get_uint32_le(opt_header->base_of_code));
    if (pe_format == PE_FORMAT_PE32) {
        printf("        data base                0x%x\n",          get_uint32_le(opt_header->u1.u32.base_of_data));
        printf("        image base               0x%x\n",          get_uint32_le(opt_header->u1.u32.image_base));
    }
    else
        printf("        image base               0x%" PRIx64 "\n", get_uint64_le(opt_header->u1.u64.image_base));
    printf("        section alignment        %u\n",                get_uint32_le(opt_header->section_alignment));
    printf("        file alignment           %u\n",                get_uint32_le(opt_header->file_alignment));
    printf("        min os ver               %u.%u\n",             get_uint16_le(opt_header->min_os_ver_major), get_uint16_le(opt_header->min_os_ver_minor));
    printf("        image ver                %u.%u\n",             get_uint16_le(opt_header->image_ver_major), get_uint16_le(opt_header->image_ver_minor));
    printf("        subsystem ver            %u.%u\n",             get_uint16_le(opt_header->subsystem_ver_major), get_uint16_le(opt_header->subsystem_ver_minor));
    printf("        image size               0x%x\n",              get_uint32_le(opt_header->size_of_image));
    printf("        headers size             0x%x\n",              get_uint32_le(opt_header->size_of_headers));
    printf("        checksum                 0x%x\n",              get_uint32_le(opt_header->checksum));
    printf("        subsystem                %u\n",                get_uint16_le(opt_header->subsystem));
    printf("        dll_flags                0x%x\n",              get_uint16_le(opt_header->dll_flags));
    if (pe_format == PE_FORMAT_PE32) {
        printf("        size_of_stack_reserve    0x%x\n",          get_uint32_le(opt_header->u2.u32.size_of_stack_reserve));
        printf("        size_of_stack_commit     0x%x\n",          get_uint32_le(opt_header->u2.u32.size_of_stack_commit));
        printf("        size_of_head_reserve     0x%x\n",          get_uint32_le(opt_header->u2.u32.size_of_head_reserve));
        printf("        size_of_heap_commit      0x%x\n",          get_uint32_le(opt_header->u2.u32.size_of_heap_commit));
    }
    else {
        printf("        size_of_stack_reserve    0x%" PRIx64 "\n", get_uint64_le(opt_header->u2.u64.size_of_stack_reserve));
        printf("        size_of_stack_commit     0x%" PRIx64 "\n", get_uint64_le(opt_header->u2.u64.size_of_stack_commit));
        printf("        size_of_head_reserve     0x%" PRIx64 "\n", get_uint64_le(opt_header->u2.u64.size_of_head_reserve));
        printf("        size_of_heap_commit      0x%" PRIx64 "\n", get_uint64_le(opt_header->u2.u64.size_of_heap_commit));
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

        if (section_va < 0x1000) {
            fprintf(stderr, "Error: Unexpected section %u starting at RVA 0x%x\n", i, section_va);
            return output;
        }
        value = get_uint32_le(section_header[i].pointer_to_relocations);
        if (value) {
            fprintf(stderr, "Error: pointer_to_relocations is %u in section %u but expected 0\n", value, i);
            return output;
        }
        value = get_uint32_le(section_header[i].pointer_to_line_numbers);
        if (value) {
            fprintf(stderr, "Error: pointer_to_line_numbers is %u in section %u but expected 0\n", value, i);
            return output;
        }
        value = get_uint16_le(section_header[i].number_of_relocations);
        if (value) {
            fprintf(stderr, "Error: number_of_relocations is %u in section %u but expected 0\n", value, i);
            return output;
        }
        value = get_uint16_le(section_header[i].number_of_line_numbers);
        if (value) {
            fprintf(stderr, "Error: number_of_line_numbers is %u in section %u but expected 0\n", value, i);
            return output;
        }
    }

    va_end     = align_up(va_end, 0x1000);
    alloc_size = va_end * 3 + estimate_compress_size(va_end);
    mem_image  = buf_alloc(alloc_size);
    process_va = buf_truncate(mem_image, va_end);
    output     = buf_get_tail(mem_image, va_end);

    if (pe_format == PE_FORMAT_PE32)
        layout.image_base = get_uint32_le(opt_header->u1.u32.image_base);
    else
        layout.image_base = get_uint64_le(opt_header->u1.u64.image_base);
    layout.decomp_base_rva = va_start;
    layout.iat_rva         = va_end;

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

    /* Prepare new PE header */
    header_data = prepare_pe_header(process_va, pe_header, opt_header);

    /* Process data directory entries */
    if (pe_format == PE_FORMAT_PE32) {
        data_dir = opt_header->u2.u32.rva_and_sizes;
        dir_size = get_uint32_le(opt_header->u2.u32.number_of_rva_and_sizes);
    }
    else {
        data_dir = opt_header->u2.u64.rva_and_sizes;
        dir_size = get_uint32_le(opt_header->u2.u64.number_of_rva_and_sizes);
    }

    pe_hdrs_size = (pe_format == PE_FORMAT_PE32) ? 96 : 112;

    if (dir_size == 0 ||
        get_uint16_le(pe_header->optional_hdr_size) != pe_hdrs_size + dir_size * sizeof(DATA_DIRECTORY)) {

        fprintf(stderr, "Error: Unexpected optional header size %u (expected %zu)\n",
                get_uint16_le(pe_header->optional_hdr_size),
                pe_hdrs_size + dir_size * sizeof(DATA_DIRECTORY));
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
                if (process_import_table(process_va, entry_buf, va_start, &iat_data,
                                         pe_format == PE_FORMAT_PE32_PLUS))
                    goto cleanup;

                iat_data.size = align_up((uint32_t)iat_data.size, 16);

                output = buf_get_tail(output, iat_data.size);

                layout.import_loader_rva = layout.iat_rva + (uint32_t)iat_data.size;
                break;

            default:
                fprintf(stderr, "Error: Unsupported data directory table %u (%s)\n", i, name);
                goto cleanup;
        }
    }

    /* Add import loader */
    import_loader = output;
    import_loader_offs = add_loader(&import_loader, "pe_load_imports", machine);
    if (import_loader_offs == ~0U)
        goto cleanup;

    output = buf_get_tail(output, import_loader.size);

    layout.lz77_data_rva = layout.import_loader_rva + (uint32_t)import_loader.size;

    /* TODO separate .text section into streams */
    /* TODO add loader to restore .text section */

    /* Compress the program's address space with LZ77 */
    lz77_data  = output;
    compressed = lz_compress(lz77_data.buf, lz77_data.size, process_va.buf + va_start, layout.lz77_data_rva - va_start);

    if ( ! compressed.lz)
        goto cleanup;

    layout.lz77_decompressor_rva = align_up(layout.lz77_data_rva + (uint32_t)compressed.lz, 16);
    lz77_data.size               = layout.lz77_decompressor_rva - layout.lz77_data_rva;

    output = buf_get_tail(output, lz77_data.size);

    /* Add LZ77 decompressor */
    lz77_decomp = output;
    lz77_decomp_offs = add_loader(&lz77_decomp, "pe_lz_decompress", machine);
    if (lz77_decomp_offs == ~0U)
        goto cleanup;

    lz77_data_size       = (uint32_t)lz77_decomp.size;
    output               = buf_get_tail(output, lz77_decomp.size);
    lz77_data_size      += (uint32_t)lz77_data.size;
    layout.comp_data_rva = layout.lz77_decompressor_rva + (uint32_t)lz77_decomp.size;

    /* Next section is loaded from file, so align it on 4K */
    {
        const uint32_t tail = layout.comp_data_rva % 0x1000;
        const uint32_t fill = tail ? (0x1000 - tail) : 0;

        layout.comp_data_rva += fill;
        output = buf_get_tail(output, fill);
    }

    /* Encode the LZ77-compressed data with arithmetic coder */
    comp_data                = output;
    compressed.compressed    = arith_encode(comp_data.buf, comp_data.size, lz77_data.buf, lz77_data_size);
    comp_data.size           = align_up((uint32_t)compressed.compressed, 16);
    output                   = buf_get_tail(output, comp_data.size);
    layout.arith_decoder_rva = layout.comp_data_rva + (uint32_t)comp_data.size;

    /* Add arithmetic decoder */
    arith_decoder = output;
    arith_decoder_offs = add_loader(&arith_decoder, "pe_arith_decode", machine);
    if (arith_decoder_offs == ~0U)
        goto cleanup;

    output                = buf_get_tail(output, arith_decoder.size);
    layout.import_dir_rva = layout.arith_decoder_rva + (uint32_t)arith_decoder.size;

    /* Add import directory */
    {
        BUFFER   import_dir;
        uint32_t import_dir_size;

        import_dir = output;
        import_dir_size = add_mini_import_dir(&import_dir, layout.import_dir_rva, pe_format);
        if ( ! import_dir_size)
            goto cleanup;

        output = buf_get_tail(output, import_dir.size);
        layout.live_layout_rva = layout.import_dir_rva + (uint32_t)import_dir.size;
        layout.mini_iat_rva    = layout.import_dir_rva + import_dir_size;
    }

    /* Add layout which is used by the loaders */
    if (add_live_layout(output,
                        &layout,
                        get_uint32_le(opt_header->entry_point),
                        pe_format,
                        import_loader_offs,
                        lz77_decomp_offs,
                        lz77_data_size,
                        (uint32_t)compressed.compressed))
        goto cleanup;
    output = buf_get_tail(output, layout.end_rva - layout.live_layout_rva);

    /* Patch arithmetic decoder to locate live layout */
    if (patch_live_layout(arith_decoder, pe_format, &layout))
        goto cleanup;

    fill_pe_header(process_va,
                   &layout,
                   layout.arith_decoder_rva + arith_decoder_offs,
                   opt_header);

    printf("Wasted %u bytes in the header\n", new_header_size - (uint32_t)header_data.size);

    printf("Process virtual address space layout:\n");
    printf("        image base               0x%" PRIx64 "\n",   layout.image_base);
    printf("        decomp base              0x%x\n",            layout.decomp_base_rva);
    printf("        iat rva                  0x%x (%u bytes)\n", layout.iat_rva,               layout.import_loader_rva - layout.iat_rva);
    printf("        import loader rva        0x%x (%u bytes)\n", layout.import_loader_rva,     layout.lz77_data_rva - layout.import_loader_rva);
    printf("        lz77 data rva            0x%x (%u bytes)\n", layout.lz77_data_rva,         layout.lz77_decompressor_rva - layout.lz77_data_rva);
    printf("        lz77 decompressor rva    0x%x (%u bytes)\n", layout.lz77_decompressor_rva, lz77_data_size - (layout.lz77_decompressor_rva - layout.lz77_data_rva));
    printf("        comp data rva            0x%x (%u bytes)\n", layout.comp_data_rva,         layout.arith_decoder_rva - layout.comp_data_rva);
    printf("        arith decoder rva        0x%x (%u bytes)\n", layout.arith_decoder_rva,     layout.import_dir_rva - layout.arith_decoder_rva);
    printf("        import dir rva           0x%x (%u bytes)\n", layout.import_dir_rva,        layout.mini_iat_rva - layout.import_dir_rva);
    printf("        mini iat rva             0x%x (%u bytes)\n", layout.mini_iat_rva,          layout.live_layout_rva - layout.mini_iat_rva);
    printf("        live layout rva          0x%x (%u bytes)\n", layout.live_layout_rva,       layout.end_rva - layout.live_layout_rva);
    printf("        end rva                  0x%x\n",            layout.end_rva);

    /* Verify compression */
    if (verify_compression(mem_image, &layout, output, lz77_data_size, (uint32_t)compressed.compressed))
        goto cleanup;

    /* Produce final file image */
    output = mem_image;
    memcpy(output.buf + new_header_size,
           comp_data.buf,
           layout.end_rva - layout.comp_data_rva);

    output.size = new_header_size + (layout.end_rva - layout.comp_data_rva);

    error = 0;

cleanup:
    if (error) {
        free(mem_image.buf);

        output.buf  = NULL;
        output.size = 0;
    }

    return output;
}
