/*
 * inline-api-test.c - assertions over the inline markdown reader
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>

static int failures;

#define CHECK(_cond, _fmt, ...)                                              \
    do {                                                                     \
        if (!(_cond)) {                                                      \
            fprintf(stderr, "%s:%d: " _fmt "\n", __func__, __LINE__,         \
                    ##__VA_ARGS__);                                          \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* The run that carries @needle, or NULL. */
static const struct fymd_inline_run *find_run(const struct fymd_inline *inl,
                                              const char *needle)
{
    const struct fymd_inline_run *run;
    size_t i;

    for (i = 0; i < fymd_inline_count(inl); i++) {
        run = fymd_inline_get(inl, i);
        if (strstr(run->text, needle) != NULL)
            return run;
    }
    return NULL;
}

static void test_plain(void)
{
    struct fymd_inline *inl;

    inl = fymd_inline_parse("just text", FYMD_NT, 0);
    CHECK(inl != NULL, "a plain text did not parse");
    if (inl == NULL)
        return;
    CHECK(fymd_inline_count(inl) == 1, "expected one run, got %zu",
          fymd_inline_count(inl));
    CHECK(strcmp(fymd_inline_get(inl, 0)->text, "just text") == 0,
          "the run text is '%s'", fymd_inline_get(inl, 0)->text);
    CHECK(fymd_inline_get(inl, 0)->attrs == 0, "a plain run carries no attrs");
    CHECK(fymd_inline_get(inl, 1) == NULL, "there should be no second run");
    CHECK(strcmp(fymd_inline_plain(inl), "just text") == 0,
          "the plain text is '%s'", fymd_inline_plain(inl));
    fymd_inline_destroy(inl);
}

static void test_attributes(void)
{
    const struct fymd_inline_run *run;
    struct fymd_inline *inl;

    inl = fymd_inline_parse("a **b** *c* `d`", FYMD_NT, 0);
    CHECK(inl != NULL, "the marked up text did not parse");
    if (inl == NULL)
        return;

    run = find_run(inl, "b");
    CHECK(run != NULL && (run->attrs & FYMD_IA_STRONG),
          "'b' should be strong");
    run = find_run(inl, "c");
    CHECK(run != NULL && (run->attrs & FYMD_IA_EM), "'c' should be emphasis");
    run = find_run(inl, "d");
    CHECK(run != NULL && (run->attrs & FYMD_IA_CODE), "'d' should be code");
    CHECK(strcmp(fymd_inline_plain(inl), "a b c d") == 0,
          "the markers should be gone, got '%s'", fymd_inline_plain(inl));
    fymd_inline_destroy(inl);

    /* strikethrough is an extension, so it needs asking for */
    inl = fymd_inline_parse("a ~~b~~", FYMD_NT, 0);
    CHECK(inl != NULL && find_run(inl, "~~") != NULL,
          "without the flag the tildes stay");
    fymd_inline_destroy(inl);

    inl = fymd_inline_parse("a ~~b~~", FYMD_NT, FYMD_IF_STRIKETHROUGH);
    run = inl != NULL ? find_run(inl, "b") : NULL;
    CHECK(run != NULL && (run->attrs & FYMD_IA_DEL), "'b' should be struck");
    fymd_inline_destroy(inl);

    /* the attributes nest */
    inl = fymd_inline_parse("***both***", FYMD_NT, 0);
    run = inl != NULL ? find_run(inl, "both") : NULL;
    CHECK(run != NULL && (run->attrs & FYMD_IA_STRONG) &&
          (run->attrs & FYMD_IA_EM), "'both' should be strong and emphasis");
    fymd_inline_destroy(inl);
}

static void test_links(void)
{
    const struct fymd_inline_run *run;
    struct fymd_inline *inl;

    inl = fymd_inline_parse("see [the docs](https://example.test/x)", FYMD_NT,
                            0);
    CHECK(inl != NULL, "the link did not parse");
    if (inl == NULL)
        return;
    run = find_run(inl, "the docs");
    CHECK(run != NULL && (run->attrs & FYMD_IA_LINK), "the text is a link");
    CHECK(run != NULL && run->href != NULL &&
          strcmp(run->href, "https://example.test/x") == 0,
          "the target is '%s'", run != NULL && run->href != NULL ?
          run->href : "(none)");
    /* the target belongs to the link, not to what follows it */
    run = find_run(inl, "see ");
    CHECK(run != NULL && run->href == NULL,
          "the text before a link carries no target");
    fymd_inline_destroy(inl);
}

static void test_breaks(void)
{
    const struct fymd_inline_run *run;
    struct fymd_inline *inl;
    size_t i, breaks = 0;

    /* a hard break is its own run; a soft one is a space */
    inl = fymd_inline_parse("a  \nb", FYMD_NT, 0);
    CHECK(inl != NULL, "the hard break did not parse");
    if (inl != NULL) {
        for (i = 0; i < fymd_inline_count(inl); i++) {
            run = fymd_inline_get(inl, i);
            if (run->attrs & FYMD_IA_BREAK)
                breaks++;
        }
        CHECK(breaks == 1, "expected one break run, got %zu", breaks);
        fymd_inline_destroy(inl);
    }

    inl = fymd_inline_parse("a\nb", FYMD_NT, 0);
    CHECK(inl != NULL && strcmp(fymd_inline_plain(inl), "a b") == 0,
          "a soft break should be a space, got '%s'",
          inl != NULL ? fymd_inline_plain(inl) : "(none)");
    fymd_inline_destroy(inl);
}

/* Block structure is consumed; only what it contains is reported. */
static void test_no_blocks(void)
{
    struct fymd_inline *inl;

    inl = fymd_inline_parse("# a heading", FYMD_NT, 0);
    CHECK(inl != NULL && strcmp(fymd_inline_plain(inl), "a heading") == 0,
          "a heading marker should be consumed, got '%s'",
          inl != NULL ? fymd_inline_plain(inl) : "(none)");
    fymd_inline_destroy(inl);

    /* an indented label must not become a code block */
    inl = fymd_inline_parse("    indented **bold**", FYMD_NT, 0);
    CHECK(inl != NULL && find_run(inl, "bold") != NULL &&
          (find_run(inl, "bold")->attrs & FYMD_IA_STRONG),
          "an indented label should still be read as markdown");
    fymd_inline_destroy(inl);
}

static void test_bad_input(void)
{
    struct fymd_inline *inl;

    CHECK(fymd_inline_parse(NULL, 0, 0) == NULL, "NULL should not parse");
    CHECK(fymd_inline_count(NULL) == 0, "a NULL result counts zero runs");
    CHECK(fymd_inline_get(NULL, 0) == NULL, "a NULL result has no runs");

    inl = fymd_inline_parse("", FYMD_NT, 0);
    CHECK(inl != NULL, "an empty text should parse");
    CHECK(inl != NULL && fymd_inline_count(inl) == 0,
          "an empty text has no runs");
    CHECK(inl != NULL && strcmp(fymd_inline_plain(inl), "") == 0,
          "an empty text is empty");
    fymd_inline_destroy(inl);

    /* a length that stops short of the NUL */
    inl = fymd_inline_parse("abcdef", 3, 0);
    CHECK(inl != NULL && strcmp(fymd_inline_plain(inl), "abc") == 0,
          "an explicit length should be honoured, got '%s'",
          inl != NULL ? fymd_inline_plain(inl) : "(none)");
    fymd_inline_destroy(inl);

    fymd_inline_destroy(NULL);
}

int main(void)
{
    test_plain();
    test_attributes();
    test_links();
    test_breaks();
    test_no_blocks();
    test_bad_input();

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
