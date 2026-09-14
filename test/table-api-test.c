/*
 * table-api-test.c - where a table fitted to its content stands in the width
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

static const char table_md[] = "| name | value |\n|---|---|\n| a | 1 |\n";

/* Where the header row of the table stands: its leading blanks, and the
 * columns after them. The blanks at the end of the row pad its last cell, so
 * they count. */
struct row {
    int lead;
    int cols;
};

static int header_row(unsigned int flags, int width, struct row *row)
{
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    char *out, *line, *end, *last;
    int rc = -1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.width = width;
    cfg.flags = (enum fymd_cfg_flags)(FYMD_RF_NO_COLOR | flags);
    r = fymd_renderer_create(&cfg);
    if (!r)
        return -1;
    out = fymd_render_to_string(r, table_md, sizeof(table_md) - 1);
    fymd_renderer_destroy(r);
    if (!out)
        return -1;
    for (line = out; *line; line = *end ? end + 1 : end) {
        end = strchr(line, '\n');
        if (!end)
            end = line + strlen(line);
        if (!memmem(line, (size_t)(end - line), "name", 4))
            continue;
        for (row->lead = 0; line + row->lead < end &&
             line[row->lead] == ' '; row->lead++)
            ;
        last = end;
        row->cols = (int)fymd_str_width(line + row->lead,
                                        (size_t)(last - line - row->lead));
        rc = 0;
        break;
    }
    fymd_free(out);
    return rc;
}

static void test_fit_places_the_table(void)
{
    struct row fill, left, center, right;
    int edge, room;

    CHECK(!header_row(0, 60, &fill), "a filled table renders");
    CHECK(!header_row(FYMD_RF_TABLE_FIT, 60, &left),
          "a fitted table renders");
    CHECK(!header_row(FYMD_RF_TABLE_FIT | FYMD_RF_TABLE_CENTER, 60, &center),
          "a centred table renders");
    CHECK(!header_row(FYMD_RF_TABLE_FIT | FYMD_RF_TABLE_RIGHT, 60, &right),
          "a right table renders");

    /* a filled table reaches the right edge of the width */
    edge = fill.lead + fill.cols;
    CHECK(left.cols < fill.cols, "a fitted table is narrower: %d, %d",
          left.cols, fill.cols);
    room = edge - (left.lead + left.cols);

    CHECK(center.cols == left.cols && right.cols == left.cols,
          "the place does not change the columns: %d %d %d", left.cols,
          center.cols, right.cols);
    CHECK(center.lead == left.lead + room / 2,
          "a centred table has half the room on its left: %d, want %d",
          center.lead, left.lead + room / 2);
    CHECK(right.lead + right.cols == edge,
          "a right table ends at the edge: %d, want %d",
          right.lead + right.cols, edge);
}

static void test_place_needs_fit(void)
{
    struct row fill, center, right;

    CHECK(!header_row(0, 60, &fill), "a filled table renders");
    CHECK(!header_row(FYMD_RF_TABLE_CENTER, 60, &center),
          "a centred filled table renders");
    CHECK(!header_row(FYMD_RF_TABLE_RIGHT, 60, &right),
          "a right filled table renders");
    CHECK(center.lead == fill.lead && center.cols == fill.cols,
          "a filled table has no room to be centred");
    CHECK(right.lead == fill.lead && right.cols == fill.cols,
          "a filled table has no room to move right");
}

static void test_unlimited_width_keeps_the_table_left(void)
{
    struct row left, right;

    CHECK(!header_row(FYMD_RF_TABLE_FIT, FYMD_WIDTH_INF, &left),
          "a table without a width renders");
    CHECK(!header_row(FYMD_RF_TABLE_FIT | FYMD_RF_TABLE_RIGHT, FYMD_WIDTH_INF,
                      &right),
          "a right table without a width renders");
    CHECK(right.lead == left.lead,
          "without a width there is no edge to move to: %d, %d", right.lead,
          left.lead);
}

int main(void)
{
    test_fit_places_the_table();
    test_place_needs_fit();
    test_unlimited_width_keeps_the_table_left();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
