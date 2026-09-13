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
    "params:\n"
    "  gutter.cols: 3\n"
    "glyphs:\n"
    "  md:\n"
    "    bullet: {utf: \"\u2022\", ascii: \"+\"}\n"
    "    task:\n"
    "      done: {utf: \"\u2714\", ascii: \"v\"}\n"
    "      open: {utf: \"\u2610\", ascii: \"o\"}\n"
    "  tool:\n"
    "    pending: {utf: \"\u2192\", ascii: \"->\", 1: \" \"}\n"
    "    ok: \"\u2192\"\n"
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
    "- one\n"
    "- [x] done\n"
    "- [ ] open\n"
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

static size_t
rule_rows(const char *out)
{
    size_t rows = 0;
    const char *p = out, *end, *rule;
    while(p && *p) {
        end = strchr(p, '\n');
        rule = strstr(p, "\u2500");
        if(rule && (!end || rule < end))
            rows++;
        p = end ? end + 1 : NULL;
    }
    return rows;
}

static void
test_code_rules(void)
{
    static const struct { const char *mode; size_t rows; } modes[] = {
        { "none", 0 }, { "top", 1 }, { "both", 2 }, { "top-bottom", 2 }
    };
    static const char input[] = "```c\nint x;\n```\n\nafter\n";
    struct fypal_caps caps = { .depth = FYPAL_DEPTH_TRUECOLOR, .attrs = FYPAL_ATTR_ALL };
    struct fypal_ctx *palette = fypal_ctx_create(&caps);
    struct fymd_renderer *r = renderer(FYMD_RF_NO_COLOR);
    struct fymd_fenced_block_opts opts = { .language = "c", .flags = FYMD_FBF_DEFAULT };
    char *out = NULL;
    const char *p;
    size_t i, len;

    CHECK(palette && r);
    if(!palette || !r)
        goto out;
    CHECK(fypal_ctx_load(palette,
          "colors: {rule: '#123456', faint: '#abcdef'}\n", "rules") == 0);
    for(i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        CHECK(fypal_ctx_set_param_string(palette, "md.code.rules",
                                        FYPAL_SECTION_ALL, modes[i].mode) == 0);
        CHECK(fymd_renderer_set_palette(r, palette) == 0);
        CHECK(fymd_render(r, input, strlen(input), &out, &len) == 0);
        CHECK(out && rule_rows(out) == modes[i].rows);
        expect(__LINE__, out, "int x;", NULL);
        fymd_free(out);
        out = NULL;
        CHECK(fymd_render_fenced_block(r, "int x;\n", 7, &opts, &out, &len) == 0);
        CHECK(out && rule_rows(out) == modes[i].rows);
        fymd_free(out);
        out = NULL;
    }
    CHECK(fypal_ctx_set_param_string(palette, "md.code.rules", FYPAL_SECTION_ALL,
                                    "bubble-rule-faint") == 0);
    CHECK(fymd_renderer_set_palette(r, palette) == 0);
    CHECK(fymd_render_fenced_block(r, "int x;\n", 7, &opts, &out, &len) == 0);
    expect(__LINE__, out, "int x;", "\033");
    expect(__LINE__, out, "  \u2500\u2500 C \u2500\u2500", NULL);
    CHECK(out && rule_rows(out) == 1);
    /* Label, blank and body rows are padded to the prose right edge. */
    p = out ? strchr(out, '\n') : NULL;
    CHECK(p && p - out == 86 && len == 245);
    CHECK(p && strspn(p + 1, " ") == 78 && p[79] == '\n');
    fymd_free(out);
    out = NULL;
    opts.flags = 0;
    CHECK(fymd_render_fenced_block(r, "int x;\n", 7, &opts, &out, &len) == 0);
    CHECK(out && !strcmp(out, "int x;\n"));
    fymd_free(out);
    out = NULL;
    fymd_renderer_destroy(r);
    r = renderer(0);
    CHECK(r && fymd_renderer_set_palette(r, palette) == 0);
    CHECK(fymd_render(r, input, strlen(input), &out, &len) == 0);
    expect(__LINE__, out, "48;2;18;52;86m", NULL);
    expect(__LINE__, out, "38;2;171;205;239m", NULL);
    expect(__LINE__, out, "\u2500\u2500 C \u2500\u2500\033[39m", NULL);
    expect(__LINE__, out, "\033[49m\n", NULL);
    p = out ? strstr(out, "after") : NULL;
    CHECK(p && strstr(p, "48;2;18;52;86m") == NULL);
    fymd_free(out);
    out = NULL;
    /* Invalid modes and unresolved colours fail instead of silently changing layout. */
    CHECK(fypal_ctx_set_param_string(palette, "md.code.rules", FYPAL_SECTION_ALL,
                                    "bubble-missing-faint") == 0);
    CHECK(fymd_renderer_set_palette(r, palette) == -1);
    CHECK(fypal_ctx_set_param_string(palette, "md.code.rules", FYPAL_SECTION_ALL,
                                    "sideways") == 0);
    CHECK(fymd_renderer_set_palette(r, palette) == -1);
    CHECK(fymd_renderer_set_palette(r, NULL) == 0);
    CHECK(fymd_render(r, input, strlen(input), &out, &len) == 0);
    CHECK(out && rule_rows(out) == 2);
    expect(__LINE__, out, NULL, "48;2;18;52;86m");
out:
    fymd_free(out);
    fymd_renderer_destroy(r);
    fypal_ctx_destroy(palette);
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
    struct fypal_ctx *norules;
    struct fymd_renderer *r;
    const char *on, *off, *fin, *glyph;
    char *out;
    size_t len;

    test_code_rules();

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
    /* the palette gives the glyphs and the document margin */
    expect(__LINE__, out, "\n   Use", NULL);
    expect(__LINE__, out, "\u2022 ", "* ");
    expect(__LINE__, out, "\u2714\033", "[x]");
    expect(__LINE__, out, "\u2610 open", "[ ]");
    fymd_free(out);

    /* the pending indicator blinks through the frames of the palette */
    CHECK(fymd_renderer_get_indicator(r, FYMD_INDICATOR_PENDING, 0, &glyph,
                                      NULL, NULL, NULL) == 0);
    expect(__LINE__, glyph, "\u2192", NULL);
    CHECK(fymd_renderer_get_indicator(r, FYMD_INDICATOR_PENDING, 1, &glyph,
                                      NULL, NULL, NULL) == 0);
    CHECK(glyph != NULL && !strcmp(glyph, " "));
    CHECK(fymd_renderer_get_indicator(r, FYMD_INDICATOR_PENDING, 2, &glyph,
                                      NULL, NULL, NULL) == 0);
    expect(__LINE__, glyph, "\u2192", NULL);
    CHECK(fymd_renderer_get_indicator(r, FYMD_INDICATOR_SUCCESS, 0, &glyph,
                                      NULL, NULL, NULL) == 0);
    expect(__LINE__, glyph, "\u2192", NULL);

    /* the ASCII form */
    CHECK(fymd_renderer_set_palette_flags(r, palette, FYMD_PF_ASCII) == 0);
    out = render(r);
    expect(__LINE__, out, "+ ", "\u2022");
    expect(__LINE__, out, "o open", "\u2610");
    fymd_free(out);
    CHECK(fymd_renderer_get_indicator(r, FYMD_INDICATOR_PENDING, 0, &glyph,
                                      NULL, NULL, NULL) == 0);
    CHECK(glyph != NULL && !strcmp(glyph, "->"));
    CHECK(fymd_renderer_set_palette(r, palette) == 0);

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
    /* the theme glyphs and margin return too */
    expect(__LINE__, out, "\n  Use", "\u2022");
    expect(__LINE__, out, "[ ] open", "\n   Use");
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

    /* a theme draws fenced blocks with their rules unless it turns them off */
    r = renderer(FYMD_RF_NO_COLOR);
    CHECK(r != NULL && fymd_renderer_set_palette(r, palette) == 0);
    out = r ? render(r) : NULL;
    expect(__LINE__, out, "\u2500\u2500 c ", NULL);
    fymd_free(out);
    norules = fypal_ctx_create(&caps);
    CHECK(norules != NULL &&
          fypal_ctx_load(norules, "params:\n  md.code.rules: 0\n", "test") == 0);
    CHECK(r != NULL && fymd_renderer_set_palette(r, norules) == 0);
    out = r ? render(r) : NULL;
    expect(__LINE__, out, "int f(void)", "\u2500\u2500 c ");
    expect(__LINE__, out, "@@ -1 +1 @@", "\u2500\u2500 diff");
    fymd_free(out);
    /* without the palette the theme rules return */
    CHECK(r != NULL && fymd_renderer_set_palette(r, NULL) == 0);
    out = r ? render(r) : NULL;
    expect(__LINE__, out, "\u2500\u2500 c ", NULL);
    fymd_free(out);
    fymd_renderer_destroy(r);
    fypal_ctx_destroy(norules);

    fypal_ctx_destroy(palette);
    return failures ? 1 : 0;
}
