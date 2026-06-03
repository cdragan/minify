/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#include <stdint.h>

/* Returns the size in bytes of the embedded ad-hoc code signature that
 * macho_adhoc_sign() writes for a file of `code_limit` bytes.  The caller
 * reserves this many bytes at the signature offset and records it in
 * LC_CODE_SIGNATURE.datasize before the load commands are hashed.
 */
uint32_t macho_adhoc_sig_size(uint64_t code_limit, const char *identifier);

/* Write an ad-hoc embedded code signature (CodeDirectory + empty Requirements
 * + empty CMS wrapper) at image + code_limit.  Hashes the bytes in
 * [0, code_limit), so the load commands -- including LC_CODE_SIGNATURE and the
 * final __LINKEDIT sizes -- must already be in place.  `exec_seg_limit` is the
 * file size of the __TEXT (code) segment.
 */
void macho_adhoc_sign(uint8_t    *image,
                      uint64_t    code_limit,
                      uint64_t    exec_seg_limit,
                      const char *identifier);
