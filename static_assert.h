/* SPDX-License-Identifier: MIT
 * Copyright (c) 2021-2026 Chris Dragan
 */

#pragma once

#ifdef __cplusplus
#define STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#else
#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#endif
