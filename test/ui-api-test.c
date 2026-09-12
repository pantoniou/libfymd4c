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

static size_t cols(const char *s);

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

    /* a fill with a glyph draws its columns with it */
    out = render(r, "a<fy-fill char=\"\u2500\"/>b\n");
    line = row(out, 0, buf, sizeof(buf));
    CHECK(strstr(line, "a\u2500\u2500") != NULL && strstr(line, "\u2500b") != NULL &&
          cols(line) == 18);
    if(strstr(line, "\u2500b") == NULL || cols(line) != 18)
        dump("fill glyph", out);
    fymd_free(out);
    /* a glyph of two columns is not a fill glyph: blanks instead */
    out = render(r, "a<fy-fill char=\"\u4e00\"/>b\n");
    line = row(out, 0, buf, sizeof(buf));
    CHECK(strstr(line, "\u4e00") == NULL && cols(line) == 18);
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


/* A slot renderer that draws "x y" and records what it was asked. */
static char slot_last_id[64];
static int slot_last_width, slot_last_height, slot_nested_rc = 0;

static int
slot_text(void *userdata, const char *id, int width, int height,
          enum fymd_block_flags flags, fymd_block_emit_fn emit, void *emit_ctx)
{
    const char *text = (const char *) userdata;

    (void) flags;
    snprintf(slot_last_id, sizeof(slot_last_id), "%s", id);
    slot_last_width = width;
    slot_last_height = height;
    emit(emit_ctx, text, strlen(text));
    return 0;
}

/* A slot renderer that renders Markdown with a renderer of its own. */
static int
slot_nested(void *userdata, const char *id, int width, int height,
            enum fymd_block_flags flags, fymd_block_emit_fn emit, void *emit_ctx)
{
    struct fymd_renderer *outer = (struct fymd_renderer *) userdata;
    struct fymd_renderer *inner;
    char *out = NULL;
    size_t len = 0;

    (void) id;
    (void) height;
    (void) flags;
    /* the calling renderer does not render while it renders */
    slot_nested_rc = fymd_render(outer, "x", 1, &out, &len);
    fymd_free(out);
    out = NULL;
    inner = renderer(1, width + 4, 0);
    if(inner == NULL || fymd_render(inner, "**nested** body\n\nsecond\n", 24,
                                    &out, &len) != 0) {
        fymd_renderer_destroy(inner);
        return -1;
    }
    while(len > 0 && out[len - 1] == '\n')
        len--;
    emit(emit_ctx, out, len);
    fymd_free(out);
    fymd_renderer_destroy(inner);
    return 0;
}

static const struct fymd_region *
region_find(struct fymd_renderer *r, const char *id)
{
    const struct fymd_region *rg;
    size_t count, i;

    if(fymd_renderer_get_regions(r, &rg, &count) != 0)
        return NULL;
    for(i = 0; i < count; i++)
        if(!strcmp(rg[i].id, id))
            return &rg[i];
    return NULL;
}

