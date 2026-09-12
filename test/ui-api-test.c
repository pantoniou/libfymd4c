/*
 * ui-api-test.c - UI Markdown: fills, clickable regions, roles, glyphs,
 * columns and the vertical layout.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>
#ifdef FYMD_TEST_PALETTE
#include <libfypalette.h>
#endif

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #cond);                                       \
            failures++;                                                     \
        }                                                                   \
    } while(0)

static void
dump(const char *what, const char *out)
{
    const char *p;

    fprintf(stderr, "--- %s\n", what);
    for(p = out ? out : ""; *p; p++) {
        if(*p == '\033')
            fputs("\\e", stderr);
        else
            fputc(*p, stderr);
    }
    fprintf(stderr, "---\n");
}

static struct fymd_renderer *
renderer(int ui, int width, int height)
{
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_DEFAULT | FYMD_RF_NO_COLOR | (ui ? FYMD_RF_UI : 0);
    cfg.width = width;
    cfg.background = FYMD_BG_DARK;
    r = fymd_renderer_create(&cfg);
    if(r != NULL)
        fymd_renderer_set_height(r, height);
    return r;
}

static char *
render(struct fymd_renderer *r, const char *md)
{
    char *out = NULL;
    size_t len;

    if(r == NULL || fymd_render(r, md, strlen(md), &out, &len) != 0)
        return NULL;
    /* a layout marker never leaves the library */
    if(strstr(out, "\033_fy") != NULL) {
        dump("marker left in output", out);
        failures++;
    }
    return out;
}

