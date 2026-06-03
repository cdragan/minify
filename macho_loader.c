/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#include "lza_decompress.h"
#include "macho_common.h"

#include <stddef.h>
#include <stdint.h>

__attribute__((used, section("__TEXT,__layout")))
MACHO_LIVE_LAYOUT macho_live_layout;

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4

#define MAP_PRIVATE  0x0002
#define MAP_FIXED    0x0010
#define MAP_ANON     0x1000

/* macOS arm64 disallows MRS CTR_EL0 in EL0, so we hardcode the arm64
 * architectural minimum line size (16 bytes).  This means more
 * invalidations than strictly needed on chips with 64- or 128-byte
 * lines, but it is always correct.
 */
#define ARM64_CACHE_LINE_SIZE 16U

static void invalidate_icache(uint8_t *base, uint64_t size)
{
    const uintptr_t start = (uintptr_t)base;
    const uintptr_t end   = start + size;
    uintptr_t       addr;

    addr = start & ~((uintptr_t)ARM64_CACHE_LINE_SIZE - 1U);
    while (addr < end) {
        __asm__ volatile("dc cvau, %0" : : "r"(addr) : "memory");
        addr += ARM64_CACHE_LINE_SIZE;
    }
    __asm__ volatile("dsb ish" : : : "memory");

    addr = start & ~((uintptr_t)ARM64_CACHE_LINE_SIZE - 1U);
    while (addr < end) {
        __asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");
        addr += ARM64_CACHE_LINE_SIZE;
    }
    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
}

/* macOS arm64 BSD syscall: x16 = syscall number, x0-x5 = args, svc #0x80.
 * Returns 0 (or mapped address) on success, -errno on failure.  The kernel
 * clobbers x1-x15 across the call, so they are listed in the clobber set.
 */
#define SYS_MPROTECT 74
#define SYS_MMAP     197

static void *sys_mmap(void *addr, uint64_t len, int prot, int flags, int fd, int64_t offs)
{
    register long x0  __asm__("x0")  = (long)(uintptr_t)addr;
    register long x1  __asm__("x1")  = (long)len;
    register long x2  __asm__("x2")  = (long)prot;
    register long x3  __asm__("x3")  = (long)flags;
    register long x4  __asm__("x4")  = (long)fd;
    register long x5  __asm__("x5")  = (long)offs;
    register long x16 __asm__("x16") = SYS_MMAP;
    register long ret __asm__("x0");

    __asm__ volatile("svc #0x80"
                     : "=r"(ret), "+r"(x1), "+r"(x2), "+r"(x3),
                       "+r"(x4), "+r"(x5), "+r"(x16)
                     : "0"(x0)
                     : "memory", "cc",
                       "x6", "x7", "x8", "x9", "x10",
                       "x11", "x12", "x13", "x14", "x15");
    return (void *)(uintptr_t)ret;
}

static int64_t sys_mprotect(void *addr, uint64_t len, int prot)
{
    register long x0  __asm__("x0")  = (long)(uintptr_t)addr;
    register long x1  __asm__("x1")  = (long)len;
    register long x2  __asm__("x2")  = (long)prot;
    register long x16 __asm__("x16") = SYS_MPROTECT;
    register long ret __asm__("x0");

    __asm__ volatile("svc #0x80"
                     : "=r"(ret), "+r"(x1), "+r"(x2), "+r"(x16)
                     : "0"(x0)
                     : "memory", "cc",
                       "x3", "x4", "x5", "x6", "x7", "x8",
                       "x9", "x10", "x11", "x12", "x13", "x14", "x15");
    return ret;
}

