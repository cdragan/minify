/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "exe_macho.h"
#include "arith_encode.h"
#include "buffer.h"
#include "load_file.h"
#include "lza_compress.h"
#include "macho_common.h"
#include "macho_sign.h"
#include "static_assert.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

/*
 * Two address spaces matter to this packer and are diagrammed below: the
 * output FILE layout (what the kernel maps from disk) and the runtime VM
 * layout (where dyld places each segment, sorted by vmaddr).  The loader
 * locates everything at decompression time through MACHO_LIVE_LAYOUT (see
 * macho_common.h), whose pointer fields are offsets from the runtime image
 * base.
 *
 * Runtime sequence: dyld maps the file, binds the __DATA_CONST GOT from the
 * trimmed chained fixups, and jumps to the loader (LC_MAIN).  The loader
 * gathers the arith payload from its scattered file ranges into __SCRATCH,
 * arith-decodes it into the LZ77 byte stream, LZ-decompresses that into
 * __UNPACK (splitting the trailing __DATA bytes back out when folded),
 * mprotects __UNPACK to RX, self-rebases a folded __DATA, and jumps to the
 * original entry point.
 *
 * Output FILE layout (ascending file offset).  The arith payload is split
 * across three ranges so no file space is wasted.  The alignment gap and the
 * __DATA_CONST page padding are reused without enlarging the file, so they
 * are filled first; only the __TEXT tail can grow the file, so it is grown
 * last:
 *
 * 0 ------------------> +------------------+ <- Mach-O header + load commands, including the two
 *                       | header + cmds    |    inserted __UNPACK / __SCRATCH segment commands
 * cmds_end_offs ------> +------------------+ <- Alignment gap before the loader holds payload
 *                       | payload range 1  |    slice 1 (payload_gap); does not grow the file
 * loader_offs --------> +------------------+ <- Loader stub, placed so its low 12 file-offset bits
 *                       |      loader      |    match the compile-time __TEXT (PC-relative refs work)
 * payload_offs -------> +------------------+ <- __TEXT tail holds payload slice 2 (payload_tail);
 *                       | payload range 2  |    this is the only slice that extends __TEXT
 * out_text_filesize --> +------------------+ <- __TEXT.filesize, page aligned (rest is zero-fill)
 * data_const_fileoff -> +------------------+ <- __DATA_CONST copied verbatim (dyld binds its GOT)
 *                       |   __DATA_CONST   |
 *                       | . . . . . . . . .| <- __DATA_CONST page padding holds payload slice 3
 *                       | payload range 3  |    (payload_data_const); that page is mandatory anyway
 * data_fileoff -------> +------------------+ <- __DATA verbatim, ONLY when not folded; a folded
 *                       |  __DATA verbatim |    __DATA has no file bytes (loader rebuilds it)
 * linkedit_fileoff ---> +------------------+ <- Trimmed __LINKEDIT: just the chained fixups dyld
 *                       | chained fixups   |    needs for the GOT (symtab/exports/etc. dropped)
 * code_limit ---------> +------------------+ <- Ad-hoc code signature; CodeDirectory hashes
 *                       |    signature     |    [0, code_limit)
 *                       +------------------+
 *
 * Runtime VM layout (ascending vmaddr; dyld requires segments sorted this
 * way).  __DATA_CONST, __DATA and __LINKEDIT are each shifted up by vm_shift
 * (== the original __TEXT.vmsize) to make room for __UNPACK right after
 * __TEXT:
 *
 * 0 ------------------> +------------------+ <- __PAGEZERO (unmapped)
 * image_base ---------> +------------------+ <- __TEXT maps here from file offset 0.  vmsize is kept
 *                       |      __TEXT      |    at the ORIGINAL value (codesign requires the section
 *                       | header, cmds,    |    table to stay within segment bounds).  Holds the
 *                       | loader, payload  |    header+cmds, the loader and payload slice 2
 * +vm_shift ----------> +------------------+ <- __UNPACK: zero-fill RW the size of the original
 *                       |     __UNPACK     |    __TEXT.  Loader decompresses __TEXT here, then
 *                       | (decomp __TEXT)  |    mprotects it RX before jumping to the entry point
 * data_const_vmaddr --> +------------------+ <- __DATA_CONST verbatim (GOT page)
 *                       |   __DATA_CONST   |
 * data_vmaddr --------> +------------------+ <- __DATA: verbatim, or (folded) anonymous zero-fill RW
 *                       |      __DATA      |    that the loader reconstructs and self-rebases
 * linkedit_vmaddr ----> +------------------+ <- __LINKEDIT (chained fixups + signature)
 *                       |    __LINKEDIT    |
 * scratch_vmaddr -----> +------------------+ <- __SCRATCH: zero-fill RW work area, placed past
 *                       |     __SCRATCH    |    __LINKEDIT (+1 MB) so it does not perturb vm_shift.
 *                       | LZ buf, arith    |    Holds the gathered arith payload, the LZ77 stream
 *                       | gather, raw blob |    and the combined raw [__TEXT][rebases][__data]
 *                       +------------------+
 */

/* Mach-O on-disk structures.  Only what we touch is declared here. */

#define MH_MAGIC_64                   0xFEEDFACFU
#define MH_CIGAM_64                   0xCFFAEDFEU

#define CPU_TYPE_ARM64                0x0100000CU

#define MH_EXECUTE                    0x2U
#define MH_PIE                        0x00200000U

#define LC_REQ_DYLD                   0x80000000U

#define LC_SEGMENT_64                 0x19U
#define LC_SYMTAB                     0x02U
#define LC_DYSYMTAB                   0x0BU
#define LC_FUNCTION_STARTS            0x26U
#define LC_DATA_IN_CODE               0x29U
#define LC_DYLD_EXPORTS_TRIE          (0x33U | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS        (0x34U | LC_REQ_DYLD)
#define LC_MAIN                       (0x28U | LC_REQ_DYLD)
#define LC_CODE_SIGNATURE             0x1DU

#define VM_PROT_READ                  0x1U
#define VM_PROT_WRITE                 0x2U
#define VM_PROT_EXECUTE               0x4U
#define VM_PROT_RX                    (VM_PROT_READ | VM_PROT_EXECUTE)
#define VM_PROT_RW                    (VM_PROT_READ | VM_PROT_WRITE)

#define MACOS_ARM64_PAGE              0x4000U
#define ARM64_ADRP_PAGE               0x1000U

/* Section type byte (low 8 bits of SECTION_64.flags) values that make dyld
 * call into the section at load time.  Neutralized to S_REGULAR (0) in the
 * packed output, since dyld would otherwise invoke shifted/compressed code.
 */
#define SECTION_TYPE_MASK             0xFFU
#define S_MOD_INIT_FUNC_POINTERS      0x09U
#define S_MOD_TERM_FUNC_POINTERS      0x0AU
#define S_INIT_FUNC_OFFSETS           0x16U

/* Chained-fixups format identifiers we need to handle on arm64.  Other
 * formats exist (arm64e, 32-bit) but are not part of our target set.
 */
#define DYLD_CHAINED_PTR_64           2U
#define DYLD_CHAINED_PTR_64_OFFSET    6U

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} MACHO_HEADER_64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} LOAD_COMMAND;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} SEGMENT_COMMAND_64;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} SECTION_64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} ENTRY_POINT_COMMAND;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
} LINKEDIT_DATA_COMMAND;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} SYMTAB_COMMAND;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} DYSYMTAB_COMMAND;

/* dyld chained-fixups header (LC_DYLD_CHAINED_FIXUPS dataoff -> this). */
typedef struct {
    uint32_t fixups_version;
    uint32_t starts_offset;
    uint32_t imports_offset;
    uint32_t symbols_offset;
    uint32_t imports_count;
    uint32_t imports_format;
    uint32_t symbols_format;
} DYLD_CHAINED_FIXUPS_HEADER;

typedef struct {
    uint32_t seg_count;
    uint32_t seg_info_offset[1];
} DYLD_CHAINED_STARTS_IN_IMAGE;

typedef struct {
    uint32_t size;
    uint16_t page_size;
    uint16_t pointer_format;
    uint64_t segment_offset;
    uint32_t max_valid_pointer;
    uint16_t page_count;
    uint16_t page_start[1];
} DYLD_CHAINED_STARTS_IN_SEGMENT;

#define DYLD_CHAINED_PTR_START_NONE  0xFFFFU
#define DYLD_CHAINED_PTR_START_MULTI 0x8000U

