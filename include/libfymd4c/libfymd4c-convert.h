/*
 * libfymd4c-convert.h - stateless Markdown conversions (HTML, heal)
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

#ifndef LIBFYMD4C_CONVERT_H
#define LIBFYMD4C_CONVERT_H

#include <stddef.h>

#include "libfymd4c-util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stateless conversions over the bundled md4c parser. These do not need a
 * fymd_renderer; results are heap-allocated and freed with fymd_free(). */

/* Heal incomplete / mid-stream Markdown: close unclosed bold/italic/code/math
 * markers and fenced blocks, complete dangling links, drop trailing partial
 * markup, etc. A pre-parser text transform. On success *out is a heap-allocated
 * NUL-terminated string (free with fymd_free()), *out_len its length. 0 / -1. */
int fymd_heal(const char *md, size_t len, char **out, size_t *out_len) FYMD_EXPORT;

/* Convenience: returns a heap string (free with fymd_free()) or NULL. */
char *fymd_heal_to_string(const char *md, size_t len) FYMD_EXPORT;

/* HTML render flags. */
enum fymd_html_flags {
    FYMD_HTML_XHTML             = FYMD_BIT(0), /* emit XHTML */
    FYMD_HTML_VERBATIM_ENTITIES = FYMD_BIT(1), /* do not translate entities */
    FYMD_HTML_SKIP_UTF8_BOM     = FYMD_BIT(2)  /* skip a leading UTF-8 BOM */
};

/* Render Markdown to HTML (md4c's HTML renderer; mainly for conformance and
 * non-terminal output). parser_flags are md4c MD_FLAG_* (0 => CommonMark);
 * flags are FYMD_HTML_*. On success *out is a heap-allocated NUL-terminated
 * string (free with fymd_free()), *out_len its length. Returns 0 / -1. */
int fymd_render_html(const char *md, size_t len, unsigned parser_flags,
                     unsigned flags, char **out, size_t *out_len) FYMD_EXPORT;

/* Convenience: returns a heap string (free with fymd_free()) or NULL. */
char *fymd_render_html_to_string(const char *md, size_t len,
                                 unsigned parser_flags, unsigned flags) FYMD_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* LIBFYMD4C_CONVERT_H */
