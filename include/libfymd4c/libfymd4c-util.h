/*
 * libfymd4c-util.h - portability and utility macros for libfymd4c
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBFYMD4C_UTIL_H
#define LIBFYMD4C_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FYMD_EXPORT - mark a symbol as part of the shared-library public ABI.
 * On GCC/Clang (>= 4) overrides -fvisibility=hidden for the annotated symbol;
 * elsewhere expands to nothing. */
#if defined(__GNUC__) && __GNUC__ >= 4
#define FYMD_EXPORT __attribute__((visibility("default")))
#else
#define FYMD_EXPORT /* nothing */
#endif

/* FYMD_BIT(x) - unsigned bitmask with bit x set. */
#ifndef FYMD_BIT
#define FYMD_BIT(x) (1U << (x))
#endif

/* Library version string, e.g. "0.5.3". */
const char *fymd_library_version(void) FYMD_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* LIBFYMD4C_UTIL_H */