STATIC_ASSERT(sizeof(MACHO_HEADER_64)       == 32, "MACHO_HEADER_64 layout changed");
STATIC_ASSERT(sizeof(LOAD_COMMAND)          == 8,  "LOAD_COMMAND layout changed");
STATIC_ASSERT(sizeof(SEGMENT_COMMAND_64)    == 72, "SEGMENT_COMMAND_64 layout changed");
STATIC_ASSERT(sizeof(SECTION_64)            == 80, "SECTION_64 layout changed");
STATIC_ASSERT(sizeof(ENTRY_POINT_COMMAND)   == 24, "ENTRY_POINT_COMMAND layout changed");
STATIC_ASSERT(sizeof(LINKEDIT_DATA_COMMAND) == 16, "LINKEDIT_DATA_COMMAND layout changed");
STATIC_ASSERT(sizeof(SYMTAB_COMMAND)        == 24, "SYMTAB_COMMAND layout changed");
STATIC_ASSERT(sizeof(DYSYMTAB_COMMAND)      == 80, "DYSYMTAB_COMMAND layout changed");

typedef struct {
    const MACHO_HEADER_64    *header;
    const SEGMENT_COMMAND_64 *text_seg;
    const SEGMENT_COMMAND_64 *data_const_seg;
    const SEGMENT_COMMAND_64 *data_seg;
    const SEGMENT_COMMAND_64 *linkedit_seg;
    const SEGMENT_COMMAND_64 *page_zero_seg;
    const ENTRY_POINT_COMMAND *entry_point;
    const LINKEDIT_DATA_COMMAND *chained_fixups_cmd;
    const LINKEDIT_DATA_COMMAND *code_sig_cmd;
    uint64_t                  image_base;
} MACHO_INPUT;

typedef struct {
    BUFFER   bytes;
    uint64_t text_vmaddr;
    uint32_t layout_offs;
    uint32_t entry_offs;
} LOADER_BLOB;

int is_macho_file(const void *buf, size_t size)
{
    uint32_t magic;

    if (size < sizeof(uint32_t)) {
        return 0;
    }

    memcpy(&magic, buf, sizeof(magic));
    return magic == MH_MAGIC_64 || magic == MH_CIGAM_64;
}

static uint64_t align_up_u64(uint64_t value, uint64_t align)
{
    assert(align > 0U && (align & (align - 1U)) == 0U);
    return (value + align - 1U) & ~(align - 1U);
}

static int seg_name_equals(const SEGMENT_COMMAND_64 *seg, const char *name)
{
    const size_t name_len = strlen(name);
    return name_len < sizeof(seg->segname) &&
           memcmp(seg->segname, name, name_len) == 0 &&
           seg->segname[name_len] == 0;
}

/* Unlike segment names, a section name may fill all 16 bytes with no NUL
 * terminator, so a name whose length equals sizeof(sectname) is accepted as an
 * exact match (seg_name_equals requires the name to be strictly shorter).
 */
static int sect_name_equals(const SECTION_64 *sect, const char *name)
{
    const size_t name_len = strlen(name);
    return name_len <= sizeof(sect->sectname) &&
           memcmp(sect->sectname, name, name_len) == 0 &&
           (name_len == sizeof(sect->sectname) || sect->sectname[name_len] == 0);
}

static const SECTION_64 *find_section(const SEGMENT_COMMAND_64 *seg, const char *sectname)
{
    const SECTION_64 *sect = (const SECTION_64 *)(seg + 1);
    uint32_t          i;

    for (i = 0; i < seg->nsects; i++) {
        if (sect_name_equals(&sect[i], sectname)) {
            return &sect[i];
        }
    }
    return NULL;
}

/* Extent of a segment's real (section-described) content, in bytes from the
 * segment start.  The trailing space up to filesize is padding we can reuse.
 */
static uint64_t segment_content_size(const SEGMENT_COMMAND_64 *seg)
{
    const SECTION_64 *sect = (const SECTION_64 *)(seg + 1);
    uint64_t          max_end = 0;
    uint32_t          i;

    for (i = 0; i < seg->nsects; i++) {
        const uint64_t end = (sect[i].addr - seg->vmaddr) + sect[i].size;
        if (end > max_end) {
            max_end = end;
        }
    }
    return max_end;
}

