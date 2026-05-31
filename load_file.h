/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "buffer.h"

enum FILE_EXISTENCE {
    file_optional,
    file_mandatory
};

BUFFER load_file(const char *filename, enum FILE_EXISTENCE existence);
