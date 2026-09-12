/*
 * block-renderer-api-test.c - fymd_renderer_set_block_renderer(): a closed
 * fenced block of a Markdown document is drawn by the renderer registered for
 * its language, and nothing else is handed to it.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>

static const char doc[] =
    "# Title\n"
    "\n"
    "```shout\n"
    "hello\n"
    "```\n"
    "\n"
    "```diff\n"
    "@@ -1 +1 @@\n"
    "-shout\n"
    "+shout\n"
    "```\n"
    "\n"
    "```c\n"
    "int shout;\n"
    "```\n";

struct calls {
    int n;
    int width;
    unsigned flags;
    char lang[16];
};

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #cond);                                       \
            failures++;                                                     \
        }                                                                   \
    } while(0)

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

/* Draw the body upper-cased between markers, and a second row. */
static int
shout(void *userdata, const char *lang, const char *text, size_t len, int width,
      enum fymd_block_flags flags, fymd_block_emit_fn emit, void *emit_ctx)
{
    struct calls *c = userdata;
    char up[64];
    size_t i;

    c->n++;
    c->width = width;
    c->flags = (unsigned) flags;
    snprintf(c->lang, sizeof(c->lang), "%s", lang);
    while(len > 0 && text[len - 1] == '\n')
        len--;
    if(len >= sizeof(up))
        return -1;
    for(i = 0; i < len; i++)
        up[i] = (char) toupper((unsigned char) text[i]);
    emit(emit_ctx, "<<", 2);
    emit(emit_ctx, up, len);
    emit(emit_ctx, ">>\nsecond row\n", 14);
    return 0;
}

/* Emit, then decline: the output must not appear. */
static int
decline(void *userdata, const char *lang, const char *text, size_t len,
        int width, enum fymd_block_flags flags, fymd_block_emit_fn emit,
        void *emit_ctx)
{
    struct calls *c = userdata;

    (void) lang;
    (void) text;
    (void) len;
    (void) width;
    (void) flags;
    c->n++;
    emit(emit_ctx, "JUNK", 4);
    return -1;
}

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
    cfg.width = 60;
    return fymd_renderer_create(&cfg);
}

int
main(void)
{
    static const char open_doc[] = "# Title\n\n```shout\nhel";
    struct calls c;
    struct fymd_update upd;
    struct fymd_renderer *r;
    const char *fin;
    char *out;
    size_t len;

    r = renderer(0);
    CHECK(r != NULL);
    if(r == NULL)
        return 1;

    /* without a renderer the block is code */
    out = render(r);
    expect(__LINE__, out, "hello", "HELLO");
    fymd_free(out);

    /* a closed block is drawn by its renderer, under the document margin,
     * without code chrome; the diff and the C block are not handed to it */
    memset(&c, 0, sizeof(c));
    CHECK(fymd_renderer_set_block_renderer(r, "shout", shout, &c) == 0);
    out = render(r);
    expect(__LINE__, out, "  <<HELLO>>\n  second row\n", "hello");
    /* the diff keeps its own rendering */
    expect(__LINE__, out, "-shout", NULL);
    CHECK(c.n == 1);
    CHECK(strcmp(c.lang, "shout") == 0);
    CHECK(c.width > 0 && c.width < 60);
    CHECK(c.flags == 0);
    fymd_free(out);

    /* a new theme keeps the renderer */
    CHECK(fymd_renderer_set_theme(r, "default") == 0);
    out = render(r);
    expect(__LINE__, out, "<<HELLO>>", NULL);
    fymd_free(out);

    /* a declined block renders as code, and what the renderer emitted is gone */
    memset(&c, 0, sizeof(c));
    CHECK(fymd_renderer_set_block_renderer(r, "shout", decline, &c) == 0);
    out = render(r);
    expect(__LINE__, out, "hello", "JUNK");
    CHECK(c.n == 1);
    fymd_free(out);

    /* a stream does not hand an open fence to the renderer, and holds it */
    memset(&c, 0, sizeof(c));
    CHECK(fymd_renderer_set_block_renderer(r, "shout", shout, &c) == 0);
    memset(&upd, 0, sizeof(upd));
    CHECK(fymd_render_push(r, open_doc, strlen(open_doc), &upd) == 0);
    CHECK(c.n == 0);
    CHECK(fymd_renderer_set_block_renderer(r, "shout", NULL, NULL) == -1);
    fin = NULL;
    CHECK(fymd_render_finish(r, &fin, &len) == 0);
    fymd_render_reset(r);

    /* NULL removes the renderer */
    CHECK(fymd_renderer_set_block_renderer(r, "shout", NULL, NULL) == 0);
    memset(&c, 0, sizeof(c));
    out = render(r);
    expect(__LINE__, out, "hello", "HELLO");
    CHECK(c.n == 0);
    fymd_free(out);

    CHECK(fymd_renderer_set_block_renderer(r, "", shout, &c) == -1);
    CHECK(fymd_renderer_set_block_renderer(NULL, "shout", shout, &c) == -1);
    fymd_renderer_destroy(r);

    /* the renderer learns that the render has no colour */
    r = renderer(FYMD_RF_NO_COLOR);
    CHECK(r != NULL);
    memset(&c, 0, sizeof(c));
    CHECK(r && fymd_renderer_set_block_renderer(r, "shout", shout, &c) == 0);
    out = r ? render(r) : NULL;
    expect(__LINE__, out, "<<HELLO>>", NULL);
    CHECK(c.flags & FYMD_BF_NO_COLOR);
    fymd_free(out);
    fymd_renderer_destroy(r);

    return failures ? 1 : 0;
}
