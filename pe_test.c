/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Dragan
 */

/* Minimal no-CRT Windows console program.  It is packed by minify and run in CI to
 * verify the full unpack -> import-load -> entry path on Windows.  It imports three
 * kernel32 functions so minify's import rewriting (MINI_IAT + pe_load_imports) is
 * exercised end to end.  Built without the CRT, so the entry never returns and ends
 * with ExitProcess. */

#include <stdint.h>

#define WINAPI __stdcall

#define STD_OUTPUT_HANDLE ((uint32_t)-11)

/* The no-CRT stub flags compile this translation unit as C++ (-TP).  Force C linkage
 * so the kernel32 imports resolve against the C-decorated kernel32.lib symbols rather
 * than being name-mangled, and so -entry:start matches the undecorated entry symbol. */
#ifdef __cplusplus
extern "C" {
#endif

void *WINAPI GetStdHandle(uint32_t std_handle);
int   WINAPI WriteFile(void *file, const void *buffer, uint32_t bytes_to_write,
                       uint32_t *bytes_written, void *overlapped);
void  WINAPI ExitProcess(uint32_t exit_code);

void WINAPI start(void)
{
    static const char hello[] = "Hello";
    uint32_t          bytes_written;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), hello, sizeof(hello) - 1,
              &bytes_written, 0);

    ExitProcess(0);
}

#ifdef __cplusplus
}
#endif