int loader(int argc, char **argv, char **envp, char **apple)
{
    const uintptr_t   image_base        = macho_live_image_base(&macho_live_layout);
    const uint32_t    range_count       = macho_live_layout.payload_range_count;
    uint8_t          *gather            = (uint8_t *)(image_base + macho_live_layout.gather_offs);
    uint8_t          *decomp_base       = (uint8_t *)(image_base + macho_live_layout.decomp_base_offs);
    const uint32_t    decomp_size       = macho_live_layout.decomp_size;
    const uint32_t    data_content_size = macho_live_layout.data_content_size;
    const uint32_t    data_rebase_count = macho_live_layout.data_rebase_count;
    MACHO_ENTRY_POINT entry_point       = (MACHO_ENTRY_POINT)(image_base + macho_live_layout.entry_point_offs);
    uint32_t          payload_size      = 0;
    uint32_t          range_idx;

    /* The compressed payload is stored in disjoint file ranges:
     * - the alignment gap before the loader,
     * - the __TEXT tail,
     * - the __DATA_CONST padding
     * which are not contiguous in VM, so reassemble it into the __SCRATCH
     * gather buffer before decoding.
     */
    for (range_idx = 0; range_idx < range_count; range_idx++) {
        const uint8_t *piece      = (const uint8_t *)(image_base + macho_live_layout.payload_range_offs[range_idx]);
        const uint32_t piece_size = macho_live_layout.payload_range_size[range_idx];
        uint32_t       copy_idx;

        for (copy_idx = 0; copy_idx < piece_size; copy_idx++) {
            gather[payload_size + copy_idx] = piece[copy_idx];
        }
        payload_size += piece_size;
    }

    /* Decompress into __UNPACK.  AMFI forbids a one-shot mmap
     * with both PROT_WRITE and PROT_EXEC, so map the region RW (anonymous,
     * MAP_FIXED to keep the baked-in vmaddr), decompress, then mprotect up to
     * RX before jumping in.
     */
    sys_mmap(decomp_base, decomp_size,
             PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_PRIVATE | MAP_ANON,
             -1, 0);

    /* __TEXT and (when folded) __DATA share one LZ77 stream that decompresses
     * to [__TEXT (decomp_size)][rebase table][__data].  With no __DATA the
     * stream is just __TEXT, so decompress it straight into __UNPACK.  When
     * __DATA is folded the combined output exceeds __UNPACK, so decompress
     * into the __SCRATCH raw buffer, then split: copy __TEXT into __UNPACK,
     * copy __data to the __DATA base, and self-apply the rebases dyld was
     * prevented from processing (dyld already mapped __DATA as RW zero-fill).
     */
    if (data_content_size) {
        uint8_t                 *data_raw     = (uint8_t *)(image_base + macho_live_layout.data_raw_offs);
        const uint32_t           rebase_bytes = data_rebase_count * (uint32_t)sizeof(MACHO_DATA_REBASE);
        uint8_t                 *data_dest    = (uint8_t *)(image_base + macho_live_layout.data_dest_offs);
        const uint8_t           *data_src;
        const MACHO_DATA_REBASE *rebases;
        uint32_t                 copy_idx;
        uint32_t                 rebase_idx;

        lza_decompress(data_raw, (size_t)decomp_size + rebase_bytes + data_content_size,
                       gather, payload_size);

        for (copy_idx = 0; copy_idx < decomp_size; copy_idx++) {
            decomp_base[copy_idx] = data_raw[copy_idx];
        }

        data_src = data_raw + decomp_size + rebase_bytes;
        for (copy_idx = 0; copy_idx < data_content_size; copy_idx++) {
            data_dest[copy_idx] = data_src[copy_idx];
        }

        rebases = (const MACHO_DATA_REBASE *)(data_raw + decomp_size);
        for (rebase_idx = 0; rebase_idx < data_rebase_count; rebase_idx++) {
            *(uintptr_t *)(image_base + rebases[rebase_idx].slot_offs) =
                image_base + rebases[rebase_idx].target_offs;
        }
    }
    else {
        lza_decompress(decomp_base, decomp_size, gather, payload_size);
    }

    invalidate_icache(decomp_base, decomp_size);

    sys_mprotect(decomp_base, decomp_size, PROT_READ | PROT_EXEC);

    return entry_point(argc, argv, envp, apple);
}
