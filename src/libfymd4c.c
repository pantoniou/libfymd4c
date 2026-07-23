/*
 * libfymd4c.c - public facade over the md4c ANSI renderer, styling and
 * streaming modules.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "md4c.h"
#include "md4c-ansi.h"
#include "md4c-style.h"
#include "md4c-stream.h"
#include "md4c-html.h"
#include "md4c-heal.h"

#include <libfymd4c.h>

#ifdef MD4C_WITH_FYTS
#include <fyts/fyts.h>
#endif

#ifndef FYMD_VERSION_STRING
#define FYMD_VERSION_STRING "0.0.0"
#endif

/* Growable output buffer for one-shot rendering. */
struct fymd_buf {
    char *data;
    size_t size;
    size_t asize;
    int oom;
};

struct fymd_renderer {
    struct fymd_renderer_cfg cfg;   /* owned copy (string fields strdup'd) */
    MD_ANSI_STYLE *style;           /* built once; freed in destroy */
    unsigned parser_flags;          /* resolved (cfg 0 => default set) */
    unsigned renderer_flags;        /* resolved MD_ANSI_FLAG_* for md_ansi */
    int width;                      /* MD_ANSI_WIDTH_* / columns */
    MD4C_STREAM *stream;            /* lazily created live stream, or NULL */

    struct fymd_line_limit_opts limit;
    char *limit_separator;
    struct fymd_buf screen;         /* reconstructed complete progressive screen */
    struct fymd_buf visible;        /* viewport returned by the previous push */
    struct fymd_buf viewport;       /* projection scratch / finish result */
    size_t stream_active_rows;      /* mutable rows in the underlying stream */
};

static int fymd_buf_finish(struct fymd_buf *b, char **out, size_t *out_len);

static void
fymd_buf_append(const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    struct fymd_buf *b = (struct fymd_buf *) userdata;
    if(b->oom)
        return;
    if(b->size + size + 1 > b->asize) {
        size_t na = b->asize ? b->asize : 256;
        char *nd;
        while(na < b->size + size + 1)
            na += na / 2 + 1;
        nd = realloc(b->data, na);
        if(nd == NULL) {
            b->oom = 1;
            return;
        }
        b->data = nd;
        b->asize = na;
    }
    memcpy(b->data + b->size, text, size);
    b->size += size;
}

static void
fymd_buf_reset(struct fymd_buf *b)
{
    b->size = 0;
    b->oom = 0;
}