/* Row @n of @out, without its newline, in @buf. */
static const char *
row(const char *out, int n, char *buf, size_t size)
{
    const char *p = out, *nl;
    size_t len;

    while(n-- > 0 && p != NULL) {
        p = strchr(p, '\n');
        if(p != NULL)
            p++;
    }
    if(p == NULL)
        return "";
    nl = strchr(p, '\n');
    len = nl ? (size_t)(nl - p) : strlen(p);
    if(len >= size)
        len = size - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/* The row of @out that holds @needle, or -1. */
static int
row_of(const char *out, const char *needle)
{
    const char *hit = out ? strstr(out, needle) : NULL, *p;
    int n = 0;

    if(hit == NULL)
        return -1;
    for(p = out; p < hit; p++)
        n += *p == '\n';
    return n;
}

/* The display columns of @s: one for each character. */
static size_t
cols(const char *s)
{
    size_t n = 0;

    for(; *s; s++)
        n += ((unsigned char) *s & 0xc0) != 0x80;
    return n;
}

static int
rows(const char *out)
{
    const char *p;
    int n = 0;

    for(p = out; p && *p; p++)
        n += *p == '\n';
    return n;
}

static void
test_fill(void)
{
    struct fymd_renderer *r;
    char buf[256];
    const char *line;
    char *out;

    /* a fill right-aligns what follows it, inside the margins */
    r = renderer(1, 20, 0);
    out = render(r, "a<fy-fill/>b\n");
    line = row(out, 0, buf, sizeof(buf));
    CHECK(strlen(line) == 18 && line[2] == 'a' && line[17] == 'b');
    if(strlen(line) != 18)
        dump("fill", out);
    fymd_free(out);

    /* two fills share the columns: the middle part is centred */
    out = render(r, "a<fy-fill/>m<fy-fill/>z\n");
    line = row(out, 0, buf, sizeof(buf));
    CHECK(strlen(line) == 18 && line[10] == 'm' && line[17] == 'z');
    if(strlen(line) != 18 || line[10] != 'm')
        dump("two fills", out);
    fymd_free(out);
    fymd_renderer_destroy(r);

    /* without the flag the tag is raw HTML and renders as nothing */
    r = renderer(0, 20, 0);
    out = render(r, "a<fy-fill/>b\n");
    CHECK(out != NULL && strstr(out, "ab") != NULL);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

static void
test_table_fill(void)
{
    struct fymd_renderer *r;
    char buf[256];
    const char *line, *l, *rr;
    char *out;
    int n;

    r = renderer(1, 40, 0);
    out = render(r, "| name | change |\n|---|---|\n| foo.c<fy-fill/>x | +10 |\n");
    n = row_of(out, "foo.c");
    line = row(out, n, buf, sizeof(buf));
    l = strstr(line, "foo.c");
    rr = l ? strchr(l, 'x') : NULL;
    /* the fill takes the padding of its cell */
    CHECK(l != NULL && rr != NULL && rr - l > 6);
    if(rr == NULL || rr - l <= 6)
        dump("table fill", out);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

static void
test_regions(void)
{
    const struct fymd_region *rg;
    struct fymd_renderer *r;
    size_t count, i;
    char *out;
    int wrapped;

    r = renderer(1, 40, 0);
    out = render(r, "x <fy-act id=\"go:here\">label</fy-act> y\n");
    CHECK(fymd_renderer_get_regions(r, &rg, &count) == 0 && count == 1);
    if(count == 1) {
        CHECK(!strcmp(rg[0].id, "go:here"));
        CHECK(rg[0].row == 0 && rg[0].col == 4 && rg[0].width == 5);
    }
    CHECK(fymd_renderer_region_at(r, 0, 4) != NULL);
    CHECK(fymd_renderer_region_at(r, 0, 8) != NULL);
    CHECK(fymd_renderer_region_at(r, 0, 9) == NULL);
    CHECK(fymd_renderer_region_at(r, 1, 4) == NULL);
    fymd_free(out);

    /* an id that is not valid makes no region */
    out = render(r, "<fy-act id=\"bad id\">label</fy-act>\n");
    CHECK(fymd_renderer_get_regions(r, &rg, &count) == 0 && count == 0);
    CHECK(out != NULL && strstr(out, "label") != NULL);
    fymd_free(out);

    /* a label that wraps has a region on each of its rows */
    out = render(r, "start <fy-act id=\"long\">one two three four five six seven "
                    "eight nine ten</fy-act>\n");
    CHECK(fymd_renderer_get_regions(r, &rg, &count) == 0 && count >= 2);
    for(i = 0, wrapped = 0; i < count; i++)
        wrapped += !strcmp(rg[i].id, "long") && rg[i].row > 0 && rg[i].col == 2;
    CHECK(wrapped >= 1);
    if(count < 2 || !wrapped)
        dump("wrapped act", out);
    fymd_free(out);
    fymd_renderer_destroy(r);

    /* without the flag there are no regions */
    r = renderer(0, 40, 0);
    out = render(r, "x <fy-act id=\"go\">label</fy-act> y\n");
    CHECK(fymd_renderer_get_regions(r, &rg, &count) == 0 && count == 0);
    CHECK(out != NULL && strstr(out, "x label y") != NULL);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

static void
test_role_glyph(void)
{
    struct fymd_renderer *r;
    char *out;

    r = renderer(1, 40, 0);
    out = render(r, "<fy-role name=\"tool.ok\">done</fy-role> "
                    "<fy-glyph name=\"gutter.tool\" fallback=\"->\"/> go\n");
    /* without a palette a role is plain text and a glyph is its fallback */
    CHECK(out != NULL && strstr(out, "done -> go") != NULL);
    if(out == NULL || strstr(out, "done -> go") == NULL)
        dump("role and glyph", out);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

static void
test_columns(void)
{
    struct fymd_renderer *r;
    char buf[256];
    const char *line;
    char *out;
    int n;

    r = renderer(1, 40, 0);
    out = render(r,
        "above\n"
        "\n"
        "<fy-columns widths=\"10,*\" gap=\"2\">\n"
        "<fy-col>\n"
        "\n"
        "left\n"
        "\n"
        "</fy-col>\n"
        "<fy-col>\n"
        "\n"
        "right <fy-act id=\"r\">side</fy-act>\n"
        "\n"
        "</fy-col>\n"
        "</fy-columns>\n"
        "\n"
        "below\n");
    n = row_of(out, "left");
    line = row(out, n, buf, sizeof(buf));
    /* margin 2, a 10 column cell, a 2 column gap */
    CHECK(n > 0 && strncmp(line, "  left", 6) == 0 && strstr(line, "right") == line + 14);
    CHECK(row_of(out, "below") > n);
    if(n <= 0 || strstr(line, "right") != line + 14)
        dump("columns", out);
    /* a region inside a column has the columns of the whole row */
    CHECK(fymd_renderer_region_at(r, (size_t) n, 20) != NULL);
    fymd_free(out);

    /* a table fits the width of its column */
    out = render(r,
        "<fy-columns widths=\"*,*\" gap=\"2\">\n"
        "<fy-col>\n\nleft side\n\n</fy-col>\n"
        "<fy-col>\n\n| test | result |\n|---|---|\n| parse | pass |\n\n</fy-col>\n"
        "</fy-columns>\n");
    for(n = 0; n < rows(out) + 1; n++)
        CHECK(cols(row(out, n, buf, sizeof(buf))) <= 40);
    CHECK(row_of(out, "left side") == row_of(out, "test"));
    if(row_of(out, "left side") != row_of(out, "test"))
        dump("table in a column", out);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

static void
test_vertical(void)
{
    const struct fymd_region *rg;
    struct fymd_renderer *r;
    size_t count;
    char *out;
    int top, bottom;

    /* the rows left over go to the vfill: the footer is on the last rows */
    r = renderer(1, 40, 12);
    out = render(r, "top\n\n<fy-vfill/>\n\n<fy-act id=\"quit\">bottom</fy-act>\n");
    top = row_of(out, "top");
    bottom = row_of(out, "bottom");
    CHECK(top == 0 && bottom >= 10 && rows(out) <= 12);
    if(!(top == 0 && bottom >= 10 && rows(out) <= 12))
        dump("vfill", out);
    /* a region moves with its row */
    CHECK(fymd_renderer_get_regions(r, &rg, &count) == 0 && count == 1);
    CHECK(count == 1 && (int) rg[0].row == bottom);
    fymd_free(out);

    /* a body taller than the page gives up its first rows, bottom anchored */
    fymd_renderer_set_height(r, 6);
    out = render(r, "head\n\n<fy-scroll anchor=\"bottom\">\n\n"
                    "* one\n* two\n* three\n* four\n* five\n\n"
                    "</fy-scroll>\n\nfoot\n");
    CHECK(out != NULL && strstr(out, "head") && strstr(out, "foot") &&
          strstr(out, "five") && !strstr(out, "one"));
    CHECK(rows(out) <= 6);
    if(out == NULL || strstr(out, "one") || rows(out) > 6)
        dump("scroll", out);
    fymd_free(out);

    /* top anchored: the last rows go */
    out = render(r, "head\n\n<fy-scroll anchor=\"top\">\n\n"
                    "* one\n* two\n* three\n* four\n* five\n\n"
                    "</fy-scroll>\n\nfoot\n");
    CHECK(out != NULL && strstr(out, "one") && !strstr(out, "five") &&
          strstr(out, "foot"));
    fymd_free(out);

    /* no height: the vertical tags change nothing */
    fymd_renderer_set_height(r, 0);
    out = render(r, "top\n\n<fy-vfill/>\n\nbottom\n");
    CHECK(row_of(out, "bottom") > 0 && row_of(out, "bottom") < 4);
    fymd_free(out);
    CHECK(fymd_renderer_set_height(r, -1) == -1);
    fymd_renderer_destroy(r);
}

#ifdef FYMD_TEST_PALETTE
static void
test_palette(void)
{
    struct fypal_caps caps = {
        .depth = FYPAL_DEPTH_TRUECOLOR,
        .attrs = FYPAL_ATTR_ALL,
        .underline_color = 1,
    };
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    struct fypal_ctx *pal;
    char *out;

    pal = fypal_ctx_create(&caps);
    CHECK(pal != NULL);
    if(pal == NULL)
        return;
    CHECK(fypal_ctx_load(pal,
        "colors: {okc: '#102030'}\n"
        "roles: {tool: {ok: {fg: okc}}}\n"
        "glyphs: {gutter: {tool: {utf: \"\\u2192\", ascii: \"->\"}}}\n",
        "test") == 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_DEFAULT | FYMD_RF_UI;
    cfg.width = 40;
    cfg.background = FYMD_BG_DARK;
    r = fymd_renderer_create(&cfg);
    CHECK(r != NULL && fymd_renderer_set_palette(r, pal) == 0);
    out = render(r, "<fy-role name=\"tool.ok\">done</fy-role> "
                    "<fy-glyph name=\"gutter.tool\" fallback=\"x\"/> go\n");
    CHECK(out != NULL && strstr(out, "38;2;16;32;48mdone") != NULL);
    CHECK(out != NULL && strstr(out, "\u2192 go") != NULL);
    if(out == NULL || strstr(out, "\u2192 go") == NULL)
        dump("palette role and glyph", out);
    fymd_free(out);

    CHECK(fymd_renderer_set_palette_flags(r, pal, FYMD_PF_ASCII) == 0);
    out = render(r, "<fy-glyph name=\"gutter.tool\" fallback=\"x\"/> go\n");
    CHECK(out != NULL && strstr(out, "-> go") != NULL);
    fymd_free(out);
    fymd_renderer_destroy(r);
    fypal_ctx_destroy(pal);
}
#endif

int
main(void)
{
    test_fill();
    test_table_fill();
    test_regions();
    test_role_glyph();
    test_columns();
    test_vertical();
#ifdef FYMD_TEST_PALETTE
    test_palette();
#endif
    return failures ? 1 : 0;
}
