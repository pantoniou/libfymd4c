/*
 * libfymd4c-renderer.h - opaque Markdown -> ANSI renderer
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

#ifndef LIBFYMD4C_RENDERER_H
#define LIBFYMD4C_RENDERER_H

#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

#include "libfymd4c-util.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libfymd4c renders Markdown into ANSI terminal output. A single opaque
 * fymd_renderer handle, built from a fymd_renderer_cfg, drives both:
 *
 *   - one-shot rendering of a whole document (fymd_render /
 *     fymd_render_to_string), and
 *   - progressive, self-healing streaming of a document that arrives in
 *     chunks (fymd_render_push / fymd_render_finish), suitable for live
 *     terminal output of an LLM's token stream.
 *
 * Fenced code blocks are syntax-highlighted (tree-sitter, via libfyts) when
 * the build supports it. Styling (colors, glyphs, light/dark background) is
 * driven by a YAML config carried in the cfg; pass NULL for the built-in
 * default theme.
 */

/* Opaque renderer handle. */
struct fymd_renderer;

/* Document background, for themes that ship light/dark variants. AUTO consults
 * $COLORFGBG and falls back to dark. */
enum fymd_background {
    FYMD_BG_AUTO = 0,
    FYMD_BG_DARK,
    FYMD_BG_LIGHT
};

/* Renderer behaviour flags (bitmask). */
enum fymd_cfg_flags {
    FYMD_RF_NO_COLOR     = FYMD_BIT(0), /* emit no SGR color sequences */
    FYMD_RF_SHOW_URLS    = FYMD_BIT(1), /* show link targets inline */
    FYMD_RF_TABLE_FIT    = FYMD_BIT(2), /* size tables to content, not to width */
    FYMD_RF_HEAL         = FYMD_BIT(3), /* close dangling markers in the active tail */
    FYMD_RF_CODE_REVERSE = FYMD_BIT(4), /* reverse "bubble" fenced code blocks */
    FYMD_RF_NO_CODE_HL   = FYMD_BIT(5)  /* disable fenced-code syntax highlighting */
};

/* Sensible default flags: heal the in-progress tail. */
#define FYMD_RF_DEFAULT (FYMD_RF_HEAL)

/* Policy for ANSI escape sequences embedded in the input text (as opposed to
 * the SGR the renderer emits itself). Untrusted Markdown may smuggle escapes
 * that move the cursor or clear the screen, so the default is to strip them. */
enum fymd_sgr_input {
    FYMD_SGR_STRIP = 0, /* default: remove input escape sequences entirely */
    FYMD_SGR_KEEP,      /* pass input escape sequences through unchanged */
    FYMD_SGR_SAFE       /* keep only SGR (colour/attribute) sequences; strip the
                           rest (cursor moves, screen clears, OSC, ...) */
};

/* Layout width sentinels (match the columns argument otherwise). */
#define FYMD_WIDTH_AUTO (-1) /* detect from $COLUMNS / terminal, else 80 */
#define FYMD_WIDTH_INF  0    /* unlimited: size tables to content, no wrap */

/* Renderer configuration. Pass NULL to fymd_renderer_create() for all
 * defaults. String fields are copied; the caller need not keep them alive. */
struct fymd_renderer_cfg {
    /* Styling source, highest precedence first. style_generic wins over
     * style_path, which wins over style; if none is set the built-in default
     * theme is used. */
    fy_generic style_generic;   /* a parsed libfyaml mapping (config schema), or
                                   leave zero-initialized / fy_invalid to skip */
    const char *style;          /* inline YAML config, NUL-terminated, or NULL */
    const char *style_path;     /* YAML config file path (overrides `style`) */
    enum fymd_cfg_flags flags;  /* FYMD_RF_* bitmask */
    int width;                  /* FYMD_WIDTH_AUTO / FYMD_WIDTH_INF / columns */
    int max_active_lines;       /* >0: cap the progressive active region (rows) */
    unsigned parser_flags;      /* md4c MD_FLAG_*; 0 => renderer's default set */
    enum fymd_background background;
    enum fymd_sgr_input sgr_input; /* input-escape policy; default FYMD_SGR_STRIP */
    const char *code_theme;     /* libfyts styling name/path; NULL => theme default */
    void *userdata;             /* opaque, propagated to fymd_renderer_get_cfg() */
};

/* Progressive line-diff update produced by fymd_render_push().
 *
 * Apply to a virtual terminal as: move the cursor up `backtrack` lines, clear
 * to end of screen, then print `content`. `freeze` is informational: that many
 * lines at the top of the active region are now permanent. `content` points
 * into renderer-owned memory, valid only until the next call on the renderer. */
struct fymd_update {
    size_t backtrack;
    const char *content;
    size_t content_len;
    size_t freeze;
};

/* Create a renderer. Pass NULL cfg for all defaults. Returns NULL on bad
 * config (e.g. unparseable style) or allocation failure. */
struct fymd_renderer *fymd_renderer_create(const struct fymd_renderer_cfg *cfg) FYMD_EXPORT;

/* Destroy a renderer and free everything it owns (including any live stream). */
void fymd_renderer_destroy(struct fymd_renderer *r) FYMD_EXPORT;

/* The configuration the renderer was created with (string fields are the
 * renderer's owned copies). Returns NULL if r is NULL. */
const struct fymd_renderer_cfg *fymd_renderer_get_cfg(struct fymd_renderer *r) FYMD_EXPORT;

/* One-shot render of a whole Markdown document. On success *out receives a
 * heap-allocated, NUL-terminated ANSI string (free with fymd_free()) and
 * *out_len its length (excluding the NUL). Returns 0 on success, -1 on error. */
int fymd_render(struct fymd_renderer *r, const char *md, size_t len,
                char **out, size_t *out_len) FYMD_EXPORT;

/* Convenience wrapper around fymd_render(): returns a heap-allocated,
 * NUL-terminated string (free with fymd_free()), or NULL on error. */
char *fymd_render_to_string(struct fymd_renderer *r, const char *md, size_t len) FYMD_EXPORT;

/* Feed the next chunk of a streamed document and get a progressive update for
 * the active region. The renderer holds one live stream; the first push starts
 * it. Returns 0 on success, -1 on error. */
int fymd_render_push(struct fymd_renderer *r, const char *chunk, size_t len,
                     struct fymd_update *upd) FYMD_EXPORT;

/* Final flush of the live stream: *out / *out_len receive the output following
 * the last committed prefix (healed per FYMD_RF_HEAL). The pointer is
 * renderer-owned and valid until the next call on the renderer. Call
 * fymd_render_reset() before starting another stream. Returns 0 / -1. */
int fymd_render_finish(struct fymd_renderer *r, const char **out, size_t *out_len) FYMD_EXPORT;

/* Drop any live stream state so the renderer can start a fresh stream (or be
 * reused for one-shot rendering). Keeps the style/config. */
void fymd_render_reset(struct fymd_renderer *r) FYMD_EXPORT;

/* Free a buffer returned by fymd_render() / fymd_render_to_string(). */
void fymd_free(void *p) FYMD_EXPORT;

/* Resolve the auto layout width (from $COLUMNS, then the terminal, else 80). */
int fymd_detect_width(void) FYMD_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* LIBFYMD4C_RENDERER_H */
