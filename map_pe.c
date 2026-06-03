/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#include "map_pe.h"

#include <string.h>

/* Look for "Preferred load address is <hex>"; returns that base or image_base. */
static uint64_t find_load_base(const char *begin, const char *end, uint64_t image_base)
{
    static const char marker[]   = "Preferred load address is";
    const size_t      marker_len = sizeof(marker) - 1;

    while (begin && begin + marker_len < end) {

        if (memcmp(begin, marker, marker_len) == 0) {
            const char *pos = begin + marker_len;
            uint64_t    base;

            map_text_skip_spaces(&pos, end);

            if (map_text_parse_hex(&pos, end, &base))
                return base;
        }

        begin = (char *)memchr(begin + 1, marker[0], (size_t)(end - begin));
    }

    return image_base;
}

/* Parse one line as "ssss:oooooooo  name  rva_base ...".  On success adds an
 * item (orig_size 0, set later).  Returns 0 always; unparsable lines are skipped. */
static int parse_symbol_line(const char *begin,
                             const char *end,
                             uint32_t    va_start,
                             uint32_t    input_size,
                             uint64_t    load_base,
                             uint64_t    image_base,
                             MAP_TABLE  *out)
{
    const char *pos = begin;
    const char *name_begin;
    const char *name_end;
    uint64_t    scratch;
    uint64_t    rva_base;
    uint64_t    rva;
    char        name[256];
    size_t      name_len;

    map_text_skip_spaces(&pos, end);

    /* section index ':' offset */
    if ( ! map_text_parse_hex(&pos, end, &scratch))
        return 0;
    if (pos >= end || *pos != ':')
        return 0;
    ++pos;
    if ( ! map_text_parse_hex(&pos, end, &scratch))
        return 0;

    map_text_skip_spaces(&pos, end);

    /* symbol name token */
    name_begin = pos;
    while (pos < end && ! map_text_is_space(*pos))
        ++pos;
    name_end = pos;
    if (name_end == name_begin)
        return 0;

    map_text_skip_spaces(&pos, end);

    /* Rva+Base token must be hex; this rejects the section/class table lines. */
    if ( ! map_text_parse_hex(&pos, end, &rva_base))
        return 0;

    /* The character after the address must end the token (space or EOL). */
    if (pos < end && ! map_text_is_space(*pos))
        return 0;

    if (rva_base < load_base)
        return 0;
    rva = rva_base - load_base;

    if (rva < va_start || rva - va_start >= input_size)
        return 0;

    name_len = (size_t)(name_end - name_begin);
    if (name_len >= sizeof(name))
        name_len = sizeof(name) - 1;
    memcpy(name, name_begin, name_len);
    name[name_len] = 0;

    return map_table_add(out, name, rva + image_base, rva - va_start, 0);
}

int pe_parse_map(BUFFER map, uint32_t va_start, uint32_t input_size,
                 uint64_t image_base, MAP_TABLE *out)
{
    const char *begin;
    const char *end;
    const char *line;
    uint64_t    load_base;
    size_t      i;

    if ( ! map.buf || ! map.size)
        return 0;

    begin     = (const char *)map.buf;
    end       = begin + map.size;
    load_base = find_load_base(begin, end, image_base);

    for (line = begin; line < end; ) {
        const char *eol = line;

        while (eol < end && *eol != '\n')
            ++eol;

        if (parse_symbol_line(line, eol, va_start, input_size, load_base, image_base, out))
            return -1;

        line = (eol < end) ? eol + 1 : end;
    }

    if ( ! out->count)
        return 0;

    map_table_sort(out);

    /* Infer each symbol's size from the gap to the next symbol by value. */
    for (i = 0; i < out->count; i++) {
        const size_t next = (i + 1 < out->count) ? out->items[i + 1].input_offset
                                                 : input_size;
        out->items[i].orig_size = next - out->items[i].input_offset;
    }

    return 0;
}
