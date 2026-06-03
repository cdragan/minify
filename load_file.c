/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#include "load_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#elif defined(__APPLE__)
#   include <stdint.h>
#   include <mach-o/dyld.h>
#else
#   include <unistd.h>
#endif

BUFFER load_file(const char *filename, enum FILE_EXISTENCE existence)
{
    FILE  *file;
    BUFFER buf = { NULL, 0 };
    long   size;

    file = fopen(filename, "rb");
    if ( ! file) {
        if (existence == file_mandatory)
            perror(filename);
        return buf;
    }

    if (fseek(file, 0, SEEK_END)) {
        perror(filename);
        fclose(file);
        return buf;
    }

    size = ftell(file);
    if (size < 0) {
        perror(filename);
        fclose(file);
        return buf;
    }

    if (size == 0) {
        fprintf(stderr, "%s: empty file\n", filename);
        fclose(file);
        return buf;
    }

    if (fseek(file, 0, SEEK_SET)) {
        perror(filename);
        fclose(file);
        return buf;
    }

    buf = buf_alloc((size_t)size);
    if ( ! buf.buf) {
        perror(NULL);
        fclose(file);
        return buf;
    }

    if (fread(buf.buf, 1, (size_t)size, file) != (size_t)size) {
        perror(filename);
        fclose(file);

        free(buf.buf);
        buf.buf  = NULL;
        buf.size = 0;
        return buf;
    }

    fclose(file);

    return buf;
}

const char *get_exe_dir(void)
{
    static char exe_dir[1024];
    char       *last_slash;

    if (exe_dir[0])
        return exe_dir;

#if defined(_WIN32)
    {
        const DWORD len = GetModuleFileNameA(NULL, exe_dir, (DWORD)sizeof(exe_dir));

        if (len == 0 || len >= (DWORD)sizeof(exe_dir))
            exe_dir[0] = 0;
    }
#elif defined(__APPLE__)
    {
        uint32_t buf_size = (uint32_t)sizeof(exe_dir);

        if (_NSGetExecutablePath(exe_dir, &buf_size) != 0)
            exe_dir[0] = 0;
    }
#else
    {
        const ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);

        exe_dir[len > 0 ? (size_t)len : 0] = 0;
    }
#endif

    last_slash = strrchr(exe_dir, '/');

#if defined(_WIN32)
    {
        char *const backslash = strrchr(exe_dir, '\\');

        if ( ! last_slash || (backslash && backslash > last_slash))
            last_slash = backslash;
    }
#endif

    if (last_slash) {
        *last_slash = 0;
    }
    else {
        exe_dir[0] = '.';
        exe_dir[1] = 0;
    }

    return exe_dir;
}