/* True if any section in `seg` has a name starting with `prefix`. */
static int segment_has_section_prefix(const SEGMENT_COMMAND_64 *seg, const char *prefix)
{
    const SECTION_64 *sect;
    const size_t      prefix_len = strlen(prefix);
    uint32_t          i;

    if ( ! seg) {
        return 0;
    }
    sect = (const SECTION_64 *)(seg + 1);
    for (i = 0; i < seg->nsects; i++) {
        if (memcmp(sect[i].sectname, prefix, prefix_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Objective-C / Swift metadata sections require load-time runtime
 * registration (class realization, selector/category registration) that we
 * do not replay, so a binary carrying them is not eligible for __DATA
 * compression -- the caller falls back to copying __DATA verbatim.
 */
static int macho_has_objc_or_swift(const MACHO_INPUT *input)
{
    const SEGMENT_COMMAND_64 *segs[3];
    size_t                    i;

    segs[0] = input->text_seg;
    segs[1] = input->data_const_seg;
    segs[2] = input->data_seg;
    for (i = 0; i < 3; i++) {
        if (segment_has_section_prefix(segs[i], "__objc") ||
            segment_has_section_prefix(segs[i], "__swift")) {
            return 1;
        }
    }
    return 0;
}

/* Walk __DATA's chained-fixup chain in the input image and collect its rebase
 * targets.  When `out` is NULL only *count is produced (counting pass).
 *
 * Sets *eligible to 0 (and returns 0) if __DATA cannot be self-rebased -- a
 * bind link, an unsupported pointer format, a multi-start page, or no __DATA
 * chain at all -- so the caller copies __DATA verbatim instead.  Returns
 * non-zero only on a malformed chain (hard error).
 */
static int walk_data_chain(const uint8_t      *input_bytes,
                           const MACHO_INPUT  *input,
                           uint64_t            vm_shift,
                           MACHO_DATA_REBASE  *out,
                           uint32_t           *count,
                           int                *eligible)
{
    const DYLD_CHAINED_FIXUPS_HEADER   *hdr;
    const DYLD_CHAINED_STARTS_IN_IMAGE *image_starts;
    const uint8_t                      *fixups_data;
    const uint8_t                      *data_bytes;
    uint64_t                            data_voff;
    uint32_t                            seg_idx;

    *count    = 0;
    *eligible = 1;

    if ( ! input->chained_fixups_cmd || ! input->data_seg ||
         input->data_seg->filesize == 0U) {
        *eligible = 0;
        return 0;
    }

    fixups_data  = input_bytes + input->chained_fixups_cmd->dataoff;
    hdr          = (const DYLD_CHAINED_FIXUPS_HEADER *)fixups_data;
    if (hdr->fixups_version != 0U) {
        *eligible = 0;
        return 0;
    }
    image_starts = (const DYLD_CHAINED_STARTS_IN_IMAGE *)(fixups_data + hdr->starts_offset);
    data_bytes   = input_bytes + input->data_seg->fileoff;
    data_voff    = input->data_seg->vmaddr - input->image_base;

    for (seg_idx = 0; seg_idx < image_starts->seg_count; seg_idx++) {
        const uint32_t                        seg_info_offs = image_starts->seg_info_offset[seg_idx];
        const DYLD_CHAINED_STARTS_IN_SEGMENT *seg_starts;
        uint32_t                              page_idx;

        if (seg_info_offs == 0U) {
            continue;
        }
        seg_starts = (const DYLD_CHAINED_STARTS_IN_SEGMENT *)((const uint8_t *)image_starts + seg_info_offs);
        if (seg_starts->segment_offset != data_voff) {
            continue;
        }
        if (seg_starts->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            *eligible = 0;
            return 0;
        }

        for (page_idx = 0; page_idx < seg_starts->page_count; page_idx++) {
            const uint16_t page_start = seg_starts->page_start[page_idx];
            uint64_t       chain_offs;

            if (page_start == DYLD_CHAINED_PTR_START_NONE) {
                continue;
            }
            if (page_start & DYLD_CHAINED_PTR_START_MULTI) {
                *eligible = 0;
                return 0;
            }

            chain_offs = (uint64_t)page_idx * seg_starts->page_size + page_start;
            for (;;) {
                uint64_t raw;
                uint64_t next;

                if (chain_offs + sizeof(uint64_t) > input->data_seg->filesize) {
                    fprintf(stderr, "Error: __DATA chain link out of bounds\n");
                    return 1;
                }
                memcpy(&raw, data_bytes + chain_offs, sizeof(raw));
                next = (raw >> 51) & 0xFFFU;

                if ((raw >> 63) & 1U) {
                    /* Bind: we have no symbol resolver in the loader. */
                    *eligible = 0;
                    return 0;
                }

                if (out) {
                    out[*count].slot_offs   = data_voff + chain_offs + vm_shift;
                    out[*count].target_offs = (raw & 0xFFFFFFFFFULL) + vm_shift;
                }
                (*count)++;

                if (next == 0U) {
                    break;
                }
                chain_offs += next * 4U;
            }
        }
        break;
    }

    return 0;
}

static int parse_macho_input(const void *buf, size_t size, MACHO_INPUT *out)
{
    const uint8_t      *cursor;
    const uint8_t      *cmds_end;
    const LOAD_COMMAND *load_cmd;
    uint32_t            ncmds;
    uint32_t            i;

    memset(out, 0, sizeof(*out));

    if (size < sizeof(MACHO_HEADER_64)) {
        fprintf(stderr, "Error: file is too small to contain a Mach-O header\n");
        return 1;
    }

    out->header = (const MACHO_HEADER_64 *)buf;

    if (out->header->magic != MH_MAGIC_64) {
        fprintf(stderr, "Error: unsupported Mach-O magic 0x%08x\n", out->header->magic);
        return 1;
    }

    if (out->header->cputype != CPU_TYPE_ARM64) {
        fprintf(stderr, "Error: only arm64 supported (cputype=0x%08x)\n", out->header->cputype);
        return 1;
    }

    if (out->header->filetype != MH_EXECUTE) {
        fprintf(stderr, "Error: only EXECUTE filetype supported (filetype=0x%x)\n",
                out->header->filetype);
        return 1;
    }

    if ((out->header->flags & MH_PIE) == 0) {
        fprintf(stderr, "Error: input must be PIE\n");
        return 1;
    }

    if (size < sizeof(MACHO_HEADER_64) + out->header->sizeofcmds) {
        fprintf(stderr, "Error: file too small for declared load commands\n");
        return 1;
    }

    cursor   = (const uint8_t *)buf + sizeof(MACHO_HEADER_64);
    cmds_end = cursor + out->header->sizeofcmds;
    ncmds    = out->header->ncmds;

    for (i = 0; i < ncmds; i++) {
        if ((size_t)(cmds_end - cursor) < sizeof(LOAD_COMMAND)) {
            fprintf(stderr, "Error: load command %u truncated\n", i);
            return 1;
        }
        load_cmd = (const LOAD_COMMAND *)cursor;

        if (load_cmd->cmdsize < sizeof(LOAD_COMMAND) ||
            (size_t)(cmds_end - cursor) < load_cmd->cmdsize) {
            fprintf(stderr, "Error: load command %u invalid cmdsize %u\n", i, load_cmd->cmdsize);
            return 1;
        }

        switch (load_cmd->cmd) {

            case LC_SEGMENT_64: {
                const SEGMENT_COMMAND_64 *seg = (const SEGMENT_COMMAND_64 *)cursor;

                if (load_cmd->cmdsize < sizeof(SEGMENT_COMMAND_64)) {
                    fprintf(stderr, "Error: LC_SEGMENT_64 invalid cmdsize\n");
                    return 1;
                }

                if (seg_name_equals(seg, "__PAGEZERO")) {
                    out->page_zero_seg = seg;
                }
                else if (seg_name_equals(seg, "__TEXT")) {
                    out->text_seg = seg;
                    out->image_base = seg->vmaddr;
                }
                else if (seg_name_equals(seg, "__DATA_CONST")) {
                    out->data_const_seg = seg;
                }
                else if (seg_name_equals(seg, "__DATA")) {
                    out->data_seg = seg;
                }
                else if (seg_name_equals(seg, "__LINKEDIT")) {
                    out->linkedit_seg = seg;
                }
                else {
                    fprintf(stderr, "Error: unsupported segment '%.16s'\n", seg->segname);
                    return 1;
                }
                break;
            }

            case LC_MAIN:
                if (load_cmd->cmdsize < sizeof(ENTRY_POINT_COMMAND)) {
                    fprintf(stderr, "Error: LC_MAIN invalid cmdsize\n");
                    return 1;
                }
                out->entry_point = (const ENTRY_POINT_COMMAND *)cursor;
                break;

            case LC_DYLD_CHAINED_FIXUPS:
                out->chained_fixups_cmd = (const LINKEDIT_DATA_COMMAND *)cursor;
                break;

            case LC_CODE_SIGNATURE:
                out->code_sig_cmd = (const LINKEDIT_DATA_COMMAND *)cursor;
                break;

            default:
                break;
        }

        cursor += load_cmd->cmdsize;
    }

    if ( ! out->text_seg) {
        fprintf(stderr, "Error: no __TEXT segment\n");
        return 1;
    }
    if ( ! out->linkedit_seg) {
        fprintf(stderr, "Error: no __LINKEDIT segment\n");
        return 1;
    }
    if ( ! out->entry_point) {
        fprintf(stderr, "Error: no LC_MAIN\n");
        return 1;
    }
    if ( ! out->code_sig_cmd) {
        fprintf(stderr, "Error: no LC_CODE_SIGNATURE (input must be signed)\n");
        return 1;
    }

    return 0;
}

static int load_loader(LOADER_BLOB *out, const char *loader_name)
{
    char                       filename[64];
    BUFFER                     file_buf;
    const MACHO_HEADER_64     *header;
    const uint8_t             *cursor;
    const LOAD_COMMAND        *load_cmd;
    const SEGMENT_COMMAND_64  *text_seg = NULL;
    const SECTION_64          *text_section = NULL;
    const SECTION_64          *layout_section = NULL;
    const ENTRY_POINT_COMMAND *loader_entry = NULL;
    uint32_t                   i;
    uint64_t                   blob_start;
    uint64_t                   blob_end;
    BUFFER                     blob;
    int                        error = 1;
    const char                *candidate_dirs[] = { "Out/loaders", "loaders/macos/arm64" };
    size_t                     dir_idx;

    out->bytes.buf = NULL;
    out->bytes.size = 0;
    file_buf.buf = NULL;
    file_buf.size = 0;

    for (dir_idx = 0; dir_idx < sizeof(candidate_dirs) / sizeof(candidate_dirs[0]); dir_idx++) {
        FILE *probe;
        snprintf(filename, sizeof(filename), "%s/%s", candidate_dirs[dir_idx], loader_name);
        /* Probe before load_file() so a missing dir does not perror. */
        probe = fopen(filename, "rb");
        if ( ! probe) {
            continue;
        }
        fclose(probe);
        file_buf = load_file(filename);
        if (file_buf.buf) {
            break;
        }
    }

    if ( ! file_buf.buf) {
        fprintf(stderr, "Error: failed to load loader '%s'\n", loader_name);
        return 1;
    }

    if (file_buf.size < sizeof(MACHO_HEADER_64)) {
        fprintf(stderr, "Error: loader '%s' too small\n", loader_name);
        goto cleanup;
    }
    header = (const MACHO_HEADER_64 *)file_buf.buf;

    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) {
        fprintf(stderr, "Error: loader '%s' not arm64 Mach-O\n", loader_name);
        goto cleanup;
    }

    cursor = (const uint8_t *)file_buf.buf + sizeof(MACHO_HEADER_64);

    for (i = 0; i < header->ncmds; i++) {
        load_cmd = (const LOAD_COMMAND *)cursor;
        if (load_cmd->cmd == LC_SEGMENT_64) {
            const SEGMENT_COMMAND_64 *seg = (const SEGMENT_COMMAND_64 *)cursor;
            if (seg_name_equals(seg, "__TEXT")) {
                text_seg         = seg;
                text_section     = find_section(seg, "__text");
                layout_section = find_section(seg, "__layout");
            }
        }
        else if (load_cmd->cmd == LC_MAIN) {
            loader_entry = (const ENTRY_POINT_COMMAND *)cursor;
        }
        cursor += load_cmd->cmdsize;
    }

    if ( ! text_section || ! layout_section || ! loader_entry) {
        fprintf(stderr, "Error: loader '%s' missing __text/__layout/LC_MAIN\n", loader_name);
        goto cleanup;
    }

    /* LC_MAIN.entryoff is a file offset (segment-relative for __TEXT
     * starting at fileoff 0).  Convert to an offset within our extracted
     * blob (which starts at __text.offset).
     */
    if (loader_entry->entryoff < text_section->offset) {
        fprintf(stderr, "Error: loader '%s' entry %llu before __text offset %u\n",
                loader_name, (unsigned long long)loader_entry->entryoff, text_section->offset);
        goto cleanup;
    }

    if (layout_section->size != sizeof(MACHO_LIVE_LAYOUT)) {
        fprintf(stderr, "Error: loader '%s' __layout size mismatch\n", loader_name);
        goto cleanup;
    }

    /* The blob is anchored at __text and copied verbatim, so PC-relative
     * references inside it stay valid once relocated.  The loader's code may
     * reach beyond __text and __layout: -Oz pools constants (e.g. a 16-byte
     * vector literal) into __TEXT,__const, loaded with a PC-relative LDR.
     * Such sections must travel with the blob at their original offsets.
     * Cover every __TEXT section except the unwind metadata, which the
     * running loader never reads and which ld64 places after the code/const
     * sections; including it would just bloat the embedded blob.
     */
    blob_start = text_section->offset;
    blob_end   = (uint64_t)layout_section->offset + layout_section->size;
    {
        const SECTION_64 *sect = (const SECTION_64 *)(text_seg + 1);
        for (i = 0; i < text_seg->nsects; i++) {
            uint64_t sect_end;
            if (sect_name_equals(&sect[i], "__unwind_info") ||
                sect_name_equals(&sect[i], "__eh_frame")) {
                continue;
            }
            if (sect[i].offset < blob_start) {
                fprintf(stderr, "Error: loader '%s' __TEXT section '%.16s' before __text\n",
                        loader_name, sect[i].sectname);
                goto cleanup;
            }
            sect_end = (uint64_t)sect[i].offset + sect[i].size;
            if (sect_end > blob_end) {
                blob_end = sect_end;
            }
        }
    }

    if (blob_start >= blob_end || blob_end > file_buf.size) {
        fprintf(stderr, "Error: loader '%s' __text/__layout outside file\n", loader_name);
        goto cleanup;
    }

    blob = buf_alloc(blob_end - blob_start);
    if ( ! blob.buf) {
        fprintf(stderr, "Error: OOM extracting loader '%s'\n", loader_name);
        goto cleanup;
    }

    memcpy(blob.buf, (const uint8_t *)file_buf.buf + blob_start, blob.size);

    out->bytes       = blob;
    out->text_vmaddr = text_section->addr;
    out->layout_offs = (uint32_t)(layout_section->addr - text_section->addr);
    out->entry_offs  = (uint32_t)(loader_entry->entryoff - text_section->offset);
    error = 0;

cleanup:
    free(file_buf.buf);
    return error;
}

/* Align a file/vmaddr offset so that its low 12 bits equal `target_low12`.
 * Used to place a loader blob at a position where its compile-time
 * PC-relative ADRP+ADD pairs into __layout still resolve correctly.
 */
static uint64_t align_to_low12(uint64_t min_offs, uint64_t target_low12)
{
    uint64_t base = (min_offs & ~(uint64_t)(ARM64_ADRP_PAGE - 1U)) + target_low12;
    if (base < min_offs) {
        base += ARM64_ADRP_PAGE;
    }
    return base;
}

/* The output is already ad-hoc signed in the buffer (macho_adhoc_sign), which
 * is sufficient to run on macOS arm64.  All that remains for a runnable file
 * is the executable permission bit, which save_file does not set.
 */
int macho_set_executable(const char *path)
{
#if !defined(_WIN32)
    if (chmod(path, 0755) != 0) {
        perror("chmod");
        return 1;
    }
#else
    (void)path;
#endif
    return 0;
}

/* Walk the chained-fixups stream and add `vm_shift` to every rebase
 * chain link's target offset.  Bind chain links (which reference dylib
 * imports by ordinal) are not touched.
 */
static int shift_chained_fixups(uint8_t        *linkedit_buf,
                                uint64_t        linkedit_fileoff,
                                uint32_t        fixups_dataoff,
                                uint32_t        fixups_datasize,
                                uint8_t        *data_const_seg_bytes,
                                uint64_t        data_const_seg_vmaddr_offs,
                                uint64_t        data_const_seg_size,
                                uint8_t        *data_seg_bytes,
                                uint64_t        data_seg_vmaddr_offs,
                                uint64_t        data_seg_size,
                                uint64_t        vm_shift,
                                int             data_folded)
{
    DYLD_CHAINED_FIXUPS_HEADER  *hdr;
    DYLD_CHAINED_STARTS_IN_IMAGE *image_starts;
    uint8_t                     *fixups_data;
    uint32_t                     seg_idx;

    if (fixups_dataoff < linkedit_fileoff ||
        fixups_dataoff - linkedit_fileoff + fixups_datasize < fixups_datasize) {
        fprintf(stderr, "Error: chained-fixups dataoff out of __LINKEDIT\n");
        return 1;
    }

    fixups_data = linkedit_buf + (fixups_dataoff - linkedit_fileoff);
    hdr = (DYLD_CHAINED_FIXUPS_HEADER *)fixups_data;

    if (hdr->fixups_version != 0U) {
        fprintf(stderr, "Error: unsupported chained-fixups version %u\n", hdr->fixups_version);
        return 1;
    }

    image_starts = (DYLD_CHAINED_STARTS_IN_IMAGE *)(fixups_data + hdr->starts_offset);

    for (seg_idx = 0; seg_idx < image_starts->seg_count; seg_idx++) {
        const uint32_t                  seg_info_offs = image_starts->seg_info_offset[seg_idx];
        DYLD_CHAINED_STARTS_IN_SEGMENT *seg_starts;
        uint8_t                        *seg_bytes;
        uint64_t                        seg_size;
        uint32_t                        page_idx;

        if (seg_info_offs == 0U) {
            continue;
        }

        seg_starts = (DYLD_CHAINED_STARTS_IN_SEGMENT *)((uint8_t *)image_starts + seg_info_offs);

        /* When __DATA was folded into the compressed payload its bytes are
         * zero at load time, so dyld must not walk its chain (it would
         * dereference zero links).  Neutralize the entry; the LZ decompressor
         * self-applies these rebases after it reconstructs __DATA.
         */
        if (data_folded && seg_starts->segment_offset == data_seg_vmaddr_offs) {
            image_starts->seg_info_offset[seg_idx] = 0U;
            continue;
        }

        if (seg_starts->page_count == 0U) {
            continue;
        }
        if (seg_starts->pointer_format != DYLD_CHAINED_PTR_64 &&
            seg_starts->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            fprintf(stderr, "Error: unsupported chain pointer_format %u in seg %u\n",
                    seg_starts->pointer_format, seg_idx);
            return 1;
        }

        /* Identify which output segment buffer holds this chain. */
        if (seg_starts->segment_offset == data_const_seg_vmaddr_offs) {
            seg_bytes = data_const_seg_bytes;
            seg_size  = data_const_seg_size;
        }
        else if (seg_starts->segment_offset == data_seg_vmaddr_offs) {
            seg_bytes = data_seg_bytes;
            seg_size  = data_seg_size;
        }
        else {
            /* Segment we do not have buffered and do not shift (e.g. __TEXT).
             * Leave its links and segment_offset untouched.
             */
            seg_bytes = NULL;
            seg_size  = 0;
        }

        /* Walk each page chain and rewrite rebase targets. */
        if (seg_bytes) {
            for (page_idx = 0; page_idx < seg_starts->page_count; page_idx++) {
                const uint16_t page_start = seg_starts->page_start[page_idx];
                uint64_t       chain_offs;
                uint64_t       page_offs_in_seg;

                if (page_start == DYLD_CHAINED_PTR_START_NONE) {
                    continue;
                }
                if (page_start & DYLD_CHAINED_PTR_START_MULTI) {
                    fprintf(stderr, "Error: multi-start chains not supported\n");
                    return 1;
                }

                page_offs_in_seg = (uint64_t)page_idx * seg_starts->page_size;
                chain_offs       = page_offs_in_seg + page_start;

                for (;;) {
                    uint64_t *slot;
                    uint64_t  raw;
                    uint64_t  next;
                    int       is_bind;

                    if (chain_offs + sizeof(uint64_t) > seg_size) {
                        fprintf(stderr, "Error: chain link out of segment bounds\n");
                        return 1;
                    }

                    slot = (uint64_t *)(seg_bytes + chain_offs);
                    raw  = *slot;
                    is_bind = (raw >> 63) & 1U;
                    next    = (raw >> 51) & 0xFFFU;

                    if ( ! is_bind) {
                        /* Rebase: bits 0-35 = target, bits 36-43 = high8.
                         * Add vm_shift to the target field.
                         */
                        const uint64_t target = raw & 0xFFFFFFFFFULL;
                        const uint64_t high8  = (raw >> 36) & 0xFFULL;
                        const uint64_t reserved = (raw >> 44) & 0x7FULL;
                        const uint64_t new_target = target + vm_shift;

                        if (new_target >> 36) {
                            fprintf(stderr, "Error: rebase target overflow after shift\n");
                            return 1;
                        }

                        *slot = (uint64_t)new_target |
                                (high8 << 36) |
                                (reserved << 44) |
                                (next << 51) |
                                ((uint64_t)is_bind << 63);
                    }

                    if (next == 0U) {
                        break;
                    }
                    chain_offs += next * 4U;
                }
            }
        }

        /* Update segment_offset for shifted segments. */
        if (seg_starts->segment_offset == data_const_seg_vmaddr_offs && data_const_seg_bytes) {
            seg_starts->segment_offset = data_const_seg_vmaddr_offs + vm_shift;
        }
        else if (seg_starts->segment_offset == data_seg_vmaddr_offs && data_seg_bytes) {
            seg_starts->segment_offset = data_seg_vmaddr_offs + vm_shift;
        }
    }

    /* seg_info_offset[] is indexed by LC segment index.  Inserting __UNPACK
     * at LC index 2 pushed __DATA_CONST / __DATA down by one slot, so the
     * blob's index array needs the same shift.  Drop the original final
     * entry (which must be 0 -- typically __LINKEDIT with no fixups) and
     * insert a 0 at index 2 for __UNPACK.  seg_count stays put.
     */
    if (image_starts->seg_count >= 3U) {
        uint32_t shuffle_idx;

        if (image_starts->seg_info_offset[image_starts->seg_count - 1U] != 0U) {
            fprintf(stderr, "Error: chained-fixups last entry non-zero - cannot reindex for __UNPACK\n");
            return 1;
        }
        for (shuffle_idx = image_starts->seg_count - 1U; shuffle_idx >= 3U; shuffle_idx--) {
            image_starts->seg_info_offset[shuffle_idx] = image_starts->seg_info_offset[shuffle_idx - 1U];
        }
        image_starts->seg_info_offset[2] = 0U;
    }

    return 0;
}

/* dyld scans every section's type byte for initializer and terminator hooks
 * (__mod_init_func, __mod_term_func, __init_offsets ...) and calls into the
 * addresses they encode.  Those sections now live inside the zero-fill gap of
 * our compressed __TEXT or reference shifted code pages that dyld cannot
 * recover until the loader runs, so we neutralize their type byte to S_REGULAR
 * to keep dyld from invoking garbage at startup.
 */
static void neutralize_init_term_sections(SEGMENT_COMMAND_64 *seg)
{
    SECTION_64 *sect = (SECTION_64 *)(seg + 1);
    uint32_t    sect_idx;

    for (sect_idx = 0; sect_idx < seg->nsects; sect_idx++) {
        const uint32_t sect_type = sect[sect_idx].flags & SECTION_TYPE_MASK;
        if (sect_type == S_MOD_INIT_FUNC_POINTERS ||
            sect_type == S_MOD_TERM_FUNC_POINTERS ||
            sect_type == S_INIT_FUNC_OFFSETS) {
            sect[sect_idx].flags &= ~SECTION_TYPE_MASK;
        }
    }
}

/* Zero every DYSYMTAB table reference.  The symbol tables are dropped from the
 * packed output (not needed to load or run it), so the command must describe
 * nothing.
 */
static void zero_dysymtab(DYSYMTAB_COMMAND *dst)
{
    dst->ilocalsym = 0;
    dst->nlocalsym = 0;
    dst->iextdefsym = 0;
    dst->nextdefsym = 0;
    dst->iundefsym = 0;
    dst->nundefsym = 0;
    dst->tocoff = 0;
    dst->ntoc = 0;
    dst->modtaboff = 0;
    dst->nmodtab = 0;
    dst->extrefsymoff = 0;
    dst->nextrefsyms = 0;
    dst->indirectsymoff = 0;
    dst->nindirectsyms = 0;
    dst->extreloff = 0;
    dst->nextrel = 0;
    dst->locreloff = 0;
    dst->nlocrel = 0;
}

BUFFER exe_macho(const void *buf, size_t size)
{
    BUFFER         empty            = { NULL, 0 };
    BUFFER         output           = { NULL, 0 };
    BUFFER         compress_scratch = { NULL, 0 };
    BUFFER         combined_raw     = { NULL, 0 };
    BUFFER         combined_payload = { NULL, 0 };
    MACHO_DATA_REBASE *data_rebases = NULL;
    LOADER_BLOB    loader           = { { NULL, 0 }, 0, 0, 0 };
    MACHO_INPUT    input;
    int            error = 1;

    /* Layout values */
    uint64_t       loader_low12;
    uint64_t       cmds_end_offs;
    uint64_t       loader_offs;
    uint64_t       payload_offs;
    size_t         arith_encoded_size;
    uint64_t       data_const_content_size;
    uint64_t       payload_gap;
    uint64_t       payload_data_const;
    uint64_t       payload_tail;
    int            data_eligible;
    int            data_folded = 0;
    uint32_t       data_rebase_count = 0;
    uint32_t       data_content_size = 0;
    size_t         combined_raw_size = 0;
    uint64_t       out_text_filesize;
    uint64_t       out_text_vmsize;
    uint64_t       out_text_payload_filesize;
    uint64_t       vm_shift;

    uint64_t       unpack_vmaddr;
    uint64_t       unpack_vmsize;
    uint64_t       scratch_vmaddr;
    uint64_t       scratch_vmsize;
    uint64_t       data_raw_scratch_offs;
    uint64_t       data_const_vmaddr;
    uint64_t       data_vmaddr;
    uint64_t       linkedit_vmaddr;
    uint64_t       data_const_fileoff;
    uint64_t       data_fileoff;
    uint64_t       linkedit_fileoff;
    uint64_t       linkedit_content_size;
    uint64_t       code_limit;
    uint32_t       sig_size;

    COMPRESSED_SIZES lz_sizes;
    MACHO_LIVE_LAYOUT layout;

    uint64_t       new_sizeofcmds;
    uint64_t       new_ncmds;
    uint64_t       extra_segments;

    const uint8_t *input_bytes = (const uint8_t *)buf;

    if (parse_macho_input(buf, size, &input)) {
        return empty;
    }

    if (load_loader(&loader, "macho_loader")) {
        goto cleanup;
    }

    loader_low12 = loader.text_vmaddr & (ARM64_ADRP_PAGE - 1U);

    extra_segments = 2U;
    new_sizeofcmds = (uint64_t)input.header->sizeofcmds + extra_segments * sizeof(SEGMENT_COMMAND_64);
    new_ncmds      = (uint64_t)input.header->ncmds + extra_segments;

    /* Place the loader at the first file offset >= (header + load_cmds)
     * whose low 12 bits match loader_low12, so its compile-time PC-relative
     * references into __layout/__const still resolve once relocated.
     */
    cmds_end_offs = sizeof(MACHO_HEADER_64) + new_sizeofcmds;
    loader_offs   = align_to_low12(cmds_end_offs, loader_low12);
    payload_offs  = loader_offs + loader.bytes.size;

    /* vm_shift is the original __TEXT.vmsize regardless of how small the
     * compressed payload turns out to be, so it is known up front and is
     * needed by the __DATA eligibility walk below.
     */
    out_text_vmsize = input.text_seg->vmsize;
    vm_shift        = out_text_vmsize;

    /* Decide whether __DATA can be folded (compressed + self-rebased) or must
     * be copied verbatim, and when foldable build its rebase table.  The
     * counting walk runs first, then a second walk fills the table; if the two
     * disagree we fall back to verbatim.  __DATA_CONST always stays verbatim
     * (dyld binds its GOT); pointer-free const already lives in __TEXT,__const.
     */
    data_eligible = ! macho_has_objc_or_swift(&input);
    if (data_eligible &&
        walk_data_chain(input_bytes, &input, vm_shift, NULL, &data_rebase_count, &data_eligible)) {
        goto cleanup;
    }
    if (data_eligible && input.data_seg && input.data_seg->filesize > 0U) {
        uint32_t recount = 0;
        int      recheck = 1;

        data_rebases = malloc((size_t)data_rebase_count * sizeof(MACHO_DATA_REBASE));
        if (data_rebase_count && ! data_rebases) {
            fprintf(stderr, "Error: OOM data rebase table\n");
            goto cleanup;
        }
        if (walk_data_chain(input_bytes, &input, vm_shift, data_rebases, &recount, &recheck) ||
            ! recheck || recount != data_rebase_count) {
            data_eligible = 0;
            free(data_rebases);
            data_rebases = NULL;
        }
        else {
            data_content_size = (uint32_t)input.data_seg->filesize;
            data_folded       = 1;
        }
    }

    /* Build one contiguous raw range [__TEXT][rebase table][__data] and
     * compress it with a single LZ77 pass feeding a single arithmetic stream,
     * so the arith model stays warm across the segment boundary and LZ77 can
     * match __DATA bytes against __TEXT.  The whole __TEXT.filesize is
     * compressed (the Mach-O header at offset 0 is dead at runtime but
     * compresses to almost nothing).  __DATA bytes follow __TEXT; the loader
     * splits them back out at decomp_size.  When __DATA is not folded the
     * range is just __TEXT.
     */
    {
        const uint32_t rebase_bytes = data_folded
                                          ? data_rebase_count * (uint32_t)sizeof(MACHO_DATA_REBASE)
                                          : 0U;
        const size_t   text_size    = input.text_seg->filesize;

        combined_raw_size = text_size + rebase_bytes + data_content_size;
        combined_raw      = buf_alloc(combined_raw_size);
        if ( ! combined_raw.buf) {
            fprintf(stderr, "Error: OOM combined raw\n");
            goto cleanup;
        }
        memcpy(combined_raw.buf, input_bytes, text_size);
        if (data_folded) {
            memcpy((uint8_t *)combined_raw.buf + text_size, data_rebases, rebase_bytes);
            memcpy((uint8_t *)combined_raw.buf + text_size + rebase_bytes,
                   input_bytes + input.data_seg->fileoff, data_content_size);
        }
    }

    compress_scratch = buf_alloc(estimate_compress_size(combined_raw_size) * 2U + 4096U);
    if ( ! compress_scratch.buf) {
        fprintf(stderr, "Error: OOM compress scratch\n");
        goto cleanup;
    }

    {
        const size_t lz_dest_capacity = compress_scratch.size / 2U;
        uint8_t     *lz_dest          = compress_scratch.buf;
        uint8_t     *arith_dest       = compress_scratch.buf + lz_dest_capacity;
        const size_t arith_dest_cap   = compress_scratch.size - lz_dest_capacity;

        lz_sizes = lz_compress(lz_dest, lz_dest_capacity,
                               combined_raw.buf, combined_raw_size);
        if ( ! lz_sizes.lz) {
            fprintf(stderr, "Error: LZ77 compression failed\n");
            goto cleanup;
        }

        arith_encoded_size = arith_encode(arith_dest, arith_dest_cap, lz_dest, lz_sizes.lz);
        if ( ! arith_encoded_size || arith_encoded_size > arith_dest_cap) {
            fprintf(stderr, "Error: arith encoding failed\n");
            goto cleanup;
        }
    }

    /* Distribute the combined arith payload across the free file space so no
     * bytes are wasted.  Fill the space that does not grow the file first --
     * the alignment gap before the loader and the __DATA_CONST page
     * padding (that page is mandatory anyway; dyld needs it writable to bind
     * the GOT) -- then put the remainder in the __TEXT tail, which is the only
     * region that extends __TEXT.  None of these are contiguous in VM, so the
     * arithmetic decoder reads them as a list of ranges and concatenates them.
     */
    data_const_content_size = input.data_const_seg ? segment_content_size(input.data_const_seg) : 0;
    {
        const uint64_t gap_cap = loader_offs - cmds_end_offs;
        const uint64_t dc_cap   = (input.data_const_seg &&
                                   input.data_const_seg->filesize > data_const_content_size)
                                      ? input.data_const_seg->filesize - data_const_content_size
                                      : 0;
        /* Free __TEXT space past the loader up to the current page boundary;
         * filling it does not add a page. */
        const uint64_t tail_free = align_up_u64(payload_offs, MACOS_ARM64_PAGE) - payload_offs;
        uint64_t remaining = arith_encoded_size;

        payload_gap = remaining < gap_cap ? remaining : gap_cap;
        remaining   -= payload_gap;

        /* Finish filling __TEXT's page before spilling into __DATA_CONST, so
         * the single leftover gap lands at the very end of the file (in
         * __DATA_CONST).  Only when the payload exceeds all of that does the
         * __TEXT tail grow past the page boundary, with __DATA_CONST full.
         */
        if (remaining <= tail_free + dc_cap) {
            payload_tail       = remaining < tail_free ? remaining : tail_free;
            payload_data_const = remaining - payload_tail;
        }
        else {
            payload_data_const = dc_cap;
            payload_tail       = remaining - dc_cap;
        }
    }

    /* __TEXT.vmsize must stay at the original value so the section table
     * inside __TEXT continues to describe sections within the segment bounds
     * (codesign requires this).  __TEXT.filesize only needs to cover the
     * loader plus the __TEXT-tail payload chunk; the rest is zero-fill.
     */
    out_text_payload_filesize = payload_offs + payload_tail;
    out_text_filesize         = align_up_u64(out_text_payload_filesize, MACOS_ARM64_PAGE);
    if (out_text_filesize < MACOS_ARM64_PAGE) {
        out_text_filesize = MACOS_ARM64_PAGE;
    }
    if (out_text_filesize > out_text_vmsize) {
        fprintf(stderr,
                "Error: compressed payload (%llu) exceeds original __TEXT.vmsize (%llu)\n",
                (unsigned long long)out_text_filesize,
                (unsigned long long)out_text_vmsize);
        goto cleanup;
    }

    unpack_vmaddr  = input.image_base + out_text_vmsize;
    unpack_vmsize  = input.text_seg->vmsize;

    /* __SCRATCH is placed AFTER __LINKEDIT (see below) so its vmaddr does not
     * disturb the uniform vm_shift applied to __DATA_CONST / __DATA /
     * __LINKEDIT.  It is zero-fill, so enlarging it costs no file bytes.
     *
     * Sub-regions, each 16-byte aligned, in order:
     *   1. combined LZ output at offset 0 (lz77_data_offs), size lz_sizes.lz.
     *   2. arith gather buffer at align16(lz_sizes.lz); the arithmetic decoder derives
     *      its location the same way, so reserve align16(arith_encoded_size) for it.
     *   3. combined raw blob (data_raw_offs), size combined_raw_size; the LZ
     *      decompressor unpacks into it and splits [__TEXT][rebases][__data].
     */
    {
        const uint64_t region2_offs = align_up_u64(lz_sizes.lz, 16U);
        const uint64_t region3_offs = region2_offs + align_up_u64(arith_encoded_size, 16U);
        uint64_t       scratch_end;

        data_raw_scratch_offs = region3_offs;
        scratch_end           = data_raw_scratch_offs + combined_raw_size;
        scratch_vmsize        = align_up_u64(scratch_end + ARM64_ADRP_PAGE, MACOS_ARM64_PAGE);
    }

    data_const_vmaddr  = input.data_const_seg ? input.data_const_seg->vmaddr + vm_shift : 0;
    data_vmaddr        = input.data_seg       ? input.data_seg->vmaddr       + vm_shift : 0;
    linkedit_vmaddr    = input.linkedit_seg->vmaddr + vm_shift;

    data_const_fileoff = align_up_u64(out_text_filesize, MACOS_ARM64_PAGE);
    data_fileoff       = data_const_fileoff + (input.data_const_seg ? input.data_const_seg->filesize : 0U);
    data_fileoff       = align_up_u64(data_fileoff, MACOS_ARM64_PAGE);
    /* When __DATA is folded it contributes no file bytes (declared zero-fill),
     * so __LINKEDIT moves earlier and the output file shrinks.
     */
    linkedit_fileoff   = data_fileoff +
                         ((input.data_seg && ! data_folded) ? input.data_seg->filesize : 0U);
    linkedit_fileoff   = align_up_u64(linkedit_fileoff, MACOS_ARM64_PAGE);
    scratch_vmaddr     = linkedit_vmaddr + input.linkedit_seg->vmsize + 0x100000U;
    scratch_vmaddr     = align_up_u64(scratch_vmaddr, MACOS_ARM64_PAGE);

    /* Trim __LINKEDIT to only the chained fixups dyld needs to bind the GOT.
     * The symbol/string tables, exports trie, function starts and
     * data-in-code are not needed to load and run a packed executable, so we
     * drop them (their load commands are emptied above).  Then build a fresh
     * ad-hoc signature ourselves rather than shelling out to codesign (which
     * over-pads heavily and only exists on macOS).  code_limit -- the extent
     * the CodeDirectory hashes -- is where the new signature starts.
     */
    linkedit_content_size = input.chained_fixups_cmd ? input.chained_fixups_cmd->datasize : 0;
    code_limit            = align_up_u64(linkedit_fileoff + linkedit_content_size, 16U);
    sig_size              = macho_adhoc_sig_size(code_limit, "minify");

    /* Fill the loader layout.  All pointer fields are offsets from the
     * runtime image base, with the loader computing the runtime base from
     * the layout's own address minus layout_image_offs.  The single
     * loader gathers the combined payload from its file ranges into __SCRATCH,
     * arith-decodes it into the LZ77 stream, LZ-decompresses into __UNPACK
     * (splitting __DATA back out when folded), mprotects __UNPACK to RX and
     * jumps to the original entry point.
     */
    memset(&layout, 0, sizeof(layout));
    {
        const uint64_t layout_vmaddr = input.image_base + loader_offs + loader.layout_offs;
        uint32_t       range_count = 0;

        layout.layout_image_offs = layout_vmaddr - input.image_base;
        layout.decomp_base_offs  = unpack_vmaddr - input.image_base;
        layout.lz77_data_offs    = scratch_vmaddr - input.image_base;
        layout.entry_point_offs  = vm_shift + input.entry_point->entryoff;
        layout.decomp_size       = (uint32_t)unpack_vmsize;
        layout.lz77_data_size    = (uint32_t)lz_sizes.lz;
        layout.data_raw_offs     = data_folded ? scratch_vmaddr - input.image_base + data_raw_scratch_offs : 0;
        layout.data_dest_offs    = data_folded ? data_vmaddr - input.image_base : 0;
        layout.data_content_size = data_content_size;
        layout.data_rebase_count = data_folded ? data_rebase_count : 0U;

        /* Payload ranges, in the same order they are written below.  __TEXT
         * offsets equal image offsets (__TEXT maps at the image base from
         * fileoff 0); the __DATA_CONST range uses its segment-relative image
         * offset, which differs from its file offset.
         */
        if (payload_gap) {
            layout.payload_range_offs[range_count] = cmds_end_offs;
            layout.payload_range_size[range_count] = (uint32_t)payload_gap;
            range_count++;
        }
        if (payload_tail) {
            layout.payload_range_offs[range_count] = payload_offs;
            layout.payload_range_size[range_count] = (uint32_t)payload_tail;
            range_count++;
        }
        if (payload_data_const) {
            layout.payload_range_offs[range_count] = (data_const_vmaddr - input.image_base) + data_const_content_size;
            layout.payload_range_size[range_count] = (uint32_t)payload_data_const;
            range_count++;
        }
        layout.payload_range_count = range_count;
    }

    memcpy(loader.bytes.buf + loader.layout_offs, &layout, sizeof(layout));

    /* Allocate output buffer and write everything. */
    {
        const uint64_t out_size = code_limit + sig_size;
        output = buf_alloc(out_size);
        if ( ! output.buf) {
            fprintf(stderr, "Error: OOM output buffer\n");
            goto cleanup;
        }
        memset(output.buf, 0, out_size);
    }

    /* Mach-O header and load commands. */
    memcpy(output.buf, buf, sizeof(MACHO_HEADER_64));

    {
        MACHO_HEADER_64    *hdr            = (MACHO_HEADER_64 *)output.buf;
        const uint8_t      *src_cursor     = input_bytes + sizeof(MACHO_HEADER_64);
        const uint8_t      *src_end        = src_cursor + input.header->sizeofcmds;
        uint8_t            *dst_cursor     = output.buf + sizeof(MACHO_HEADER_64);
        uint8_t            *unpack_slot    = NULL;
        uint8_t            *scratch_slot   = NULL;
        const LOAD_COMMAND *src_cmd;
        uint8_t            *cmds_dst_end;

        /* dyld requires LC_SEGMENT_64 entries to appear in ascending
         * vmaddr order.  __UNPACK is at vmaddr right after __TEXT (so
         * insert immediately after the __TEXT segment command).
         * __SCRATCH is at a vmaddr past __LINKEDIT (so insert immediately
         * after the __LINKEDIT segment command).
         */
        while (src_cursor < src_end) {
            src_cmd = (const LOAD_COMMAND *)src_cursor;
            memcpy(dst_cursor, src_cursor, src_cmd->cmdsize);

            if (src_cmd->cmd == LC_SEGMENT_64) {
                const SEGMENT_COMMAND_64 *src_seg = (const SEGMENT_COMMAND_64 *)src_cursor;
                if (seg_name_equals(src_seg, "__TEXT") && ! unpack_slot) {
                    dst_cursor += src_cmd->cmdsize;
                    unpack_slot = dst_cursor;
                    dst_cursor += sizeof(SEGMENT_COMMAND_64);
                    src_cursor += src_cmd->cmdsize;
                    continue;
                }
                if (seg_name_equals(src_seg, "__LINKEDIT") && ! scratch_slot) {
                    dst_cursor += src_cmd->cmdsize;
                    scratch_slot = dst_cursor;
                    dst_cursor += sizeof(SEGMENT_COMMAND_64);
                    src_cursor += src_cmd->cmdsize;
                    continue;
                }
            }
            dst_cursor += src_cmd->cmdsize;
            src_cursor += src_cmd->cmdsize;
        }

        if ( ! unpack_slot || ! scratch_slot) {
            fprintf(stderr, "Error: could not find __TEXT/__LINKEDIT slots\n");
            goto cleanup;
        }
        cmds_dst_end = dst_cursor;

        /* Walk the copied commands and patch fields. */
        dst_cursor = output.buf + sizeof(MACHO_HEADER_64);
        while (dst_cursor < cmds_dst_end) {
            LOAD_COMMAND *load_cmd = (LOAD_COMMAND *)dst_cursor;

            if (dst_cursor == unpack_slot || dst_cursor == scratch_slot) {
                dst_cursor += sizeof(SEGMENT_COMMAND_64);
                continue;
            }

            switch (load_cmd->cmd) {

                case LC_SEGMENT_64: {
                    SEGMENT_COMMAND_64 *seg = (SEGMENT_COMMAND_64 *)dst_cursor;
                    int                 shift_sections = 0;

                    neutralize_init_term_sections(seg);

                    if (seg_name_equals(seg, "__TEXT")) {
                        seg->vmsize   = out_text_vmsize;
                        seg->filesize = out_text_filesize;
                    }
                    else if (seg_name_equals(seg, "__DATA_CONST")) {
                        seg->vmaddr  = data_const_vmaddr;
                        seg->fileoff = data_const_fileoff;
                        shift_sections = 1;
                    }
                    else if (seg_name_equals(seg, "__DATA")) {
                        seg->vmaddr  = data_vmaddr;
                        if (data_folded) {
                            /* Folded: no file backing, the loader reconstructs
                             * it into anonymous zero-fill RW memory at runtime.
                             */
                            seg->fileoff  = 0;
                            seg->filesize = 0;
                        }
                        else {
                            seg->fileoff = data_fileoff;
                        }
                        shift_sections = 1;
                    }
                    else if (seg_name_equals(seg, "__LINKEDIT")) {
                        seg->vmaddr   = linkedit_vmaddr;
                        seg->fileoff  = linkedit_fileoff;
                        seg->filesize = (code_limit - linkedit_fileoff) + sig_size;
                        seg->vmsize   = align_up_u64(seg->filesize, MACOS_ARM64_PAGE);
                    }

                    if (shift_sections) {
                        /* Each SECTION_64 entry within this segment has
                         * an `addr` field that must also be shifted, and
                         * `offset` (file offset) that must be adjusted by the
                         * segment fileoff delta.  A folded __DATA has no file
                         * backing, so its section offsets become 0 (zero-fill).
                         */
                        const int      is_folded_data = data_folded && seg_name_equals(seg, "__DATA");
                        const int64_t  fileoff_delta  = seg_name_equals(seg, "__DATA_CONST")
                                                          ? (int64_t)data_const_fileoff - (int64_t)input.data_const_seg->fileoff
                                                          : (int64_t)data_fileoff - (int64_t)input.data_seg->fileoff;
                        SECTION_64    *sect           = (SECTION_64 *)(seg + 1);
                        uint32_t       j;

                        for (j = 0; j < seg->nsects; j++) {
                            sect[j].addr += vm_shift;
                            if (is_folded_data) {
                                sect[j].offset = 0;
                            }
                            else if (sect[j].offset) {
                                sect[j].offset = (uint32_t)((int64_t)sect[j].offset + fileoff_delta);
                            }
                        }
                    }
                    break;
                }

                case LC_MAIN: {
                    ENTRY_POINT_COMMAND *epc = (ENTRY_POINT_COMMAND *)dst_cursor;
                    epc->entryoff = loader_offs + loader.entry_offs;
                    break;
                }

                case LC_SYMTAB: {
                    /* Trimmed away: not needed to load or run a packed exe. */
                    SYMTAB_COMMAND *st = (SYMTAB_COMMAND *)dst_cursor;
                    st->symoff  = 0;
                    st->nsyms   = 0;
                    st->stroff  = 0;
                    st->strsize = 0;
                    break;
                }

                case LC_DYSYMTAB:
                    /* Trimmed away: not needed to load or run a packed exe. */
                    zero_dysymtab((DYSYMTAB_COMMAND *)dst_cursor);
                    break;

                case LC_DYLD_CHAINED_FIXUPS: {
                    /* Kept (dyld binds the GOT from it); relocated to the start
                     * of the trimmed __LINKEDIT.
                     */
                    LINKEDIT_DATA_COMMAND *led = (LINKEDIT_DATA_COMMAND *)dst_cursor;
                    led->dataoff = (uint32_t)linkedit_fileoff;
                    break;
                }

                case LC_DYLD_EXPORTS_TRIE:
                case LC_FUNCTION_STARTS:
                case LC_DATA_IN_CODE: {
                    /* Trimmed away. */
                    LINKEDIT_DATA_COMMAND *led = (LINKEDIT_DATA_COMMAND *)dst_cursor;
                    led->dataoff  = 0;
                    led->datasize = 0;
                    break;
                }

                case LC_CODE_SIGNATURE: {
                    /* Point at the fresh signature we build below, not the
                     * input's (which we dropped).
                     */
                    LINKEDIT_DATA_COMMAND *led = (LINKEDIT_DATA_COMMAND *)dst_cursor;
                    led->dataoff  = (uint32_t)code_limit;
                    led->datasize = sig_size;
                    break;
                }

                default:
                    break;
            }
            dst_cursor += load_cmd->cmdsize;
        }

        {
            SEGMENT_COMMAND_64 *unpack_seg  = (SEGMENT_COMMAND_64 *)unpack_slot;
            SEGMENT_COMMAND_64 *scratch_seg = (SEGMENT_COMMAND_64 *)scratch_slot;

            memset(unpack_seg, 0, sizeof(*unpack_seg));
            unpack_seg->cmd      = LC_SEGMENT_64;
            unpack_seg->cmdsize  = sizeof(SEGMENT_COMMAND_64);
            memcpy(unpack_seg->segname, "__UNPACK", 8);
            unpack_seg->vmaddr   = unpack_vmaddr;
            unpack_seg->vmsize   = unpack_vmsize;
            unpack_seg->fileoff  = 0;
            unpack_seg->filesize = 0;
            unpack_seg->maxprot  = VM_PROT_RW;
            unpack_seg->initprot = VM_PROT_RW;

            memset(scratch_seg, 0, sizeof(*scratch_seg));
            scratch_seg->cmd      = LC_SEGMENT_64;
            scratch_seg->cmdsize  = sizeof(SEGMENT_COMMAND_64);
            memcpy(scratch_seg->segname, "__SCRATCH", 9);
            scratch_seg->vmaddr   = scratch_vmaddr;
            scratch_seg->vmsize   = scratch_vmsize;
            scratch_seg->fileoff  = 0;
            scratch_seg->filesize = 0;
            scratch_seg->maxprot  = VM_PROT_RW;
            scratch_seg->initprot = VM_PROT_RW;
        }

        hdr->ncmds      = (uint32_t)new_ncmds;
        hdr->sizeofcmds = (uint32_t)new_sizeofcmds;
    }

    /* Copy the single arith stream out of the scratch buffer into its own
     * buffer so it can be sliced across the file ranges below.
     */
    combined_payload = buf_alloc(arith_encoded_size);
    if ( ! combined_payload.buf) {
        fprintf(stderr, "Error: OOM combined payload\n");
        goto cleanup;
    }
    memcpy(combined_payload.buf,
           compress_scratch.buf + compress_scratch.size / 2U,
           arith_encoded_size);

    /* Write the loader into __TEXT. */
    memcpy(output.buf + loader_offs, loader.bytes.buf, loader.bytes.size);

    /* Copy __DATA_CONST verbatim first; its padding is overwritten by the
     * payload below.
     */
    if (input.data_const_seg && input.data_const_seg->filesize > 0U) {
        memcpy(output.buf + data_const_fileoff,
               input_bytes + input.data_const_seg->fileoff,
               input.data_const_seg->filesize);
    }

    /* Distribute the combined payload into its file ranges, in the same order
     * recorded in the layout: the loader alignment gap, the __TEXT tail, then
     * the __DATA_CONST padding.  Zero-length copies are no-ops.
     */
    {
        const uint8_t *payload_cursor = (const uint8_t *)combined_payload.buf;

        memcpy(output.buf + cmds_end_offs, payload_cursor, payload_gap);
        payload_cursor += payload_gap;
        memcpy(output.buf + payload_offs, payload_cursor, payload_tail);
        payload_cursor += payload_tail;
        memcpy(output.buf + data_const_fileoff + data_const_content_size, payload_cursor, payload_data_const);
    }

    /* __DATA is copied verbatim only when not folded into the payload. */
    if (input.data_seg && input.data_seg->filesize > 0U && ! data_folded) {
        memcpy(output.buf + data_fileoff,
               input_bytes + input.data_seg->fileoff,
               input.data_seg->filesize);
    }

    /* Copy the chained fixups (the only __LINKEDIT content we keep) to the
     * start of the trimmed __LINKEDIT, then shift their rebase targets.
     */
    if (linkedit_content_size > 0U) {
        memcpy(output.buf + linkedit_fileoff,
               input_bytes + input.chained_fixups_cmd->dataoff,
               linkedit_content_size);
    }

    if (input.chained_fixups_cmd) {
        const uint64_t fixups_dataoff_new = linkedit_fileoff;
        uint8_t *data_const_bytes           = input.data_const_seg && input.data_const_seg->filesize
                                                  ? output.buf + data_const_fileoff
                                                  : NULL;
        uint8_t *data_bytes                 = (input.data_seg && input.data_seg->filesize && ! data_folded)
                                                  ? output.buf + data_fileoff
                                                  : NULL;
        uint64_t data_const_seg_vmaddr_offs = input.data_const_seg
                                                  ? input.data_const_seg->vmaddr - input.image_base
                                                  : 0;
        uint64_t data_const_seg_size        = input.data_const_seg ? input.data_const_seg->filesize : 0;
        uint64_t data_seg_vmaddr_offs       = input.data_seg
                                                  ? input.data_seg->vmaddr - input.image_base
                                                  : 0;
        uint64_t data_seg_size              = input.data_seg ? input.data_seg->filesize : 0;

        if (shift_chained_fixups(output.buf + linkedit_fileoff,
                                 linkedit_fileoff,
                                 (uint32_t)fixups_dataoff_new,
                                 input.chained_fixups_cmd->datasize,
                                 data_const_bytes, data_const_seg_vmaddr_offs, data_const_seg_size,
                                 data_bytes, data_seg_vmaddr_offs, data_seg_size,
                                 vm_shift, data_folded)) {
            goto cleanup;
        }
    }

    /* Everything (including the final load commands and __LINKEDIT sizes) is
     * in place; hash [0, code_limit) and write the ad-hoc signature.
     */
    macho_adhoc_sign(output.buf, code_limit, out_text_filesize, "minify");

    printf("Mach-O compression:\n");
    printf("        original __TEXT          %llu bytes\n", (unsigned long long)input.text_seg->filesize);
    if (data_folded) {
        printf("        folded __DATA            %u bytes (+%u rebase table, %u rebases)\n",
               data_content_size,
               data_rebase_count * (uint32_t)sizeof(MACHO_DATA_REBASE),
               data_rebase_count);
    }
    else {
        printf("        __DATA                   verbatim (not folded)\n");
    }
    printf("        combined raw             %zu bytes\n", combined_raw_size);
    printf("        LZ77 compressed          %zu bytes\n", lz_sizes.lz);
    printf("        arith encoded            %zu bytes\n", arith_encoded_size);
    printf("        out __TEXT.filesize      %llu bytes (vmsize %llu)\n", (unsigned long long)out_text_filesize,
                                                                          (unsigned long long)out_text_vmsize);
    printf("        signature                %u bytes\n", sig_size);

    printf("Compression stats:\n");
    printf("        LIT                      %zu\n", lz_sizes.stats_lit);
    printf("        MATCH                    %zu\n", lz_sizes.stats_match);
    printf("        SHORTREP                 %zu\n", lz_sizes.stats_shortrep);
    printf("        LONGREP0                 %zu\n", lz_sizes.stats_longrep[0]);
    printf("        LONGREP1                 %zu\n", lz_sizes.stats_longrep[1]);
    printf("        LONGREP2                 %zu\n", lz_sizes.stats_longrep[2]);
    printf("        LONGREP3                 %zu\n", lz_sizes.stats_longrep[3]);

    printf("Process virtual address space layout:\n");
    printf("        image base               0x%llx\n", (unsigned long long)input.image_base);
    printf("        vm shift                 0x%llx\n", (unsigned long long)vm_shift);
    printf("        __UNPACK vmaddr          0x%llx\n", (unsigned long long)unpack_vmaddr);
    printf("        __SCRATCH vmaddr         0x%llx\n", (unsigned long long)scratch_vmaddr);
    printf("        loader offset            0x%llx\n", (unsigned long long)loader_offs);
    printf("        __DATA_CONST fileoff     0x%llx\n", (unsigned long long)data_const_fileoff);
    printf("        __LINKEDIT fileoff       0x%llx\n", (unsigned long long)linkedit_fileoff);
    printf("        signature fileoff        0x%llx\n", (unsigned long long)code_limit);

    error = 0;

cleanup:
    free(loader.bytes.buf);
    free(compress_scratch.buf);
    free(combined_raw.buf);
    free(combined_payload.buf);
    free(data_rebases);

    if (error) {
        free(output.buf);
        output.buf = NULL;
        output.size = 0;
    }
    return output;
}
