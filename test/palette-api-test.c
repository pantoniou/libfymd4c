/*
 * palette-api-test.c - fymd_renderer_set_palette(): the document takes the
 * colours of a libfypalette context, and the theme returns without it.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>
#include <libfypalette.h>

/* Every role has a colour of its own, so the output says which role styled
 * an element. */
static const char theme[] =
    "colors:\n"
    "  head: '#0a0a0a'\n"
    "  sub: '#111111'\n"
    "  code: '#0c0c0c'\n"
    "  link: '#0d0d0d'\n"
    "  line: '#0e0e0e'\n"
    "  kw: '#0f0f0f'\n"
    "  add: '#101010'\n"
    "roles:\n"
    "  md:\n"
    "    heading:\n"
    "      fg: head\n"
    "      attrs: [bold]\n"
    "      2: {fg: sub}\n"
    "    code: {fg: code}\n"
    "    link: {fg: link, ul: line, attrs: [underline]}\n"
    "  code:\n"
    "    keyword: {fg: kw}\n"
    "  diff:\n"
    "    add: {bg: add}\n";

static const char doc[] =
    "# Plan\n"
    "\n"
    "## Steps\n"
    "\n"
    "Use `peek` and [the notes](http://example.com).\n"
    "\n"
    "```c\n"
    "int f(void) { return 1; }\n"
    "```\n"
    "\n"
    "```diff\n"
    "@@ -1 +1 @@\n"
    "-old\n"
    "+new\n"
    "```\n";

static int failures;

/* Report an output that lacks want or holds forbid, escapes made visible. */
static void
expect(int line, const char *out, const char *want, const char *forbid)
{
    const char *p;

    if(out != NULL && (want == NULL || strstr(out, want) != NULL) &&
       (forbid == NULL || strstr(out, forbid) == NULL))
        return;
    fprintf(stderr, "%s:%d: output lacks \"%s\" or holds \"%s\":\n", __FILE__,
            line, want ? want : "", forbid ? forbid : "");
    for(p = out ? out : ""; *p; p++) {
        if(*p == '\033')
            fputs("\\e", stderr);
        else
            fputc(*p, stderr);
    }
    fputc('\n', stderr);
    failures++;
}

#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #cond);                                       \
            failures++;                                                     \
        }                                                                   \
    } while(0)

static char *
render(struct fymd_renderer *r)
{
    char *out = NULL;
    size_t len = 0;

    if(fymd_render(r, doc, strlen(doc), &out, &len) != 0)
        return NULL;
    return out;
}

static struct fymd_renderer *
renderer(unsigned flags)
{
    struct fymd_renderer_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_DEFAULT | flags;
    cfg.width = 80;
    cfg.background = FYMD_BG_DARK;
    return fymd_renderer_create(&cfg);
}

int
main(void)
{
    struct fypal_caps caps = {
        .depth = FYPAL_DEPTH_TRUECOLOR,
        .attrs = FYPAL_ATTR_ALL,
        .underline_color = 1,
    };
    struct fymd_update upd;
    struct fypal_ctx *palette;
    struct fymd_renderer *r;
    const char *on, *off, *fin;
    char *out;
    size_t len;

    palette = fypal_ctx_create(&caps);
    if(palette == NULL || fypal_ctx_load(palette, theme, "test") != 0) {
        fprintf(stderr, "theme: %s\n",
                palette ? fypal_ctx_error(palette) : "no context");
        return 1;
    }

    r = renderer(0);
    CHECK(r != NULL);
    if(r == NULL)
        return 1;

    /* without a palette the theme styles the document */
    out = render(r);
    expect(__LINE__, out, "Plan", "38;2;10;10;10");
    fymd_free(out);

    CHECK(fymd_renderer_set_palette(r, palette) == 0);
    out = render(r);
    expect(__LINE__, out, "38;2;10;10;10mPlan", NULL);
    /* a level takes its own role and keeps what it inherits */
    expect(__LINE__, out, "\033[1;38;2;17;17;17mSteps", NULL);
    expect(__LINE__, out, "38;2;12;12;12", NULL);
    expect(__LINE__, out, "38;2;13;13;13", NULL);
    /* the underline colour survives the renderer's escape handling */
    expect(__LINE__, out, "58;2;14;14;14", NULL);
    expect(__LINE__, out, "48;2;16;16;16", NULL);
#ifdef FYMD_TEST_CODE_PALETTE
    /* fenced code takes the code.* roles through libfyts */
    expect(__LINE__, out, "38;2;15;15;15", NULL);
#endif
    fymd_free(out);

    /* an application that draws its own chrome gets the palette pairs */
    CHECK(fymd_renderer_get_style_pair(r, FYMD_STYLE_HEADING, &on, &off) == 0);
    expect(__LINE__, on, "38;2;10;10;10", NULL);
    expect(__LINE__, off, "22", NULL);

    /* a new theme keeps the palette */
    CHECK(fymd_renderer_set_theme(r, "default") == 0);
    out = render(r);
    expect(__LINE__, out, "38;2;10;10;10", NULL);
    fymd_free(out);

    /* a palette with other capabilities is applied again */
    caps.depth = FYPAL_DEPTH_256;
    fypal_ctx_set_caps(palette, &caps);
    CHECK(fymd_renderer_set_palette(r, palette) == 0);
    out = render(r);
    expect(__LINE__, out, "38;5;", "38;2;10;10;10");
    fymd_free(out);
    caps.depth = FYPAL_DEPTH_TRUECOLOR;
    fypal_ctx_set_caps(palette, &caps);
    CHECK(fymd_renderer_set_palette(r, palette) == 0);

    /* a progressive stream renders with the palette, and holds it until the
     * stream is reset */
    memset(&upd, 0, sizeof(upd));
    CHECK(fymd_render_push(r, doc, strlen(doc), &upd) == 0);
    CHECK(fymd_renderer_set_palette(r, NULL) == -1);
    fin = NULL;
    CHECK(fymd_render_finish(r, &fin, &len) == 0);
    CHECK(fymd_renderer_set_palette(r, NULL) == -1);
    fymd_render_reset(r);

    /* NULL returns to the theme */
    CHECK(fymd_renderer_set_palette(r, NULL) == 0);
    out = render(r);
    expect(__LINE__, out, "Plan", "38;2;10;10;10");
    expect(__LINE__, out, NULL, "38;2;12;12;12");
    fymd_free(out);
    CHECK(fymd_renderer_get_style_pair(r, FYMD_STYLE_HEADING, &on, &off) == 0);
    expect(__LINE__, on, NULL, "38;2;10;10;10");
    fymd_renderer_destroy(r);

    /* without colour the palette draws nothing either */
    r = renderer(FYMD_RF_NO_COLOR);
    CHECK(r != NULL && fymd_renderer_set_palette(r, palette) == 0);
    out = r ? render(r) : NULL;
    expect(__LINE__, out, "Plan", "38;2;");
    fymd_free(out);
    fymd_renderer_destroy(r);

    CHECK(fymd_renderer_set_palette(NULL, palette) == -1);

    fypal_ctx_destroy(palette);
    return failures ? 1 : 0;
}