static void
fymd_buf_fini(struct fymd_buf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int
fymd_buf_put(struct fymd_buf *b, const char *data, size_t size)
{
    if(size == 0)
        return 0;
    fymd_buf_append(data, (MD_SIZE) size, b);
    return b->oom ? -1 : 0;
}

static char *
fymd_strdup(const char *s)
{
    size_t n;
    char *d;
    if(s == NULL)
        return NULL;
    n = strlen(s) + 1;
    d = malloc(n);
    if(d != NULL)
        memcpy(d, s, n);
    return d;
}

static size_t
fymd_count_rows(const char *data, size_t size)
{
    size_t i, rows = 0;
    for(i = 0; i < size; i++)
        if(data[i] == '\n')
            rows++;
    if(size > 0 && data[size - 1] != '\n')
        rows++;
    return rows;
}

static size_t
fymd_after_rows(const char *data, size_t size, size_t rows)
{
    size_t i, seen = 0;
    if(rows == 0)
        return 0;
    for(i = 0; i < size; i++)
        if(data[i] == '\n' && ++seen == rows)
            return i + 1;
    return size;
}

static void
fymd_drop_trailing_rows(struct fymd_buf *b, size_t rows)
{
    size_t pos = b->size, k;
    for(k = 0; k < rows && pos > 0; k++) {
        if(pos > 0 && b->data[pos - 1] == '\n')
            pos--;
        while(pos > 0 && b->data[pos - 1] != '\n')
            pos--;
    }
    b->size = pos;
}

/* Validate the deliberately small printf subset accepted for separators. */
static int
fymd_separator_valid(const char *fmt)
{
    size_t i;
    int conversions = 0;
    if(fmt == NULL)
        return 1;
    for(i = 0; fmt[i] != '\0'; i++) {
        if(fmt[i] == '\n' || fmt[i] == '\r')
            return 0;
        if(fmt[i] != '%')
            continue;
        i++;
        if(fmt[i] == '%')
            continue;
        if(fmt[i] == 'd') {
            conversions++;
            continue;
        }
        return 0;
    }
    return conversions == 1;
}

static int
fymd_append_separator(struct fymd_buf *b, const char *fmt, size_t omitted)
{
    char number[32];
    size_t i, start = 0;
    int n = snprintf(number, sizeof(number), "%zu", omitted);
    if(n < 0)
        return -1;
    for(i = 0; fmt[i] != '\0'; i++) {
        if(fmt[i] != '%')
            continue;
        if(fymd_buf_put(b, fmt + start, i - start) != 0)
            return -1;
        i++;
        if(fmt[i] == '%') {
            if(fymd_buf_put(b, "%", 1) != 0)
                return -1;
        } else {
            if(fymd_buf_put(b, number, (size_t) n) != 0)
                return -1;
        }
        start = i + 1;
    }
    if(fymd_buf_put(b, fmt + start, strlen(fmt + start)) != 0)
        return -1;
    return fymd_buf_put(b, "\n", 1);
}

/* Length of a CSI SGR sequence, or zero. */
static size_t
fymd_sgr_len(const char *data, size_t size)
{
    size_t i;
    unsigned char c;
    if(size < 3 || (unsigned char)data[0] != 0x1b || data[1] != '[')
        return 0;
    for(i = 2; i < size; i++) {
        c = (unsigned char)data[i];
        if(c >= 0x40 && c <= 0x7e)
            return data[i] == 'm' ? i + 1 : 0;
    }
    return 0;
}

static int
fymd_sgr_is_reset(const char *seq, size_t size)
{
    size_t i;
    for(i = 2; i + 1 < size; i++)
        if(seq[i] != '0' && seq[i] != ';')
            return 0;
    return 1;
}

static int
fymd_has_sgr(const char *data, size_t size)
{
    size_t i;
    for(i = 0; i < size; i++)
        if(fymd_sgr_len(data + i, size - i) > 0)
            return 1;
    return 0;
}

/* Make a sliced row range terminal-independent. Start from a known neutral
 * state, then replay the SGR history following the last full reset before the
 * slice. This handles generated styling and input retained under SGR_SAFE. */
static int
fymd_append_sgr_replay(struct fymd_buf *out, const char *data, size_t size)
{
    size_t i, n, replay = 0;
    int found = 0;
    for(i = 0; i < size; ) {
        n = fymd_sgr_len(data + i, size - i);
        if(n > 0) {
            found = 1;
            if(fymd_sgr_is_reset(data + i, n))
                replay = i + n;
            i += n;
        } else {
            i++;
        }
    }
    if(!found)
        return 0;
    if(fymd_buf_put(out, "\x1b[0m", 4) != 0)
        return -1;
    for(i = replay; i < size; ) {
        n = fymd_sgr_len(data + i, size - i);
        if(n > 0) {
            if(fymd_buf_put(out, data + i, n) != 0)
                return -1;
            i += n;
        } else {
            i++;
        }
    }
    return 0;
}

static int
fymd_project(struct fymd_renderer *r, const char *data, size_t size,
             struct fymd_buf *out)
{
    size_t rows, head, tail, off;

    fymd_buf_reset(out);
    rows = fymd_count_rows(data, size);
    if(r->limit.mode == FYMD_LLM_NONE || r->limit.max_lines == 0 ||
       rows <= r->limit.max_lines)
        return fymd_buf_put(out, data, size);

    if(r->limit.mode == FYMD_LLM_SCROLL) {
        off = fymd_after_rows(data, size, rows - r->limit.max_lines);
        if(fymd_append_sgr_replay(out, data, off) != 0)
            return -1;
        return fymd_buf_put(out, data + off, size - off);
    }

    if(r->limit.split == FYMD_LLS_BALANCED)
        head = (r->limit.max_lines - 1) / 2;
    else
        head = r->limit.head_lines;
    tail = r->limit.max_lines - head - 1;

    off = fymd_after_rows(data, size, head);
    if(fymd_buf_put(out, data, off) != 0)
        return -1;
    /* Neutralize styling opened in the retained head before the separator. */
    if((fymd_has_sgr(data, off) && fymd_buf_put(out, "\x1b[0m", 4) != 0) ||
       fymd_append_separator(out, r->limit_separator, rows - head - tail) != 0)
        return -1;
    off = fymd_after_rows(data, size, rows - tail);
    if(fymd_append_sgr_replay(out, data, off) != 0)
        return -1;
    return fymd_buf_put(out, data + off, size - off);
}

static size_t
fymd_common_row_prefix(const struct fymd_buf *a, const struct fymd_buf *b)
{
    size_t i = 0, start = 0, n = a->size < b->size ? a->size : b->size;
    while(i < n && a->data[i] == b->data[i]) {
        if(a->data[i] == '\n')
            start = i + 1;
        i++;
    }
    return start;
}

static MD_STYLE_BG
map_background(enum fymd_background bg)
{
    switch(bg) {
        case FYMD_BG_DARK:  return MD_STYLE_BG_DARK;
        case FYMD_BG_LIGHT: return MD_STYLE_BG_LIGHT;
        case FYMD_BG_AUTO:
        default:            return MD_STYLE_BG_AUTO;
    }
}

struct fymd_renderer *
fymd_renderer_create(const struct fymd_renderer_cfg *cfg)
{
    struct fymd_renderer *r;
    struct fymd_renderer_cfg defcfg;
    MD_ANSI_STYLE_OPTS sopts;
    unsigned rf = 0;

    if(cfg == NULL) {
        memset(&defcfg, 0, sizeof(defcfg));
        defcfg.flags = FYMD_RF_DEFAULT;
        defcfg.width = FYMD_WIDTH_AUTO;
        cfg = &defcfg;
    }

    r = calloc(1, sizeof(*r));
    if(r == NULL)
        return NULL;

    /* Own the cfg, including copies of its string fields. */
    r->cfg = *cfg;
    r->cfg.style = fymd_strdup(cfg->style);
    r->cfg.style_path = fymd_strdup(cfg->style_path);
    r->cfg.theme = fymd_strdup(cfg->theme);
    r->cfg.code_theme = fymd_strdup(cfg->code_theme);
    if((cfg->style && !r->cfg.style) ||
       (cfg->style_path && !r->cfg.style_path) ||
       (cfg->theme && !r->cfg.theme) ||
       (cfg->code_theme && !r->cfg.code_theme))
        goto err;

    /* Build the styling context once. */
    memset(&sopts, 0, sizeof(sopts));
    sopts.background = map_background(cfg->background);
    sopts.reverse = -1;   /* the fenced-code bubble is YAML-driven (code.reverse) */

    if(fy_generic_is_valid(cfg->style_generic) && cfg->style_generic.v != 0)
        r->style = md_ansi_style_create_from_generic(cfg->style_generic, &sopts);
    else if(r->cfg.style_path != NULL)
        r->style = md_ansi_style_create_from_file(r->cfg.style_path, &sopts);
    else if(r->cfg.style == NULL && r->cfg.theme != NULL)
        r->style = md_ansi_style_create_named(r->cfg.theme, &sopts);
    else
        r->style = md_ansi_style_create(r->cfg.style,
                                        r->cfg.style ? strlen(r->cfg.style) : 0,
                                        &sopts);
    if(r->style == NULL)
        goto err;

    /* The generic is consumed (strings copied) and not retained; clear the
     * stored copy so fymd_renderer_get_cfg() never hands back a value that
     * points into a builder the caller may since have freed. */
    r->cfg.style_generic = fy_invalid;

    /* Apply post-build style overrides that the YAML loader does not take. */
    if(cfg->flags & FYMD_RF_NO_CODE_HL)
        r->style->code_enabled = 0;
    if(r->cfg.code_theme != NULL) {
        r->style->code_theme = r->cfg.code_theme; /* owned by cfg; freed there */
        r->style->code_enabled = (cfg->flags & FYMD_RF_NO_CODE_HL) ? 0 : 1;
    }
    if(cfg->table_border == FYMD_TB_GRID)
        r->style->table_border_none = 0;
    else if(cfg->table_border == FYMD_TB_NONE)
        r->style->table_border_none = 1;

    /* Map public flags onto MD_ANSI_FLAG_*. */
#ifndef MD4C_USE_ASCII
    rf |= MD_ANSI_FLAG_SKIP_UTF8_BOM;
#endif
    if(cfg->flags & FYMD_RF_NO_COLOR)  rf |= MD_ANSI_FLAG_NO_COLOR;
    if(cfg->flags & FYMD_RF_SHOW_URLS) rf |= MD_ANSI_FLAG_SHOW_URLS;
    if(cfg->flags & FYMD_RF_TABLE_FIT) rf |= MD_ANSI_FLAG_TABLE_FIT_CONTENT;
    if(cfg->flags & FYMD_RF_HEAL)      rf |= MD_ANSI_FLAG_HEAL;
    if(cfg->flags & FYMD_RF_REVERSE)   rf |= MD_ANSI_FLAG_REVERSE;
    if(cfg->sgr_input == FYMD_SGR_KEEP) rf |= MD_ANSI_FLAG_SGR_KEEP;
    else if(cfg->sgr_input == FYMD_SGR_SAFE) rf |= MD_ANSI_FLAG_SGR_SAFE;
    r->renderer_flags = rf;

    r->parser_flags = cfg->parser_flags ? cfg->parser_flags : MD4C_ANSI_PARSER_FLAGS;
    r->width = cfg->width;

    return r;

err:
    fymd_renderer_destroy(r);
    return NULL;
}

void
fymd_renderer_destroy(struct fymd_renderer *r)
{
    if(r == NULL)
        return;
    if(r->stream != NULL)
        md4c_stream_destroy(r->stream);
    if(r->style != NULL)
        md_ansi_style_destroy(r->style);
    free((void *) r->cfg.style);
    free((void *) r->cfg.style_path);
    free((void *) r->cfg.theme);
    free((void *) r->cfg.code_theme);
    free(r->limit_separator);
    fymd_buf_fini(&r->screen);
    fymd_buf_fini(&r->visible);
    fymd_buf_fini(&r->viewport);
    free(r);
}

const struct fymd_renderer_cfg *
fymd_renderer_get_cfg(struct fymd_renderer *r)
{
    return r ? &r->cfg : NULL;
}

int
fymd_renderer_get_style_pair(struct fymd_renderer *r,
                             enum fymd_style_element element,
                             const char **on, const char **off)
{
    const MD_STYLE_PAIR *pair;

    if(r == NULL || r->style == NULL)
        return -1;
    switch(element) {
    case FYMD_STYLE_HEADING:    pair = &r->style->heading; break;
    case FYMD_STYLE_STRONG:     pair = &r->style->strong; break;
    case FYMD_STYLE_BLOCKQUOTE: pair = &r->style->blockquote; break;
    case FYMD_STYLE_RULE:       pair = &r->style->rule; break;
    case FYMD_STYLE_REVERSE:    pair = &r->style->reverse; break;
    case FYMD_STYLE_INDICATOR_PENDING:
        pair = &r->style->indicator_pending; break;
    case FYMD_STYLE_INDICATOR_SUCCESS:
        pair = &r->style->indicator_success; break;
    case FYMD_STYLE_INDICATOR_FAILURE:
        pair = &r->style->indicator_failure; break;
    default: return -1;
    }
    if(on != NULL)
        *on = pair->on;
    if(off != NULL)
        *off = pair->off;
    return 0;
}

int
fymd_renderer_get_indicator(struct fymd_renderer *r,
                            enum fymd_indicator_state state,
                            size_t frame, const char **glyph,
                            const char **on, const char **off,
                            unsigned int *interval_ms)
{
    const MD_STYLE_PAIR* pair;
    const char* marker;

    if(r == NULL || r->style == NULL)
        return -1;
    switch(state) {
    case FYMD_INDICATOR_PENDING:
        pair = &r->style->indicator_pending;
        marker = r->style->indicator_pending_frames[
            frame % r->style->indicator_pending_frame_count];
        break;
    case FYMD_INDICATOR_SUCCESS:
        pair = &r->style->indicator_success;
        marker = r->style->indicator_success_glyph;
        break;
    case FYMD_INDICATOR_FAILURE:
        pair = &r->style->indicator_failure;
        marker = r->style->indicator_failure_glyph;
        break;
    default:
        return -1;
    }
    if(glyph != NULL)
        *glyph = marker;
    if(on != NULL)
        *on = pair->on;
    if(off != NULL)
        *off = pair->off;
    if(interval_ms != NULL)
        *interval_ms = r->style->indicator_interval_ms;
    return 0;
}

int
fymd_renderer_get_reverse_pair(struct fymd_renderer *r,
                               const char **on, const char **off)
{
    return fymd_renderer_get_style_pair(r, FYMD_STYLE_REVERSE, on, off);
}

size_t
fymd_theme_count(void)
{
    return 1 + 2 * md_ansi_theme_count();
}

const char *
fymd_theme_name(size_t index)
{
    size_t theme_index;

    if(index == 0)
        return "default";
    theme_index = (index - 1) / 2;
    return md_ansi_theme_name(theme_index, (index - 1) % 2);
}

int
fymd_renderer_set_theme(struct fymd_renderer *r, const char *name)
{
    MD_ANSI_STYLE_OPTS sopts;
    MD_ANSI_STYLE *style;
    char *theme;

    if(r == NULL || r->stream != NULL)
        return -1;
    if(name == NULL || name[0] == '\0')
        name = "default";
    theme = fymd_strdup(name);
    if(theme == NULL)
        return -1;
    memset(&sopts, 0, sizeof(sopts));
    sopts.background = map_background(r->cfg.background);
    sopts.reverse = -1;
    style = md_ansi_style_create_named(name, &sopts);
    if(style == NULL) {
        free(theme);
        return -1;
    }
    if(r->cfg.flags & FYMD_RF_NO_CODE_HL)
        style->code_enabled = 0;
    if(r->cfg.code_theme != NULL) {
        style->code_theme = r->cfg.code_theme;
        style->code_enabled = (r->cfg.flags & FYMD_RF_NO_CODE_HL) ? 0 : 1;
    }
    if(r->cfg.table_border == FYMD_TB_GRID)
        style->table_border_none = 0;
    else if(r->cfg.table_border == FYMD_TB_NONE)
        style->table_border_none = 1;
    md_ansi_style_destroy(r->style);
    r->style = style;
    free((void *)r->cfg.style);
    free((void *)r->cfg.style_path);
    free((void *)r->cfg.theme);
    r->cfg.style = NULL;
    r->cfg.style_path = NULL;
    r->cfg.theme = theme;
    return 0;
}

int
fymd_renderer_set_line_limit(struct fymd_renderer *r,
        const struct fymd_line_limit_opts *opts)
{
    static const char default_separator[] = "... %d lines omitted ...";
    struct fymd_line_limit_opts next;
    char *separator = NULL;

    if(r == NULL || r->stream != NULL)
        return -1;
    memset(&next, 0, sizeof(next));
    if(opts == NULL || opts->mode == FYMD_LLM_NONE || opts->max_lines == 0)
        goto apply;
    next = *opts;
    if(next.mode != FYMD_LLM_SCROLL && next.mode != FYMD_LLM_HEAD_TAIL)
        return -1;
    if(next.mode == FYMD_LLM_HEAD_TAIL) {
        const char *fmt = next.separator_format ? next.separator_format : default_separator;
        if(next.max_lines < 3 ||
           (next.split != FYMD_LLS_HEAD_COUNT && next.split != FYMD_LLS_BALANCED) ||
           (next.split == FYMD_LLS_HEAD_COUNT &&
            (next.head_lines == 0 || next.head_lines > next.max_lines - 2)) ||
           !fymd_separator_valid(fmt))
            return -1;
        separator = fymd_strdup(fmt);
        if(separator == NULL)
            return -1;
        next.separator_format = separator;
    } else {
        next.split = FYMD_LLS_HEAD_COUNT;
        next.head_lines = 0;
        next.separator_format = NULL;
    }

apply:
    free(r->limit_separator);
    r->limit_separator = separator;
    r->limit = next;
    r->limit.separator_format = separator;
    fymd_buf_reset(&r->screen);
    fymd_buf_reset(&r->visible);
    fymd_buf_reset(&r->viewport);
    r->stream_active_rows = 0;
    return 0;
}

static int
fymd_render_(struct fymd_renderer *r, const char *md, size_t len,
             fymd_margin_fn margin_fn, void *margin_userdata,
             char **out, size_t *out_len)
{
    struct fymd_buf b;
    int rc;

    if(r == NULL || out == NULL)
        return -1;

    memset(&b, 0, sizeof(b));
    rc = md_ansi_ex_styled_margins(md, (MD_SIZE) len, fymd_buf_append, &b,
                           r->parser_flags, r->renderer_flags, r->width, r->style,
                           margin_fn, margin_userdata);
    if(rc != 0 || b.oom) {
        free(b.data);
        return -1;
    }
    if(r->limit.mode != FYMD_LLM_NONE && r->limit.max_lines > 0) {
        struct fymd_buf limited;
        memset(&limited, 0, sizeof(limited));
        if(fymd_project(r, b.data, b.size, &limited) != 0) {
            free(b.data);
            free(limited.data);
            return -1;
        }
        free(b.data);
        b = limited;
    }
    if(b.data == NULL) {
        /* Empty document: still hand back a valid empty string. */
        b.data = malloc(1);
        if(b.data == NULL)
            return -1;
        b.asize = 1;
    }
    b.data[b.size] = '\0';
    *out = b.data;
    if(out_len != NULL)
        *out_len = b.size;
    return 0;
}

int
fymd_render(struct fymd_renderer *r, const char *md, size_t len,
            char **out, size_t *out_len)
{
    return fymd_render_(r, md, len, NULL, NULL, out, out_len);
}

int
fymd_render_with_margins(struct fymd_renderer *r,
        const char *md, size_t len, fymd_margin_fn margin_fn,
        void *margin_userdata, char **out, size_t *out_len)
{
    return fymd_render_(r, md, len, margin_fn, margin_userdata, out, out_len);
}

char *
fymd_render_to_string(struct fymd_renderer *r, const char *md, size_t len)
{
    char *out = NULL;
    if(fymd_render(r, md, len, &out, NULL) != 0)
        return NULL;
    return out;
}

int
fymd_render_fenced_block(struct fymd_renderer *r,
        const char *text, size_t len,
        const struct fymd_fenced_block_opts *opts,
        char **out, size_t *out_len)
{
    struct fymd_fenced_block_opts defaults;
    struct fymd_buf b, limited;
    size_t lines, plain_lines, hidden_lines;
    unsigned ff = 0;
    int rc;

    if(r == NULL || out == NULL || (text == NULL && len > 0))
        return -1;
    if(opts == NULL) {
        memset(&defaults, 0, sizeof(defaults));
        defaults.flags = FYMD_FBF_DEFAULT;
        opts = &defaults;
    }
    if(opts->flags & ~(unsigned)FYMD_FBF_DEFAULT)
        return -1;
    if(opts->flags & FYMD_FBF_STYLE)
        ff |= MD_ANSI_FENCE_STYLE;
    if(opts->flags & FYMD_FBF_HIGHLIGHT)
        ff |= MD_ANSI_FENCE_HIGHLIGHT;

    plain_lines = fymd_count_rows(text, len);
    lines = plain_lines;
    if(opts->flags & FYMD_FBF_STYLE) {
        if(r->style->code_header != NULL && r->style->code_header[0] != '\0')
            lines++;
        if(r->style->code_footer != NULL && r->style->code_footer[0] != '\0')
            lines++;
    }
    hidden_lines = r->limit.mode != FYMD_LLM_NONE && r->limit.max_lines > 0 &&
                   lines > r->limit.max_lines ? lines - r->limit.max_lines : 0;

    memset(&b, 0, sizeof(b));
    rc = md_ansi_fenced_styled(text, (MD_SIZE) len, opts->language,
                                opts->template_vars, lines, plain_lines,
                                hidden_lines, ff,
                                fymd_buf_append, &b, r->renderer_flags,
                                r->width, r->style);
    if(rc != 0 || b.oom) {
        free(b.data);
        return -1;
    }
    if(r->limit.mode != FYMD_LLM_NONE && r->limit.max_lines > 0) {
        memset(&limited, 0, sizeof(limited));
        if(fymd_project(r, b.data, b.size, &limited) != 0) {
            free(b.data);
            free(limited.data);
            return -1;
        }
        free(b.data);
        b = limited;
    }
    return fymd_buf_finish(&b, out, out_len);
}

/* Build stream options from the renderer's resolved config. */
static void
fymd_fill_stream_opts(struct fymd_renderer *r, MD4C_STREAM_OPTS *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->parser_flags = r->parser_flags;
    /* The stream renders its own active region; DEBUG/HEAL bits are handled
     * separately, so pass only the display-affecting renderer flags. */
    opts->renderer_flags = r->renderer_flags & ~(unsigned) MD_ANSI_FLAG_HEAL;
    opts->width = r->width;
    opts->heal = (r->cfg.flags & FYMD_RF_HEAL) ? 1 : 0;
    opts->max_active_lines = r->cfg.max_active_lines;
    opts->style = r->style;
}

int
fymd_render_push(struct fymd_renderer *r, const char *chunk, size_t len,
                 struct fymd_update *upd)
{
    MD4C_STREAM_UPDATE u;
    size_t common;
    struct fymd_buf swap;

    if(r == NULL || upd == NULL)
        return -1;

    if(r->stream == NULL) {
        MD4C_STREAM_OPTS opts;
        fymd_fill_stream_opts(r, &opts);
        r->stream = md4c_stream_create(&opts);
        if(r->stream == NULL)
            return -1;
    }

    if(md4c_stream_render(r->stream, chunk, len, &u) != 0)
        return -1;

    if(r->limit.mode != FYMD_LLM_NONE && r->limit.max_lines > 0) {
        fymd_drop_trailing_rows(&r->screen, u.backtrack);
        if(fymd_buf_put(&r->screen, u.content, u.content_len) != 0 ||
           fymd_project(r, r->screen.data, r->screen.size, &r->viewport) != 0)
            return -1;

        common = fymd_common_row_prefix(&r->visible, &r->viewport);
        upd->backtrack = fymd_count_rows(r->visible.data + common,
                                         r->visible.size - common);

        swap = r->visible;
        r->visible = r->viewport;
        r->viewport = swap;
        upd->content = r->visible.data ? r->visible.data + common : "";
        upd->content_len = r->visible.size - common;
        upd->freeze = 0;

        r->stream_active_rows -= u.backtrack > r->stream_active_rows ?
                                 r->stream_active_rows : u.backtrack;
        r->stream_active_rows += fymd_count_rows(u.content, u.content_len);
        r->stream_active_rows = u.freeze >= r->stream_active_rows ? 0 :
                                r->stream_active_rows - u.freeze;
        return 0;
    }

    upd->backtrack = u.backtrack;
    upd->content = u.content;
    upd->content_len = u.content_len;
    upd->freeze = u.freeze;
    return 0;
}

int
fymd_render_finish(struct fymd_renderer *r, const char **out, size_t *out_len)
{
    const char *o = NULL;
    size_t olen = 0;

    if(r == NULL || out == NULL)
        return -1;

    if(r->stream == NULL) {
        /* Nothing was pushed: empty flush. */
        *out = "";
        if(out_len != NULL)
            *out_len = 0;
        return 0;
    }

    if(md4c_stream_finish(r->stream, &o, &olen) != 0)
        return -1;

    if(r->limit.mode != FYMD_LLM_NONE && r->limit.max_lines > 0) {
        fymd_drop_trailing_rows(&r->screen, r->stream_active_rows);
        if(fymd_buf_put(&r->screen, o, olen) != 0 ||
           fymd_project(r, r->screen.data, r->screen.size, &r->viewport) != 0)
            return -1;
        o = r->viewport.data ? r->viewport.data : "";
        olen = r->viewport.size;
    }

    /* Keep the stream alive: `o` points into its memory, valid until the next
     * call. fymd_render_reset()/destroy tears it down. */
    *out = o;
    if(out_len != NULL)
        *out_len = olen;
    return 0;
}

void
fymd_render_reset(struct fymd_renderer *r)
{
    if(r == NULL || r->stream == NULL)
        return;
    md4c_stream_destroy(r->stream);
    r->stream = NULL;
    fymd_buf_reset(&r->screen);
    fymd_buf_reset(&r->visible);
    fymd_buf_reset(&r->viewport);
    r->stream_active_rows = 0;
}

void
fymd_free(void *p)
{
    free(p);
}

/* Finalize a fymd_buf into a heap NUL-terminated string handed to the caller.
 * Returns 0 on success (*out owned by caller), -1 on error (buffer freed). */
static int
fymd_buf_finish(struct fymd_buf *b, char **out, size_t *out_len)
{
    if(b->oom) {
        free(b->data);
        return -1;
    }
    if(b->data == NULL) {
        b->data = malloc(1);
        if(b->data == NULL)
            return -1;
        b->asize = 1;
    }
    b->data[b->size] = '\0';
    *out = b->data;
    if(out_len != NULL)
        *out_len = b->size;
    return 0;
}

int
fymd_heal(const char *md, size_t len, char **out, size_t *out_len)
{
    struct fymd_buf b;

    if(out == NULL)
        return -1;
    memset(&b, 0, sizeof(b));
    if(md_heal(md, (unsigned) len, fymd_buf_append, &b) != 0) {
        free(b.data);
        return -1;
    }
    return fymd_buf_finish(&b, out, out_len);
}

char *
fymd_heal_to_string(const char *md, size_t len)
{
    char *out = NULL;
    if(fymd_heal(md, len, &out, NULL) != 0)
        return NULL;
    return out;
}

int
fymd_render_html(const char *md, size_t len, unsigned parser_flags,
                 unsigned flags, char **out, size_t *out_len)
{
    struct fymd_buf b;
    unsigned hf = 0;

    if(out == NULL)
        return -1;
    if(flags & FYMD_HTML_XHTML)             hf |= MD_HTML_FLAG_XHTML;
    if(flags & FYMD_HTML_VERBATIM_ENTITIES) hf |= MD_HTML_FLAG_VERBATIM_ENTITIES;
    if(flags & FYMD_HTML_SKIP_UTF8_BOM)     hf |= MD_HTML_FLAG_SKIP_UTF8_BOM;

    memset(&b, 0, sizeof(b));
    if(md_html(md, (MD_SIZE) len, fymd_buf_append, &b, parser_flags, hf) != 0) {
        free(b.data);
        return -1;
    }
    return fymd_buf_finish(&b, out, out_len);
}

char *
fymd_render_html_to_string(const char *md, size_t len, unsigned parser_flags,
                           unsigned flags)
{
    char *out = NULL;
    if(fymd_render_html(md, len, parser_flags, flags, &out, NULL) != 0)
        return NULL;
    return out;
}

int
fymd_detect_width(void)
{
    return md_ansi_detect_width();
}

const char *
fymd_library_version(void)
{
    return FYMD_VERSION_STRING;
}

char *
fymd_detect_language_for_path(const char *path)
{
    if(path == NULL)
        return NULL;
#ifdef MD4C_WITH_FYTS
    return fyts_detect_language_for_path(path);
#else
    return NULL;
#endif
}
