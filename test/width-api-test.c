/*
 * width-api-test.c - the public display width measurement
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

static void test_cp_width(void)
{
    CHECK(fymd_cp_width('a') == 1, "ASCII is one column");
    CHECK(fymd_cp_width(0x00e9) == 1, "a precomposed e-acute is one column");
    CHECK(fymd_cp_width('\t') == 0, "a control has no width of its own");
    CHECK(fymd_cp_width(0x0000) == 0, "NUL has no width");

    /* combining marks and format characters take no column */
    CHECK(fymd_cp_width(0x0301) == 0, "a combining acute is zero width");
    CHECK(fymd_cp_width(0x200b) == 0, "a zero width space is zero width");
    CHECK(fymd_cp_width(0xfe0f) == 0, "a variation selector is zero width");

    /* East Asian Wide and Fullwidth */
    CHECK(fymd_cp_width(0x4e00) == 2, "a CJK ideograph is two columns");
    CHECK(fymd_cp_width(0x3042) == 2, "hiragana is two columns");
    CHECK(fymd_cp_width(0xac00) == 2, "hangul is two columns");
    CHECK(fymd_cp_width(0xff21) == 2, "a fullwidth A is two columns");
    CHECK(fymd_cp_width(0x1f600) == 2, "an emoji is two columns");

    /*
     * Beyond the basic plane. A table that stops at the BMP gets these
     * wrong, and a terminal does not.
     */
    CHECK(fymd_cp_width(0x17000) == 2, "Tangut is two columns");
    CHECK(fymd_cp_width(0x18800) == 2, "Tangut components are two columns");
    CHECK(fymd_cp_width(0x1b000) == 2, "Kana supplement is two columns");
    CHECK(fymd_cp_width(0x1da00) == 0, "a SignWriting mark is zero width");

    /* the box drawing a terminal renderer leans on stays single width */
    CHECK(fymd_cp_width(0x2500) == 1, "a box drawing rule is one column");
    CHECK(fymd_cp_width(0x256d) == 1, "a rounded corner is one column");
    CHECK(fymd_cp_width(0x25bc) == 1, "a solid triangle is one column");
}

static void test_utf8_decode(void)
{
    unsigned int cp = 0;
    size_t n;

    n = fymd_utf8_decode("a", 1, &cp);
    CHECK(n == 1 && cp == 'a', "ASCII decodes to itself");

    n = fymd_utf8_decode("\xc3\xa9", 2, &cp);
    CHECK(n == 2 && cp == 0x00e9, "two bytes decode to e-acute, got %u", cp);

    n = fymd_utf8_decode("\xe4\xb8\x80", 3, &cp);
    CHECK(n == 3 && cp == 0x4e00, "three bytes decode to U+4E00, got %u", cp);

    n = fymd_utf8_decode("\xf0\x9f\x98\x80", 4, &cp);
    CHECK(n == 4 && cp == 0x1f600, "four bytes decode to U+1F600, got %u", cp);

    /* a caller must always advance, whatever the bytes are */
    n = fymd_utf8_decode("\xff", 1, &cp);
    CHECK(n == 1, "a malformed lead byte still advances");
    n = fymd_utf8_decode("\xe4\xb8", 2, &cp);
    CHECK(n == 1, "a truncated sequence still advances");
    n = fymd_utf8_decode("", 0, &cp);
    CHECK(n == 0, "an empty input consumes nothing");
}

static void test_str_width(void)
{
    CHECK(fymd_str_width("hello", 5) == 5, "five ASCII are five columns");
    CHECK(fymd_str_width("hello", (size_t)-1) == 5, "-1 measures to the NUL");
    CHECK(fymd_str_width(NULL, 0) == 0, "NULL measures as nothing");
    CHECK(fymd_str_width("", 0) == 0, "an empty string is no columns");

    /* one ideograph is two columns, not one codepoint and not three bytes */
    CHECK(fymd_str_width("\xe4\xb8\x80", 3) == 2, "one ideograph is two");
    CHECK(fymd_str_width("a\xe4\xb8\x80z", 5) == 4, "mixed text sums");

    /* e + combining acute is one column, e-acute precomposed also one */
    CHECK(fymd_str_width("e\xcc\x81", 3) == 1, "a combined e-acute is one");
    CHECK(fymd_str_width("\xc3\xa9", 2) == 1, "a precomposed e-acute is one");

    /* the length is in bytes; a sequence cut short by it measures as the
     * malformed bytes it leaves, one column each, and does not run away */
    CHECK(fymd_str_width("\xe4\xb8\x80", 2) == 2,
          "two bytes of a cut ideograph are two columns, got %zu",
          fymd_str_width("\xe4\xb8\x80", 2));
}

int main(void)
{
    test_cp_width();
    test_utf8_decode();
    test_str_width();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("width api: all checks passed\n");
    return 0;
}
