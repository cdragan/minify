/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 *
 * Minimal ad-hoc Mach-O code signature, equivalent to what the linker emits
 * (a CodeDirectory of page hashes plus empty Requirements and CMS blobs) but
 * sized exactly.  Self-contained -- no dependency on /usr/bin/codesign -- so
 * the compressor can sign its output on any host.  All code-signature blobs
 * are big-endian on disk regardless of the host's byte order.
 */

#include "macho_sign.h"
#include "sha256.h"

#include <string.h>

/* Code signing magic numbers and slot indices (see Apple's cs_blobs.h). */
#define CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0U
#define CSMAGIC_CODEDIRECTORY      0xfade0c02U
#define CSMAGIC_REQUIREMENTS       0xfade0c01U
#define CSMAGIC_BLOBWRAPPER        0xfade0b01U

#define CSSLOT_CODEDIRECTORY       0U
#define CSSLOT_REQUIREMENTS        2U
#define CSSLOT_SIGNATURESLOT       0x10000U

#define CS_ADHOC                   0x0002U
#define CS_HASHTYPE_SHA256         2U
#define CS_EXECSEG_MAIN_BINARY     1U

#define CD_VERSION                 0x20400U
#define CD_HEADER_SIZE             84U     /* through execSegFlags */
#define CD_NUM_SPECIAL_SLOTS       2U      /* InfoPlist (-1), Requirements (-2) */

/* macOS arm64 hashes in 16 KB code pages (matches what codesign produces and
 * AMFI accepts on this platform).
 */
#define CS_PAGE_SIZE_LOG2          14U
#define CS_PAGE_SIZE               (1U << CS_PAGE_SIZE_LOG2)

#define REQUIREMENTS_BLOB_SIZE     12U     /* empty requirements set */
#define CMS_BLOB_SIZE              8U      /* empty wrapper */
#define SUPERBLOB_HEADER_SIZE      12U
#define BLOB_INDEX_SIZE            8U
#define SUPERBLOB_NUM_BLOBS        3U

static void store_be32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)(value >> 24);
    dest[1] = (uint8_t)(value >> 16);
    dest[2] = (uint8_t)(value >>  8);
    dest[3] = (uint8_t)(value);
}

static void store_be64(uint8_t *dest, uint64_t value)
{
    store_be32(dest,     (uint32_t)(value >> 32));
    store_be32(dest + 4, (uint32_t)(value));
}

static uint32_t code_slot_count(uint64_t code_limit)
{
    return (uint32_t)((code_limit + CS_PAGE_SIZE - 1U) / CS_PAGE_SIZE);
}

static uint32_t code_directory_size(uint64_t code_limit, const char *identifier)
{
    const uint32_t ident_bytes = (uint32_t)strlen(identifier) + 1U;
    const uint32_t total_slots = CD_NUM_SPECIAL_SLOTS + code_slot_count(code_limit);

    return CD_HEADER_SIZE + ident_bytes + total_slots * SHA256_DIGEST_SIZE;
}

uint32_t macho_adhoc_sig_size(uint64_t code_limit, const char *identifier)
{
    return SUPERBLOB_HEADER_SIZE + SUPERBLOB_NUM_BLOBS * BLOB_INDEX_SIZE +
           code_directory_size(code_limit, identifier) +
           REQUIREMENTS_BLOB_SIZE + CMS_BLOB_SIZE;
}

