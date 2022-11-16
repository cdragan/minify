/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "arith_decode.h"
#include "pe_common.h"

const LIVE_LAYOUT *live_layout = (LIVE_LAYOUT *)(uintptr_t)0xFACECAFEBEEFF00D;

int STDCALL loader(void)
{
    arith_decode(live_layout->lz77_data, live_layout->lz77_data_size,
                 live_layout->comp_data, live_layout->comp_data_size);

    return live_layout->lz77_decomp(live_layout);
}

// The code below is used in place of MS Visual C Runtime library in 32-bit builds.
// In 32-bit builds the compiler calls these functions in order to perform 64-bit arithmetic.
// This code has been copied from GitHub mmozeiko/win32_crt_float.cpp
#ifdef _M_IX86
#   include <immintrin.h>
#   define CRT_LOWORD(x) dword ptr [x+0]
#   define CRT_HIWORD(x) dword ptr [x+4]

extern "C" {

    __declspec(naked) void _aulldiv()
    {
        #define DVND esp + 12 // stack address of dividend (a)
        #define DVSR esp + 20 // stack address of divisor (b)

        __asm {
            push    ebx
            push    esi

            ;
            ; Now do the divide.  First look to see if the divisor is less than 4194304K.
            ; If so, then we can use a simple algorithm with word divides, otherwise
            ; things get a little more complex.
            ;

            mov     eax, CRT_HIWORD(DVSR) ; check to see if divisor < 4194304K
            or      eax, eax
            jnz     short L1        ; nope, gotta do this the hard way
            mov     ecx, CRT_LOWORD(DVSR) ; load divisor
            mov     eax, CRT_HIWORD(DVND) ; load high word of dividend
            xor     edx, edx
            div     ecx             ; get high order bits of quotient
            mov     ebx, eax        ; save high bits of quotient
            mov     eax, CRT_LOWORD(DVND) ; edx:eax <- remainder:lo word of dividend
            div     ecx             ; get low order bits of quotient
            mov     edx, ebx        ; edx:eax <- quotient hi:quotient lo
            jmp     short L2        ; restore stack and return

            ;
            ; Here we do it the hard way.  Remember, eax contains DVSRHI
            ;

L1:
            mov     ecx, eax        ; ecx:ebx <- divisor
            mov     ebx, CRT_LOWORD(DVSR)
            mov     edx, CRT_HIWORD(DVND) ; edx:eax <- dividend
            mov     eax, CRT_LOWORD(DVND)
L3:
            shr     ecx, 1          ; shift divisor right one bit; hi bit <- 0
            rcr     ebx, 1
            shr     edx, 1          ; shift dividend right one bit; hi bit <- 0
            rcr     eax, 1
            or      ecx, ecx
            jnz     short L3        ; loop until divisor < 4194304K
            div     ebx             ; now divide, ignore remainder
            mov     esi, eax        ; save quotient

            ;
            ; We may be off by one, so to check, we will multiply the quotient
            ; by the divisor and check the result against the orignal dividend
            ; Note that we must also check for overflow, which can occur if the
            ; dividend is close to 2**64 and the quotient is off by 1.
            ;

            mul     CRT_HIWORD(DVSR) ; QUOT * CRT_HIWORD(DVSR)
            mov     ecx, eax
            mov     eax, CRT_LOWORD(DVSR)
            mul     esi             ; QUOT * CRT_LOWORD(DVSR)
            add     edx, ecx        ; EDX:EAX = QUOT * DVSR
            jc      short L4        ; carry means Quotient is off by 1

            ;
            ; do long compare here between original dividend and the result of the
            ; multiply in edx:eax.  If original is larger or equal, we are ok, otherwise
            ; subtract one (1) from the quotient.
            ;

            cmp     edx, CRT_HIWORD(DVND) ; compare hi words of result and original
            ja      short L4        ; if result > original, do subtract
            jb      short L5        ; if result < original, we are ok
            cmp     eax, CRT_LOWORD(DVND) ; hi words are equal, compare lo words
            jbe     short L5        ; if less or equal we are ok, else subtract
L4:
            dec     esi             ; subtract 1 from quotient
L5:
            xor     edx, edx        ; edx:eax <- quotient
            mov     eax, esi

            ;
            ; Just the cleanup left to do.  edx:eax contains the quotient.
            ; Restore the saved registers and return.
            ;

L2:
            pop     esi
            pop     ebx

            ret     16
        }

        #undef DVND
        #undef DVSR
    }

    __declspec(naked) void _aullshr()
    {
        __asm
        {
            cmp     cl, 64
            jae     short retzero
            ;
            ; Handle shifts of between 0 and 31 bits
            ;
            cmp     cl, 32
            jae     short more32
            shrd    eax, edx, cl
            shr     edx, cl
            ret
            ;
            ; Handle shifts of between 32 and 63 bits
            ;
    more32:
            mov     eax, edx
            xor     edx, edx
            and     cl, 31
            shr     eax, cl
            ret
            ;
            ; return 0 in edx:eax
            ;
    retzero:
            xor     eax, eax
            xor     edx, edx
            ret
        }
    }
}
#endif
