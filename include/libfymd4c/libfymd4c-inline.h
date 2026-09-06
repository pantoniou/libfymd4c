/*
 * libfymd4c-inline.h - reading the inline markdown of a short text
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

#ifndef LIBFYMD4C_INLINE_H
#define LIBFYMD4C_INLINE_H

#include <stdbool.h>
#include <stddef.h>

#include <libfymd4c/libfymd4c-util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum fymd_inline_attr - what an inline run carries
 *
 * @FYMD_IA_EM: emphasis, `*text*`
 * @FYMD_IA_STRONG: strong emphasis, `**text**`
 * @FYMD_IA_CODE: a code span, `` `text` ``
 * @FYMD_IA_DEL: struck through, `~~text~~`
 * @FYMD_IA_UNDERLINE: underlined, `_text_`, only with FYMD_IF_UNDERLINE
 * @FYMD_IA_LINK: the text of a link; its target is the run's href
 * @FYMD_IA_IMAGE: the alt text of an image
 * @FYMD_IA_BREAK: a hard line break; the run's text is a single newline
 *
 * The attributes nest, so a run may carry several.
 */
enum fymd_inline_attr {
	FYMD_IA_EM		= FYMD_BIT(0),
	FYMD_IA_STRONG		= FYMD_BIT(1),
	FYMD_IA_CODE		= FYMD_BIT(2),
	FYMD_IA_DEL		= FYMD_BIT(3),
	FYMD_IA_UNDERLINE	= FYMD_BIT(4),
	FYMD_IA_LINK		= FYMD_BIT(5),
	FYMD_IA_IMAGE		= FYMD_BIT(6),
	FYMD_IA_BREAK		= FYMD_BIT(7),
};

/**
 * enum fymd_inline_flags - what the parse accepts beyond CommonMark
 *
 * @FYMD_IF_STRIKETHROUGH: `~~text~~` marks a run struck through
 * @FYMD_IF_UNDERLINE: `_text_` underlines rather than emphasises
 * @FYMD_IF_PERMISSIVE_AUTOLINKS: a bare URL or address becomes a link
 * @FYMD_IF_WIKILINKS: `[[target]]` becomes a link
 * @FYMD_IF_LATEXMATH: `$math$` is recognised
 * @FYMD_IF_KEEP_HTML: an HTML span is reported as text rather than dropped
 */
enum fymd_inline_flags {
	FYMD_IF_STRIKETHROUGH		= FYMD_BIT(0),
	FYMD_IF_UNDERLINE		= FYMD_BIT(1),
	FYMD_IF_PERMISSIVE_AUTOLINKS	= FYMD_BIT(2),
	FYMD_IF_WIKILINKS		= FYMD_BIT(3),
	FYMD_IF_LATEXMATH		= FYMD_BIT(4),
	FYMD_IF_KEEP_HTML		= FYMD_BIT(5),
};

/* the parsed runs of one text */
struct fymd_inline;

/**
 * struct fymd_inline_run - one run of text that shares a set of attributes
 *
 * @text: the run's text, NUL terminated, with its markup resolved and its
 *        entities decoded
 * @len: its length in bytes
 * @attrs: a mask of enum fymd_inline_attr
 * @href: the target of the link or image the run sits in, or NULL
 */
struct fymd_inline_run {
	const char *text;
	size_t len;
	unsigned int attrs;
	const char *href;
};

/**
 * fymd_inline_parse() - read the inline markdown of a short text
 *
 * Reads @text as CommonMark and reports its inline content as a sequence of
 * runs, each carrying the attributes that apply to it. This is the piece a
 * caller wants when it draws a label or a cell itself and needs to know which
 * part of it is bold, rather than a rendered document.
 *
 * Block structure is not reported: a heading marker or a list marker is
 * consumed and its content reported as runs, and an indented line is not
 * taken for a code block. A soft line break becomes a space; a hard one is a
 * run carrying FYMD_IA_BREAK.
 *
 * @text: the text to read
 * @len: its length, or FYMD_NT when @text is NUL terminated
 * @flags: a mask of enum fymd_inline_flags
 *
 * Returns:
 * The runs, to release with fymd_inline_destroy(), or NULL on error.
 */
struct fymd_inline *
fymd_inline_parse(const char *text, size_t len, unsigned int flags)
	FYMD_EXPORT;

/* fymd_inline_destroy() - release what fymd_inline_parse() returned */
void
fymd_inline_destroy(struct fymd_inline *inl)
	FYMD_EXPORT;

/* fymd_inline_count() - how many runs the text produced */
size_t
fymd_inline_count(const struct fymd_inline *inl)
	FYMD_EXPORT;

/**
 * fymd_inline_get() - one run, by index
 *
 * Returns:
 * The run, or NULL when @idx is past the end. It stays valid until
 * fymd_inline_destroy().
 */
const struct fymd_inline_run *
fymd_inline_get(const struct fymd_inline *inl, size_t idx)
	FYMD_EXPORT;

/**
 * fymd_inline_plain() - the text with its markup removed
 *
 * The runs joined, which is what the text reads as once the markers are gone.
 *
 * Returns:
 * A NUL terminated string owned by @inl, or NULL on error.
 */
const char *
fymd_inline_plain(const struct fymd_inline *inl)
	FYMD_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* LIBFYMD4C_INLINE_H */
