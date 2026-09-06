/*
 * md4c-inline.c - reading the inline markdown of a short text
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

#include <stdlib.h>
#include <string.h>

#include "md4c.h"

#include <libfymd4c/libfymd4c-inline.h>

/* how many runs are allocated before the array is grown */
#define INLINE_INITIAL_RUNS 8

struct fymd_inline {
    struct fymd_inline_run *runs;
    size_t nruns;
    size_t aruns;
    char *plain;
    /* the hrefs the runs point at, kept so that a run's href outlives the
     * span that carried it */
    char **hrefs;
    size_t nhrefs;
    size_t ahrefs;
};

/* struct inline_ctx - what the md4c callbacks accumulate into */
struct inline_ctx {
    struct fymd_inline *inl;
    unsigned int attrs;
    const char *href;   /* the target of the enclosing link or image */
    int failed;
};

/* Keep a copy of a link target for as long as the runs live. */
static const char *inline_keep_href(struct fymd_inline *inl, const char *text,
                                    size_t len)
{
    char **nh;
    char *copy;

    if (inl->nhrefs == inl->ahrefs) {
        size_t na = inl->ahrefs ? inl->ahrefs * 2 : 4;

        nh = realloc(inl->hrefs, na * sizeof(*nh));
        if (nh == NULL)
            return NULL;
        inl->hrefs = nh;
        inl->ahrefs = na;
    }
    copy = malloc(len + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    inl->hrefs[inl->nhrefs++] = copy;
    return copy;
}

/*
 * Append text to the runs. A run that shares the attributes and the target of
 * the one before it is joined to it, so that a run is a run and not one entry
 * per callback.
 */
static int inline_add(struct inline_ctx *ctx, const char *text, size_t len,
                      unsigned int attrs)
{
    struct fymd_inline *inl = ctx->inl;
    struct fymd_inline_run *run;
    char *nt;

    if (len == 0)
        return 0;

    if (inl->nruns > 0) {
        run = &inl->runs[inl->nruns - 1];
        if (run->attrs == attrs && run->href == ctx->href &&
            (attrs & FYMD_IA_BREAK) == 0) {
            nt = realloc((char *)run->text, run->len + len + 1);
            if (nt == NULL)
                return -1;
            memcpy(nt + run->len, text, len);
            run->len += len;
            nt[run->len] = '\0';
            run->text = nt;
            return 0;
        }
    }

    if (inl->nruns == inl->aruns) {
        size_t na = inl->aruns ? inl->aruns * 2 : INLINE_INITIAL_RUNS;
        struct fymd_inline_run *nr;

        nr = realloc(inl->runs, na * sizeof(*nr));
        if (nr == NULL)
            return -1;
        inl->runs = nr;
        inl->aruns = na;
    }

    run = &inl->runs[inl->nruns];
    nt = malloc(len + 1);
    if (nt == NULL)
        return -1;
    memcpy(nt, text, len);
    nt[len] = '\0';
    run->text = nt;
    run->len = len;
    run->attrs = attrs;
    run->href = ctx->href;
    inl->nruns++;
    return 0;
}

/* The attribute an md4c span maps to. */
static unsigned int inline_span_attr(MD_SPANTYPE type)
{
    switch (type) {
    case MD_SPAN_EM:
        return FYMD_IA_EM;
    case MD_SPAN_STRONG:
        return FYMD_IA_STRONG;
    case MD_SPAN_CODE:
        return FYMD_IA_CODE;
    case MD_SPAN_DEL:
        return FYMD_IA_DEL;
    case MD_SPAN_U:
        return FYMD_IA_UNDERLINE;
    case MD_SPAN_A:
    case MD_SPAN_WIKILINK:
        return FYMD_IA_LINK;
    case MD_SPAN_IMG:
        return FYMD_IA_IMAGE;
    default:
        return 0;
    }
}

static int inline_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    struct inline_ctx *ctx = (struct inline_ctx *)userdata;
    MD_SPAN_A_DETAIL *a;
    MD_SPAN_IMG_DETAIL *img;

    ctx->attrs |= inline_span_attr(type);

    if (type == MD_SPAN_A && detail != NULL) {
        a = (MD_SPAN_A_DETAIL *)detail;
        ctx->href = inline_keep_href(ctx->inl, a->href.text, a->href.size);
    } else if (type == MD_SPAN_IMG && detail != NULL) {
        img = (MD_SPAN_IMG_DETAIL *)detail;
        ctx->href = inline_keep_href(ctx->inl, img->src.text, img->src.size);
    }
    return 0;
}