static void
test_slots(void)
{
    const struct fymd_region *rg;
    struct fymd_renderer *r;
    char buf[512];
    char *out;
    int top, bottom;

    /* an inline slot reserves non-breaking cells and reports them */
    r = renderer(1, 40, 0);
    out = render(r, "a <fy-slot id=\"spin\" width=\"3\"/> b\n");
    rg = region_find(r, "spin");
    CHECK(rg != NULL && rg->kind == FYMD_REGION_SLOT && rg->row == 0 &&
          rg->col == 4 && rg->width == 3 && rg->height == 1);
    CHECK(out != NULL && strstr(out, "a \xc2\xa0\xc2\xa0\xc2\xa0 b") != NULL);
    if(rg == NULL || rg->col != 4)
        dump("inline slot", out);
    fymd_free(out);

    /* drawn by the slot renderer: clipped to the slot, blanks kept together */
    CHECK(fymd_renderer_set_slot_renderer(r, slot_text, (void *) "x yz") == 0);
    out = render(r, "a <fy-slot id=\"s2\" width=\"3\"/> b\n");
    CHECK(out != NULL && strstr(out, "a x\xc2\xa0y b") != NULL);
    CHECK(!strcmp(slot_last_id, "s2") && slot_last_width == 3 &&
          slot_last_height == 1);
    if(out == NULL || strstr(out, "a x\xc2\xa0y b") == NULL)
        dump("inline slot content", out);
    fymd_free(out);
    /* an escape takes no column of the slot */
    CHECK(fymd_renderer_set_slot_renderer(r, slot_text,
                                          (void *) "\033[1mabc\033[22mdef") == 0);
    out = render(r, "a <fy-slot id=\"s3\" width=\"3\"/> b\n");
    CHECK(out != NULL && strstr(out, "\033[1mabc") != NULL &&
          strstr(out, "abcd") == NULL);
    if(out == NULL || strstr(out, "\033[1mabc") == NULL)
        dump("slot with escapes", out);
    fymd_free(out);
    out = render(r, "<fy-slot id=\"s4\" height=\"1\"/>\n");
    CHECK(out != NULL && strstr(out, "\033[1mabc\033[22mdef") != NULL);
    fymd_free(out);
    CHECK(fymd_renderer_set_slot_renderer(r, NULL, NULL) == 0);

    /* a block slot of 3 rows at the width of the page */
    out = render(r, "top\n\n<fy-slot id=\"pane\" height=\"3\"/>\n\nbottom\n");
    rg = region_find(r, "pane");
    top = row_of(out, "top");
    bottom = row_of(out, "bottom");
    CHECK(rg != NULL && rg->kind == FYMD_REGION_SLOT && rg->col == 2 &&
          rg->width == 36 && rg->height == 3);
    CHECK(rg != NULL && (int) rg->row > top && bottom >= (int) rg->row + 3);
    if(rg == NULL || rg->height != 3 || bottom < (int) rg->row + 3)
        dump("block slot", out);
    fymd_free(out);

    /* its rows come from a component that renders Markdown recursively */
    CHECK(fymd_renderer_set_slot_renderer(r, slot_nested, r) == 0);
    out = render(r, "top\n\n<fy-slot id=\"nest\"/>\n\nbottom\n");
    rg = region_find(r, "nest");
    CHECK(slot_nested_rc == -1);
    CHECK(out != NULL && strstr(out, "nested body") != NULL &&
          strstr(out, "second") != NULL);
    CHECK(rg != NULL && rg->height >= 3 &&
          row_of(out, "nested") >= (int) rg->row &&
          row_of(out, "second") < (int) rg->row + rg->height &&
          row_of(out, "bottom") >= (int) rg->row + rg->height);
    if(rg == NULL || rg->height < 3)
        dump("nested slot", out);
    fymd_free(out);
    CHECK(fymd_renderer_set_slot_renderer(r, NULL, NULL) == 0);

    /* inside a column the slot has the width and the columns of its column */
    out = render(r,
        "<fy-columns widths=\"10,*\" gap=\"2\">\n"
        "<fy-col>\n\nleft\n\n</fy-col>\n"
        "<fy-col>\n\n<fy-slot id=\"side\" height=\"2\"/>\n\n</fy-col>\n"
        "</fy-columns>\n");
    rg = region_find(r, "side");
    CHECK(rg != NULL && rg->col == 14 && rg->width == 24 && rg->height == 2);
    if(rg == NULL || rg->col != 14)
        dump("slot in a column", out);
    fymd_free(out);
    fymd_renderer_destroy(r);

    /* an elastic slot takes the rows the page leaves over */
    r = renderer(1, 40, 12);
    out = render(r, "top\n\n<fy-slot id=\"body\" height=\"*\"/>\n\n"
                    "<fy-act id=\"quit\">foot</fy-act>\n");
    rg = region_find(r, "body");
    top = row_of(out, "top");
    bottom = row_of(out, "foot");
    CHECK(rg != NULL && rg->kind == FYMD_REGION_SLOT && rg->col == 2 &&
          rg->width == 36 && rg->height >= 6);
    CHECK(rg != NULL && (int) rg->row > top &&
          (int) rg->row + rg->height <= bottom && bottom >= 10);
    rg = region_find(r, "quit");
    CHECK(rg != NULL && (int) rg->row == bottom);
    if(bottom < 10)
        dump("elastic slot", out);
    (void) buf;
    fymd_free(out);

    /* without a height an elastic slot is one row */
    fymd_renderer_set_height(r, 0);
    out = render(r, "top\n\n<fy-slot id=\"body\" height=\"*\"/>\n\nfoot\n");
    rg = region_find(r, "body");
    CHECK(rg != NULL && rg->height == 1);
    fymd_free(out);
    fymd_renderer_destroy(r);
}