void macho_adhoc_sign(uint8_t    *image,
                      uint64_t    code_limit,
                      uint64_t    exec_seg_limit,
                      const char *identifier)
{
    const uint32_t ident_bytes  = (uint32_t)strlen(identifier) + 1U;
    const uint32_t num_code      = code_slot_count(code_limit);
    const uint32_t cd_size       = code_directory_size(code_limit, identifier);
    const uint32_t cd_offset     = SUPERBLOB_HEADER_SIZE + SUPERBLOB_NUM_BLOBS * BLOB_INDEX_SIZE;
    const uint32_t req_offset    = cd_offset + cd_size;
    const uint32_t cms_offset    = req_offset + REQUIREMENTS_BLOB_SIZE;
    const uint32_t total_size    = cms_offset + CMS_BLOB_SIZE;
    const uint32_t hash_offset   = CD_HEADER_SIZE + ident_bytes + CD_NUM_SPECIAL_SLOTS * SHA256_DIGEST_SIZE;
    uint8_t       *sig           = image + code_limit;
    uint8_t       *cd            = sig + cd_offset;
    uint8_t       *req           = sig + req_offset;
    uint8_t       *code_hashes   = cd + hash_offset;
    uint32_t       slot_idx;

    /* SuperBlob header and blob index. */
    store_be32(sig,      CSMAGIC_EMBEDDED_SIGNATURE);
    store_be32(sig + 4,  total_size);
    store_be32(sig + 8,  SUPERBLOB_NUM_BLOBS);
    store_be32(sig + 12, CSSLOT_CODEDIRECTORY);
    store_be32(sig + 16, cd_offset);
    store_be32(sig + 20, CSSLOT_REQUIREMENTS);
    store_be32(sig + 24, req_offset);
    store_be32(sig + 28, CSSLOT_SIGNATURESLOT);
    store_be32(sig + 32, cms_offset);

    /* Empty requirements set and empty CMS wrapper. */
    store_be32(req,     CSMAGIC_REQUIREMENTS);
    store_be32(req + 4, REQUIREMENTS_BLOB_SIZE);
    store_be32(req + 8, 0);
    store_be32(sig + cms_offset,     CSMAGIC_BLOBWRAPPER);
    store_be32(sig + cms_offset + 4, CMS_BLOB_SIZE);

    /* CodeDirectory header. */
    store_be32(cd,      CSMAGIC_CODEDIRECTORY);
    store_be32(cd + 4,  cd_size);
    store_be32(cd + 8,  CD_VERSION);
    store_be32(cd + 12, CS_ADHOC);
    store_be32(cd + 16, hash_offset);
    store_be32(cd + 20, CD_HEADER_SIZE);            /* identOffset */
    store_be32(cd + 24, CD_NUM_SPECIAL_SLOTS);
    store_be32(cd + 28, num_code);
    store_be32(cd + 32, (uint32_t)code_limit);
    cd[36] = (uint8_t)SHA256_DIGEST_SIZE;           /* hashSize */
    cd[37] = (uint8_t)CS_HASHTYPE_SHA256;           /* hashType */
    cd[38] = 0;                                     /* platform */
    cd[39] = (uint8_t)CS_PAGE_SIZE_LOG2;            /* pageSize */
    store_be32(cd + 40, 0);                         /* spare2 */
    store_be32(cd + 44, 0);                         /* scatterOffset */
    store_be32(cd + 48, 0);                         /* teamOffset */
    store_be32(cd + 52, 0);                         /* spare3 */
    store_be64(cd + 56, 0);                         /* codeLimit64 (unused) */
    store_be64(cd + 64, 0);                         /* execSegBase (__TEXT fileoff) */
    store_be64(cd + 72, exec_seg_limit);            /* execSegLimit */
    store_be64(cd + 80, CS_EXECSEG_MAIN_BINARY);    /* execSegFlags */

    memcpy(cd + CD_HEADER_SIZE, identifier, ident_bytes);

    /* Special slots are stored just before the code slots, slot -1 nearest:
     * [-2 Requirements][-1 InfoPlist][code 0][code 1]...  InfoPlist is absent
     * (zero hash); Requirements hashes the requirements blob written above.
     */
    memset(code_hashes - SHA256_DIGEST_SIZE, 0, SHA256_DIGEST_SIZE);            /* slot -1 */
    sha256(req, REQUIREMENTS_BLOB_SIZE, code_hashes - 2U * SHA256_DIGEST_SIZE); /* slot -2 */

    for (slot_idx = 0; slot_idx < num_code; slot_idx++) {
        const uint64_t page_start = (uint64_t)slot_idx * CS_PAGE_SIZE;
        const uint64_t remaining  = code_limit - page_start;
        const size_t   page_len   = remaining < CS_PAGE_SIZE ? (size_t)remaining : CS_PAGE_SIZE;

        sha256(image + page_start, page_len, code_hashes + slot_idx * SHA256_DIGEST_SIZE);
    }
}