static int inline_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    struct inline_ctx *ctx = (struct inline_ctx *)userdata;

    (void)detail;

    ctx->attrs &= ~inline_span_attr(type);
    if (type == MD_SPAN_A || type == MD_SPAN_IMG || type == MD_SPAN_WIKILINK)
        ctx->href = NULL;
    return 0;
}

static int inline_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                       void *userdata)
{
    struct inline_ctx *ctx = (struct inline_ctx *)userdata;
    int rc;

    switch (type) {
    case MD_TEXT_NULLCHAR:
        return 0;

    case MD_TEXT_BR:
        rc = inline_add(ctx, "\n", 1, ctx->attrs | FYMD_IA_BREAK);
        break;

    case MD_TEXT_SOFTBR:
        rc = inline_add(ctx, " ", 1, ctx->attrs);
        break;

    default:
        rc = inline_add(ctx, text, size, ctx->attrs);
        break;
    }

    if (rc != 0)
        ctx->failed = 1;
    return 0;
}

/* Block structure is consumed and not reported; only its content matters. */
static int inline_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    (void)type;
    (void)detail;
    (void)userdata;
    return 0;
}

struct fymd_inline *fymd_inline_parse(const char *text, size_t len,
                                      unsigned int flags)
{
    MD_PARSER parser;
    struct inline_ctx ctx;
    struct fymd_inline *inl;
    unsigned int md_flags;

    if (text == NULL)
        return NULL;
    if (len == FYMD_NT)
        len = strlen(text);

    inl = (struct fymd_inline *)malloc(sizeof(*inl));
    if (inl == NULL)
        return NULL;
    memset(inl, 0, sizeof(*inl));

    /* an indented label is a label, not a code block */
    md_flags = MD_FLAG_NOINDENTEDCODEBLOCKS;
    if ((flags & FYMD_IF_KEEP_HTML) == 0)
        md_flags |= MD_FLAG_NOHTML;
    if (flags & FYMD_IF_STRIKETHROUGH)
        md_flags |= MD_FLAG_STRIKETHROUGH;
    if (flags & FYMD_IF_UNDERLINE)
        md_flags |= MD_FLAG_UNDERLINE;
    if (flags & FYMD_IF_PERMISSIVE_AUTOLINKS)
        md_flags |= MD_FLAG_PERMISSIVEAUTOLINKS;
    if (flags & FYMD_IF_WIKILINKS)
        md_flags |= MD_FLAG_WIKILINKS;
    if (flags & FYMD_IF_LATEXMATH)
        md_flags |= MD_FLAG_LATEXMATHSPANS;

    memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags = md_flags;
    parser.enter_block = inline_block;
    parser.leave_block = inline_block;
    parser.enter_span = inline_enter_span;
    parser.leave_span = inline_leave_span;
    parser.text = inline_text;

    memset(&ctx, 0, sizeof(ctx));
    ctx.inl = inl;

    if (md_parse(text, (MD_SIZE)len, &parser, &ctx) != 0 || ctx.failed) {
        fymd_inline_destroy(inl);
        return NULL;
    }
    return inl;
}

void fymd_inline_destroy(struct fymd_inline *inl)
{
    size_t i;

    if (inl == NULL)
        return;
    for (i = 0; i < inl->nruns; i++)
        free((char *)inl->runs[i].text);
    free(inl->runs);
    for (i = 0; i < inl->nhrefs; i++)
        free(inl->hrefs[i]);
    free(inl->hrefs);
    free(inl->plain);
    free(inl);
}

size_t fymd_inline_count(const struct fymd_inline *inl)
{
    return inl != NULL ? inl->nruns : 0;
}

const struct fymd_inline_run *fymd_inline_get(const struct fymd_inline *inl,
                                              size_t idx)
{
    if (inl == NULL || idx >= inl->nruns)
        return NULL;
    return &inl->runs[idx];
}

const char *fymd_inline_plain(const struct fymd_inline *inl)
{
    struct fymd_inline *m = (struct fymd_inline *)inl;
    size_t i, total = 0, pos = 0;

    if (inl == NULL)
        return NULL;
    if (m->plain != NULL)
        return m->plain;

    for (i = 0; i < inl->nruns; i++)
        total += inl->runs[i].len;
    m->plain = (char *)malloc(total + 1);
    if (m->plain == NULL)
        return NULL;
    for (i = 0; i < inl->nruns; i++) {
        memcpy(m->plain + pos, inl->runs[i].text, inl->runs[i].len);
        pos += inl->runs[i].len;
    }
    m->plain[pos] = '\0';
    return m->plain;
}