static void
test_weights(void)
{
    const struct fymd_region *a, *b;
    struct fymd_renderer *r;
    char buf[256];
    const char *line;
    char *out;
    int n;

    /* weighted columns: 36 columns less a gap of 2 is 34, shared 1:3 */
    r = renderer(1, 40, 0);
    out = render(r,
        "<fy-columns widths=\"*,3*\" gap=\"2\">\n"
        "<fy-col>\n\nleft\n\n</fy-col>\n"
        "<fy-col>\n\nright\n\n</fy-col>\n"
        "</fy-columns>\n");
    n = row_of(out, "left");
    line = row(out, n, buf, sizeof(buf));
    /* 34 * 1/4 is 8.5: the largest remainders give the first column 9 */
    CHECK(strstr(line, "right") == line + 2 + 9 + 2);
    if(strstr(line, "right") != line + 13)
        dump("weighted columns", out);
    fymd_free(out);

    /* a minimum holds a weighted column that its share would squeeze */
    out = render(r,
        "<fy-columns widths=\"*,9*\" gap=\"2\" min=\"12\">\n"
        "<fy-col>\n\nleft\n\n</fy-col>\n"
        "<fy-col>\n\nright\n\n</fy-col>\n"
        "</fy-columns>\n");
    n = row_of(out, "left");
    line = row(out, n, buf, sizeof(buf));
    CHECK(strstr(line, "right") == line + 2 + 12 + 2);
    if(strstr(line, "right") != line + 16)
        dump("column minimum", out);
    fymd_free(out);

    /* fixed, percent and weighted sizes together */
    out = render(r,
        "<fy-columns widths=\"6,50%,*\" gap=\"1\">\n"
        "<fy-col>\n\na\n\n</fy-col>\n"
        "<fy-col>\n\nb\n\n</fy-col>\n"
        "<fy-col>\n\nc\n\n</fy-col>\n"
        "</fy-columns>\n");
    n = row_of(out, "a");
    line = row(out, n, buf, sizeof(buf));
    /* 36 less 2 gaps is 34: 6, then 17, then the 11 left */
    CHECK(line[2] == 'a' && line[2 + 6 + 1] == 'b' && line[2 + 6 + 1 + 17 + 1] == 'c');
    if(!(line[9] == 'b' && line[27] == 'c'))
        dump("mixed sizes", out);
    fymd_free(out);
    fymd_renderer_destroy(r);

    /* weighted elastic slots share the rows the page leaves over */
    r = renderer(1, 40, 20);
    out = render(r, "top\n\n<fy-slot id=\"a\" height=\"*\"/>\n\nmid\n\n"
                    "<fy-slot id=\"b\" height=\"3*\"/>\n\nbot\n");
    a = region_find(r, "a");
    b = region_find(r, "b");
    CHECK(a != NULL && b != NULL && a->height > 0 &&
          (b->height == 3 * a->height || b->height == 3 * a->height + 1 ||
           b->height == 3 * a->height - 1 || b->height == 3 * a->height + 2));
    CHECK(rows(out) <= 20 && row_of(out, "bot") >= 18);
    if(a == NULL || b == NULL || row_of(out, "bot") < 18)
        dump("weighted slots", out);
    fymd_free(out);

    /* a vfill weight moves the middle row towards the bottom */
    out = render(r, "top\n\n<fy-vfill weight=\"3\"/>\n\nmid\n\n<fy-vfill/>\n\nbot\n");
    n = row_of(out, "mid");
    CHECK(n > row_of(out, "top") + 8 && row_of(out, "bot") >= 18);
    if(n <= 8)
        dump("weighted vfill", out);
    fymd_free(out);

    /* a minimum is held when the page is full: a scroll body gives rows up */
    fymd_renderer_set_height(r, 8);
    out = render(r, "head\n\n<fy-scroll anchor=\"bottom\">\n\n"
                    "* one\n* two\n* three\n* four\n\n</fy-scroll>\n\n"
                    "<fy-slot id=\"pane\" height=\"*\" min=\"4\"/>\n\nfoot\n");
    a = region_find(r, "pane");
    CHECK(a != NULL && a->height >= 4);
    CHECK(out != NULL && strstr(out, "head") && strstr(out, "foot") &&
          !strstr(out, "one"));
    if(a == NULL || a->height < 4)
        dump("slot minimum", out);
    fymd_free(out);

    /* without a page height an elastic slot has its least rows */
    fymd_renderer_set_height(r, 0);
    out = render(r, "<fy-slot id=\"pane\" height=\"2*\" min=\"3\"/>\n");
    a = region_find(r, "pane");
    CHECK(a != NULL && a->height == 3);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

/* The natural rows of @md: a render without a page height. */
static int
natural_rows(struct fymd_renderer *r, const char *md)
{
    char *out;
    int n;

    fymd_renderer_set_height(r, 0);
    out = render(r, md);
    n = rows(out);
    fymd_free(out);
    return n;
}

static void
test_drop(void)
{
    const struct fymd_region *rg;
    struct fymd_renderer *r;
    char *out;
    int n;
    static const char page[] =
        "<fy-drop order=\"2\">\n\nheader\n\n</fy-drop>\n\n"
        "body\n\n"
        "<fy-drop order=\"1\">\n\n<fy-act id=\"status\">status</fy-act>\n\n"
        "</fy-drop>\n\n"
        "prompt\n";
    static const char ties[] =
        "<fy-drop>\n\nfirst\n\n</fy-drop>\n\n"
        "<fy-drop>\n\nsecond\n\n</fy-drop>\n\nkeep\n";
    static const char scroll[] =
        "head\n\n<fy-scroll anchor=\"bottom\">\n\n* one\n* two\n\n"
        "</fy-scroll>\n\n<fy-drop>\n\nstatus\n\n</fy-drop>\n\nfoot\n";
    static const char order[] =
        "<fy-drop order=\"abc\">\n\nxxx\n\n</fy-drop>\n\n"
        "<fy-drop order=\"1\">\n\nyyy\n\n</fy-drop>\n\nzzz\n";

    r = renderer(1, 40, 0);

    /* a page that fits keeps every body, and the tags draw nothing */
    n = natural_rows(r, page);
    fymd_renderer_set_height(r, n);
    out = render(r, page);
    CHECK(out != NULL && strstr(out, "header") && strstr(out, "body") &&
          strstr(out, "status") && strstr(out, "prompt"));
    CHECK(region_find(r, "status") != NULL);
    CHECK(out != NULL && !strstr(out, "fy-drop"));
    fymd_free(out);

    /* one row short: the lowest order goes, and its region with it */
    fymd_renderer_set_height(r, n - 1);
    out = render(r, page);
    CHECK(out != NULL && strstr(out, "header") && !strstr(out, "status") &&
          strstr(out, "body") && strstr(out, "prompt"));
    CHECK(region_find(r, "status") == NULL);
    CHECK(rows(out) <= n - 1);
    if(out == NULL || strstr(out, "status") || !strstr(out, "header"))
        dump("drop the lowest order", out);
    fymd_free(out);

    /* shorter still: the next order goes too; what has no drop stays */
    fymd_renderer_set_height(r, 2);
    out = render(r, page);
    CHECK(out != NULL && !strstr(out, "header") && !strstr(out, "status") &&
          strstr(out, "body") && strstr(out, "prompt"));
    if(out == NULL || strstr(out, "header"))
        dump("drop both orders", out);
    fymd_free(out);

    /* equal orders: the later body goes first */
    n = natural_rows(r, ties);
    fymd_renderer_set_height(r, n - 1);
    out = render(r, ties);
    CHECK(out != NULL && strstr(out, "first") && !strstr(out, "second") &&
          strstr(out, "keep"));
    if(out == NULL || !strstr(out, "first") || strstr(out, "second"))
        dump("drop ties", out);
    fymd_free(out);

    /* a drop goes before a scroll body gives up a row */
    n = natural_rows(r, scroll);
    fymd_renderer_set_height(r, n - 1);
    out = render(r, scroll);
    CHECK(out != NULL && strstr(out, "one") && strstr(out, "two") &&
          !strstr(out, "status") && strstr(out, "foot"));
    if(out == NULL || !strstr(out, "one") || strstr(out, "status"))
        dump("drop before scroll", out);
    fymd_free(out);

    /* an elastic slot in a dropped body gets no rows and frees its minimum */
    fymd_renderer_set_height(r, 2);
    out = render(r, "<fy-drop>\n\n<fy-slot id=\"s\" height=\"*\" min=\"3\"/>\n\n"
                    "</fy-drop>\n\nkeep\n");
    rg = region_find(r, "s");
    CHECK(rg != NULL && rg->height == 0);
    CHECK(out != NULL && strstr(out, "keep") && rows(out) <= 2);
    if(rg == NULL || rg->height != 0 || rows(out) > 2)
        dump("drop an elastic slot", out);
    fymd_free(out);

    /* a dropped elastic slot frees its minimum: the next order stays */
    fymd_renderer_set_height(r, natural_rows(r, "<fy-drop order=\"2\">\n\n"
                                            "header\n\n</fy-drop>\n\nkeep\n"));
    out = render(r, "<fy-drop order=\"1\">\n\n"
                    "<fy-slot id=\"s\" height=\"*\" min=\"5\"/>\n\n"
                    "</fy-drop>\n\n<fy-drop order=\"2\">\n\nheader\n\n"
                    "</fy-drop>\n\nkeep\n");
    CHECK(out != NULL && strstr(out, "header") && strstr(out, "keep"));
    if(out == NULL || !strstr(out, "header"))
        dump("drop frees a minimum", out);
    fymd_free(out);

    /* a scroll body counts only the rows a drop left in it, so the rows
     * still over come from the next scroll body */
    fymd_renderer_set_height(r, 4);
    out = render(r, "<fy-scroll anchor=\"bottom\">\n\n<fy-drop>\n\nd1\n\n"
                    "d2\n\nd3\n\n</fy-drop>\n\n* a1\n\n</fy-scroll>\n\n"
                    "<fy-scroll anchor=\"bottom\">\n\n* b1\n* b2\n* b3\n"
                    "* b4\n\n</fy-scroll>\n\nfoot\n");
    CHECK(out != NULL && rows(out) <= 4 && !strstr(out, "d1") &&
          !strstr(out, "b2") && strstr(out, "b4") && strstr(out, "foot"));
    if(out == NULL || rows(out) > 4 || strstr(out, "b2"))
        dump("scroll after a drop", out);
    fymd_free(out);

    /* an order that is not a number is order 0 */
    n = natural_rows(r, order);
    fymd_renderer_set_height(r, n - 1);
    out = render(r, order);
    CHECK(out != NULL && !strstr(out, "xxx") && strstr(out, "yyy") &&
          strstr(out, "zzz"));
    fymd_free(out);

    /* an unclosed drop is not a body: nothing of it goes */
    fymd_renderer_set_height(r, 1);
    out = render(r, "<fy-drop>\n\nlost\n\nkeep\n");
    CHECK(out != NULL && strstr(out, "lost") && strstr(out, "keep"));
    fymd_free(out);

    /* a close without an open is ignored */
    out = render(r, "aaa\n\n</fy-drop>\n\nbbb\n");
    CHECK(out != NULL && strstr(out, "aaa") && strstr(out, "bbb"));
    fymd_free(out);

    /* no page height: nothing is dropped */
    fymd_renderer_set_height(r, 0);
    out = render(r, page);
    CHECK(out != NULL && strstr(out, "header") && strstr(out, "status"));
    fymd_free(out);
    fymd_renderer_destroy(r);

    /* without UI Markdown the bodies are plain Markdown */
    r = renderer(0, 40, 2);
    out = render(r, page);
    CHECK(out != NULL && strstr(out, "header") && strstr(out, "status"));
    fymd_free(out);
    fymd_renderer_destroy(r);
}

int
main(void)
{
    test_fill();
    test_table_fill();
    test_regions();
    test_role_glyph();
    test_columns();
    test_vertical();
    test_slots();
    test_weights();
    test_drop();
#ifdef FYMD_TEST_PALETTE
    test_palette();
#endif
    return failures ? 1 : 0;
}
