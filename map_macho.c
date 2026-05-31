/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

#include "map_macho.h"

#include <string.h>

/* Translate an ld64 vmaddr into an offset within the combined-raw buffer
 * [__TEXT][rebases][__DATA].  Returns 0 and sets *offset on success. */
static int translate_addr(const MACHO_MAP_CONTEXT *ctx, uint64_t addr, size_t *offset)
{
    if (addr >= ctx->text_vmaddr && addr < ctx->text_vmaddr + ctx->text_size) {
        *offset = (size_t)(addr - ctx->text_vmaddr);
        return 0;
    }

    if (ctx->data_folded &&
        addr >= ctx->data_vmaddr && addr < ctx->data_vmaddr + ctx->data_content_size) {
        *offset = ctx->text_size + ctx->rebase_bytes + (size_t)(addr - ctx->data_vmaddr);
        return 0;
    }

    return -1;
}

/* A '#' line that ends with ':' introduces a section; return whether it is the
 * live "# Symbols:" section.  Other '#' lines (column headers, comments) leave
 * the current section state untouched, so this is only consulted for them. */
static int is_symbols_header(const char *begin, const char *end)
{
    static const char marker[]   = "# Symbols:";
    const size_t      marker_len = sizeof(marker) - 1;

    return (size_t)(end - begin) >= marker_len &&
           memcmp(begin, marker, marker_len) == 0;
}

static int line_is_section_header(const char *begin, const char *end)
{
    /* Trim trailing whitespace, then test for a ':' terminator. */
    while (end > begin && map_text_is_space(end[-1]))
        --end;

    return end > begin && end[-1] == ':';
}

static int parse_symbol_line(const char *begin, const char *end,
                             const MACHO_MAP_CONTEXT *ctx, MAP_TABLE *out)
{
    const char *pos = begin;
    const char *name_begin;
    const char *name_end;
    uint64_t    addr;
    uint64_t    size;
    size_t      offset;
    char        name[256];
    size_t      name_len;

    map_text_skip_spaces(&pos, end);

    if ( ! map_text_parse_hex(&pos, end, &addr))
        return 0;

    map_text_skip_spaces(&pos, end);

    if ( ! map_text_parse_hex(&pos, end, &size))
        return 0;

    map_text_skip_spaces(&pos, end);

    /* Optional "[ n]" file-index token. */
    if (pos < end && *pos == '[') {
        while (pos < end && *pos != ']')
            ++pos;
        if (pos < end)
            ++pos;
        map_text_skip_spaces(&pos, end);
    }

    name_begin = pos;
    name_end   = end;
    while (name_end > name_begin && map_text_is_space(name_end[-1]))
        --name_end;
    if (name_end == name_begin)
        return 0;

    if (translate_addr(ctx, addr, &offset))
        return 0;

    name_len = (size_t)(name_end - name_begin);
    if (name_len >= sizeof(name))
        name_len = sizeof(name) - 1;
    memcpy(name, name_begin, name_len);
    name[name_len] = 0;

    return map_table_add(out, name, addr, offset, (size_t)size);
}

int macho_parse_map(BUFFER map, const MACHO_MAP_CONTEXT *ctx, MAP_TABLE *out)
{
    const char *begin;
    const char *end;
    const char *line;
    int         in_symbols = 0;

    if ( ! map.buf || ! map.size)
        return 0;

    begin = (const char *)map.buf;
    end   = begin + map.size;

    for (line = begin; line < end; ) {
        const char *eol = line;

        while (eol < end && *eol != '\n')
            ++eol;

        if (line < eol && *line == '#') {
            if (line_is_section_header(line, eol))
                in_symbols = is_symbols_header(line, eol);
        }
        else if (in_symbols) {
            if (parse_symbol_line(line, eol, ctx, out))
                return -1;
        }

        line = (eol < end) ? eol + 1 : end;
    }

    return 0;
}
