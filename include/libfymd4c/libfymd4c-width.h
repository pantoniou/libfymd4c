/*
 * libfymd4c-width.h - the display width of text in terminal columns
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

#ifndef LIBFYMD4C_WIDTH_H
#define LIBFYMD4C_WIDTH_H

#include <stddef.h>

#include <libfymd4c/libfymd4c-util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * fymd_cp_width() - the columns one codepoint occupies
 * @cp: the codepoint
 *
 * Returns 0 for a combining mark, a zero width format character and a
 * control, 2 for East Asian Wide and Fullwidth and for the emoji that present
 * wide, and 1 otherwise. The tables are generated from Unicode 15.0.0.
 *
 * This is a per-codepoint measure. It does not segment grapheme clusters, so
 * a ZWJ emoji sequence measures as the sum of its parts and is wider than a
 * terminal that composes it will draw.
 *
 * Returns: 0, 1 or 2.
 */
int fymd_cp_width(unsigned int cp)
	FYMD_EXPORT;

/**
 * fymd_str_width() - the columns a UTF-8 string occupies
 * @s: the text, which need not be NUL terminated
 * @len: its length in bytes, or (size_t)-1 to measure to the NUL
 *
 * Sums fymd_cp_width() over the codepoints of @s. A malformed byte counts as
 * one column, so a broken encoding cannot make the measure run away.
 *
 * Returns: the width in columns, or 0 when @s is NULL.
 */
size_t fymd_str_width(const char *s, size_t len)
	FYMD_EXPORT;

/**
 * fymd_utf8_decode() - read one codepoint from UTF-8
 * @s: the text
 * @len: the bytes available at @s
 * @cp: where the codepoint is stored
 *
 * A malformed or truncated sequence yields the lead byte itself, so a caller
 * always advances and never loops.
 *
 * Returns: the bytes consumed, or 0 when @len is 0.
 */
size_t fymd_utf8_decode(const char *s, size_t len, unsigned int *cp)
	FYMD_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* LIBFYMD4C_WIDTH_H */
