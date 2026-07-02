/*
 * libfymd4c.c - public facade over the md4c ANSI renderer, styling and
 * streaming modules.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
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

struct fymd_renderer {
    struct fymd_renderer_cfg cfg;   /* owned copy (string fields strdup'd) */
    MD_ANSI_STYLE *style;           /* built once; freed in destroy */
    unsigned parser_flags;          /* resolved (cfg 0 => default set) */
    unsigned renderer_flags;        /* resolved MD_ANSI_FLAG_* for md_ansi */
    int width;                      /* MD_ANSI_WIDTH_* / columns */
    MD4C_STREAM *stream;            /* lazily created live stream, or NULL */
};

/* Growable output buffer for one-shot rendering. */
struct fymd_buf {
    char *data;
    size_t size;
    size_t asize;
    int oom;
};

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
    r->cfg.code_theme = fymd_strdup(cfg->code_theme);
    if((cfg->style && !r->cfg.style) ||
       (cfg->style_path && !r->cfg.style_path) ||
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
    free((void *) r->cfg.code_theme);
    free(r);
}

const struct fymd_renderer_cfg *
fymd_renderer_get_cfg(struct fymd_renderer *r)
{
    return r ? &r->cfg : NULL;
}

int
fymd_render(struct fymd_renderer *r, const char *md, size_t len,
            char **out, size_t *out_len)
{
    struct fymd_buf b;
    int rc;

    if(r == NULL || out == NULL)
        return -1;

    memset(&b, 0, sizeof(b));
    rc = md_ansi_ex_styled(md, (MD_SIZE) len, fymd_buf_append, &b,
                           r->parser_flags, r->renderer_flags, r->width, r->style);
    if(rc != 0 || b.oom) {
        free(b.data);
        return -1;
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

char *
fymd_render_to_string(struct fymd_renderer *r, const char *md, size_t len)
{
    char *out = NULL;
    if(fymd_render(r, md, len, &out, NULL) != 0)
        return NULL;
    return out;
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
