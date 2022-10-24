/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

inline static int count_leading_zeroes(unsigned int value)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(value);
#elif defined(_MSC_VER)
    unsigned long bit;

    _BitScanReverse(&bit, value);

    return (int)bit;
#else
#error "Not implemented!"
#endif
}
