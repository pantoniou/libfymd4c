/*
 * sgr-safe-api-test.c - what SGR_SAFE keeps of the escapes in its input
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

static char *render_safe(const char *md)
{
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    char *out;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_DEFAULT;
    cfg.width = 60;
    cfg.sgr_input = FYMD_SGR_SAFE;
    r = fymd_renderer_create(&cfg);
    if (!r)
        return NULL;
    out = fymd_render_to_string(r, md, strlen(md));
    fymd_renderer_destroy(r);
    return out;
}

static void test_plain_sgr_is_kept(void)
{
    char *out = render_safe("one \x1b[31mred\x1b[0m \x1b[38;2;1;2;3mrgb\x1b[0m two\n");

    CHECK(out, "the text renders");
    if (!out)
        return;
    CHECK(strstr(out, "\x1b[31mred"), "a plain SGR sequence is kept");
    CHECK(strstr(out, "\x1b[38;2;1;2;3mrgb"), "an SGR sequence with parameters is kept");
    fymd_free(out);
}

static void test_private_csi_is_dropped(void)
{
    char *out = render_safe("one \x1b[?4mtwo\x1b[>1m three\x1b[4 m four\n");

    CHECK(out, "the text renders");
    if (!out)
        return;
    CHECK(!strstr(out, "\x1b[?4m"), "a private-prefix sequence is dropped");
    CHECK(!strstr(out, "\x1b[>1m"), "a '>'-prefix sequence is dropped");
    CHECK(!strstr(out, "\x1b[4 m"), "a sequence with an intermediate byte is dropped");
    CHECK(strstr(out, "two") && strstr(out, "four"), "the text stays");
    fymd_free(out);
}

int main(void)
{
    test_plain_sgr_is_kept();
    test_private_csi_is_dropped();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
