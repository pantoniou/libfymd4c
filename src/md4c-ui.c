/*
 * md4c-ui.c - UI Markdown: fy-* tags, layout markers and clickable regions
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "md4c-ui.h"

void
md_ui_reset(MD_ANSI_UI* ui)
{
    size_t i;

    if(ui == NULL)
        return;
    for(i = 0; i < ui->count; i++)
        free(ui->regions[i].id);
    ui->count = 0;
}

void
md_ui_fini(MD_ANSI_UI* ui)
{
    if(ui == NULL)
        return;
    md_ui_reset(ui);
    free(ui->regions);
    memset(ui, 0, sizeof(*ui));
}

int
md_ui_region_add(MD_ANSI_UI* ui, const char* id, size_t id_len, size_t row,
                 int col, int width, int height, int kind)
{
    MD_ANSI_REGION* nr;
    char* copy;
    size_t na;

    if(ui == NULL || width <= 0)
        return 0;
    if(ui->count == ui->alloc) {
        na = ui->alloc ? ui->alloc * 2 : 16;
        nr = (MD_ANSI_REGION*) realloc(ui->regions, na * sizeof(*nr));
        if(nr == NULL)
            return -1;
        ui->regions = nr;
        ui->alloc = na;
    }
    copy = (char*) malloc(id_len + 1);
    if(copy == NULL)
        return -1;
    memcpy(copy, id, id_len);
    copy[id_len] = '\0';
    ui->regions[ui->count].id = copy;
    ui->regions[ui->count].row = row;
    ui->regions[ui->count].col = col;
    ui->regions[ui->count].width = width;
    ui->regions[ui->count].height = height;
    ui->regions[ui->count].kind = kind;
    ui->count++;
    return 0;
}

int
md_ui_arg_nums(const char* arg, size_t len, size_t* id_len, int* nums, int n)
{
    size_t start, end = len, i;
    int k;

    if(arg == NULL || n <= 0)
        return -1;
    for(k = n - 1; k >= 0; k--) {
        start = end;
        while(start > 0 && arg[start - 1] >= '0' && arg[start - 1] <= '9')
            start--;
        if(start == end)
            return -1;
        for(nums[k] = 0, i = start; i < end; i++)
            if(nums[k] < 1000000)
                nums[k] = nums[k] * 10 + (arg[i] - '0');
        if(k > 0) {
            if(start == 0 || arg[start - 1] != ':')
                return -1;
            end = start - 1;
        } else {
            end = start;
        }
    }
    if(end == 0) {
        *id_len = 0;
        return 0;
    }
    if(arg[end - 1] != ':')
        return -1;
    *id_len = end - 1;
    return 0;
}

int
md_ui_slot_arg(const char* arg, size_t len, size_t* id_len, int* a, int* b)
{
    int nums[2];

    if(md_ui_arg_nums(arg, len, id_len, nums, 2) != 0 || *id_len == 0)
        return -1;
    *a = nums[0];
    *b = nums[1];
    return 0;
}

void
md_ui_share(int total, const int* weight, const int* min, int n, int* out)
{
    char pinned[64];
    long rest, w, base, rem, best_rem;
    int i, best, changed, left;

    if(n <= 0)
        return;
    if(n > (int) sizeof(pinned))
        n = (int) sizeof(pinned);
    memset(pinned, 0, sizeof(pinned));
    for(i = 0; i < n; i++)
        out[i] = 0;
    for(;;) {
        rest = total;
        w = 0;
        for(i = 0; i < n; i++) {
            if(pinned[i])
                rest -= out[i];
            else
                w += weight[i] > 0 ? weight[i] : 0;
        }
        if(rest < 0)
            rest = 0;
        changed = 0;
        for(i = 0; i < n && min != NULL; i++) {
            if(pinned[i])
                continue;
            base = w > 0 ? rest * (weight[i] > 0 ? weight[i] : 0) / w : 0;
            if(base < min[i]) {
                out[i] = min[i];
                pinned[i] = 1;
                changed = 1;
            }
        }
        if(changed)
            continue;
        if(w == 0)
            return;
        /* largest remainders: the parts add up to what is left */
        left = (int) rest;
        for(i = 0; i < n; i++) {
            if(pinned[i])
                continue;
            out[i] = (int)(rest * (weight[i] > 0 ? weight[i] : 0) / w);
            left -= out[i];
        }
        while(left > 0) {
            best = -1;
            best_rem = -1;
            for(i = 0; i < n; i++) {
                if(pinned[i] || weight[i] <= 0)
                    continue;
                rem = rest * weight[i] % w;
                if(rem > best_rem) {
                    best_rem = rem;
                    best = i;
                }
            }
            if(best < 0)
                break;
            out[best]++;
            pinned[best] = 2;   /* one extra row at most */
            left--;
        }
        for(i = 0; i < n; i++)
            if(pinned[i] == 2)
                pinned[i] = 0;
        return;
    }
}

static int
region_cmp(const void* pa, const void* pb)
{
    const MD_ANSI_REGION* a = (const MD_ANSI_REGION*) pa;
    const MD_ANSI_REGION* b = (const MD_ANSI_REGION*) pb;

    if(a->row != b->row)
        return a->row < b->row ? -1 : 1;
    return a->col < b->col ? -1 : a->col > b->col;
}

static int
ui_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int
ui_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int
md_ui_tag_parse(const char* s, size_t n, MD_UI_TAG* t)
{
    size_t i = 0, start;
    MD_UI_ATTR* a;
    char q;

    memset(t, 0, sizeof(*t));
    if(n < 5 || s[0] != '<')
        return -1;
    i = 1;
    if(s[i] == '/') {
        t->closing = 1;
        i++;
    }
    if(i + 3 > n || strncmp(s + i, "fy-", 3) != 0)
        return -1;
    i += 3;
    start = i;
    while(i < n && ui_name_char(s[i]))
        i++;
    if(i == start)
        return -1;
    t->name = s + start;
    t->name_len = i - start;
    for(;;) {
        while(i < n && ui_space(s[i]))
            i++;
        if(i >= n)
            return -1;
        if(s[i] == '>') {
            t->len = i + 1;
            return 0;
        }
        if(s[i] == '/' && i + 1 < n && s[i + 1] == '>') {
            t->self_closing = 1;
            t->len = i + 2;
            return 0;
        }
        if(t->closing)
            return -1;
        start = i;
        while(i < n && ui_name_char(s[i]))
            i++;
        if(i == start)
            return -1;
        if(t->n_attrs >= MD_UI_ATTR_MAX)
            return -1;
        a = &t->attrs[t->n_attrs++];
        a->name = s + start;
        a->name_len = i - start;
        if(i < n && s[i] == '=') {
            i++;
            if(i < n && (s[i] == '"' || s[i] == '\'')) {
                q = s[i++];
                start = i;
                while(i < n && s[i] != q)
                    i++;
                if(i >= n)
                    return -1;
                a->value = s + start;
                a->value_len = i - start;
                i++;
            } else {
                start = i;
                while(i < n && !ui_space(s[i]) && s[i] != '>' && s[i] != '/')
                    i++;
                a->value = s + start;
                a->value_len = i - start;
            }
        } else {
            a->value = "";
            a->value_len = 0;
        }
    }
}

int
md_ui_tag_is(const MD_UI_TAG* t, const char* name)
{
    return strlen(name) == t->name_len && memcmp(t->name, name, t->name_len) == 0;
}

const char*
md_ui_tag_attr(const MD_UI_TAG* t, const char* name, size_t* len)
{
    int i;

    for(i = 0; i < t->n_attrs; i++) {
        if(strlen(name) == t->attrs[i].name_len &&
           memcmp(t->attrs[i].name, name, t->attrs[i].name_len) == 0) {
            *len = t->attrs[i].value_len;
            return t->attrs[i].value;
        }
    }
    return NULL;
}

int
md_ui_id_valid(const char* s, size_t n)
{
    size_t i;

    if(s == NULL || n == 0 || n > 63)
        return 0;
    for(i = 0; i < n; i++)
        if(!ui_name_char(s[i]) && s[i] != '.' && s[i] != ':' && s[i] != '/')
            return 0;
    return 1;
}

size_t
md_ui_marker(const char* s, size_t n, const char** kind, size_t* kind_len,
             const char** arg, size_t* arg_len)
{
    size_t i;

    if(n < MD_UI_MARK_OPEN_LEN + 2 ||
       memcmp(s, MD_UI_MARK_OPEN, MD_UI_MARK_OPEN_LEN) != 0)
        return 0;
    for(i = MD_UI_MARK_OPEN_LEN; i + 1 < n; i++)
        if(s[i] == 0x1b && s[i + 1] == '\\')
            break;
    if(i + 1 >= n)
        return 0;
    *kind = s + MD_UI_MARK_OPEN_LEN;
    *kind_len = i - MD_UI_MARK_OPEN_LEN;
    *arg = NULL;
    *arg_len = 0;
    {
        const char* eq = (const char*) memchr(*kind, '=', *kind_len);
        if(eq != NULL) {
            *arg = eq + 1;
            *arg_len = *kind_len - (size_t)(eq + 1 - *kind);
            *kind_len = (size_t)(eq - *kind);
        }
    }
    return i + 2;
}

int
md_ui_marker_is(const char* kind, size_t kind_len, const char* name)
{
    return strlen(name) == kind_len && memcmp(kind, name, kind_len) == 0;
}

/* A row of the input. */
typedef struct {
    size_t start;
    size_t len;         /* without the newline */
    int newline;
    int type;           /* ROW_CONTENT, ROW_VFILL, ROW_SCROLL, ROW_END */
    int anchor_top;     /* ROW_SCROLL */
    int keep;           /* ROW_CONTENT */
    size_t pad;         /* ROW_VFILL: blank rows it becomes */
    const char* slot_id;/* ROW_VFILL of an elastic fy-slot, or NULL */
    size_t slot_id_len;
    int slot_col;
    int slot_width;
    size_t slot_row;    /* the first output row of its blank rows */
    int weight;         /* ROW_VFILL: its share of the rows left over */
    int min;            /* ROW_VFILL: the rows it keeps */
} UI_ROW;

enum { ROW_CONTENT, ROW_VFILL, ROW_SCROLL, ROW_END };

int
md_ui_vertical(const char* in, size_t len, int height, MD_ANSI_UI* ui,
               char** out, size_t* out_len)
{
    UI_ROW* rows = NULL;
    size_t nrows = 0, arows = 0, i, j, k, content = 0, nvfill = 0, minsum = 0, over;
    size_t* map = NULL;
    size_t pos, mlen, kl, al, body, take, o, newrow;
    const char* kind;
    const char* arg;
    char* buf = NULL;
    UI_ROW* nr;

    /* split into rows */
    for(pos = 0; pos < len; ) {
        const char* nl = (const char*) memchr(in + pos, '\n', len - pos);
        size_t end = nl ? (size_t)(nl - in) : len;
        if(nrows == arows) {
            arows = arows ? arows * 2 : 64;
            nr = (UI_ROW*) realloc(rows, arows * sizeof(*rows));
            if(nr == NULL)
                goto err;
            rows = nr;
        }
        memset(&rows[nrows], 0, sizeof(rows[nrows]));
        rows[nrows].start = pos;
        rows[nrows].len = end - pos;
        rows[nrows].newline = nl != NULL;
        rows[nrows].type = ROW_CONTENT;
        rows[nrows].keep = 1;
        mlen = md_ui_marker(in + pos, end - pos, &kind, &kl, &arg, &al);
        if(mlen != 0 && mlen == end - pos) {
            int nums[4];
            size_t idl;
            if(md_ui_marker_is(kind, kl, "vfill")) {
                rows[nrows].type = ROW_VFILL;
                rows[nrows].weight = 1;
                if(arg != NULL && md_ui_arg_nums(arg, al, &idl, nums, 2) == 0) {
                    rows[nrows].weight = nums[0];
                    rows[nrows].min = nums[1];
                }
                nvfill++;
            } else if(md_ui_marker_is(kind, kl, "vslot") &&
                      md_ui_arg_nums(arg, al, &idl, nums, 4) == 0 && idl > 0) {
                /* an elastic slot takes its share like a vfill */
                rows[nrows].type = ROW_VFILL;
                rows[nrows].slot_id = arg;
                rows[nrows].slot_id_len = idl;
                rows[nrows].slot_col = nums[0];
                rows[nrows].slot_width = nums[1];
                rows[nrows].weight = nums[2];
                rows[nrows].min = nums[3];
                nvfill++;
            } else if(md_ui_marker_is(kind, kl, "scroll")) {
                rows[nrows].type = ROW_SCROLL;
                rows[nrows].anchor_top = arg != NULL && al == 3 &&
                                         memcmp(arg, "top", 3) == 0;
            } else if(md_ui_marker_is(kind, kl, "/scroll")) {
                rows[nrows].type = ROW_END;
            }
        }
        if(rows[nrows].type == ROW_CONTENT)
            content++;
        nrows++;
        pos = end + (nl ? 1 : 0);
    }

    /* the minimum rows of the flexible rows are rows the page must hold */
    for(i = 0; i < nrows; i++)
        if(rows[i].type == ROW_VFILL)
            minsum += (size_t) rows[i].min;
    if(height > 0 && nvfill > 0 && content + minsum <= (size_t) height) {
        int* w = (int*) malloc(nvfill * sizeof(int));
        int* mn = (int*) malloc(nvfill * sizeof(int));
        int* share = (int*) malloc(nvfill * sizeof(int));
        if(w == NULL || mn == NULL || share == NULL) {
            free(w);
            free(mn);
            free(share);
            goto err;
        }
        for(i = 0, k = 0; i < nrows; i++)
            if(rows[i].type == ROW_VFILL) {
                w[k] = rows[i].weight;
                mn[k] = rows[i].min;
                k++;
            }
        md_ui_share((int)((size_t) height - content), w, mn, (int) nvfill, share);
        for(i = 0, k = 0; i < nrows; i++)
            if(rows[i].type == ROW_VFILL)
                rows[i].pad = (size_t) share[k++];
        free(w);
        free(mn);
        free(share);
    } else if(height > 0 && content + minsum > (size_t) height) {
        for(i = 0; i < nrows; i++)
            if(rows[i].type == ROW_VFILL)
                rows[i].pad = (size_t) rows[i].min;
        over = content + minsum - (size_t) height;
        for(i = 0; i < nrows && over > 0; i++) {
            if(rows[i].type != ROW_SCROLL)
                continue;
            for(j = i + 1, body = 0; j < nrows && rows[j].type != ROW_END; j++)
                if(rows[j].type == ROW_CONTENT)
                    body++;
            take = body < over ? body : over;
            over -= take;
            if(rows[i].anchor_top) {
                /* keep the top: take the last rows of the body */
                for(k = j; k > i + 1 && take > 0; k--)
                    if(rows[k - 1].type == ROW_CONTENT) {
                        rows[k - 1].keep = 0;
                        take--;
                    }
            } else {
                for(k = i + 1; k < j && take > 0; k++)
                    if(rows[k].type == ROW_CONTENT) {
                        rows[k].keep = 0;
                        take--;
                    }
            }
        }
    }

    map = (size_t*) malloc((nrows ? nrows : 1) * sizeof(*map));
    buf = (char*) malloc(len + (height > 0 ? (size_t) height : 0) + minsum + 1);
    if(map == NULL || buf == NULL)
        goto err;
    for(i = 0, o = 0, newrow = 0; i < nrows; i++) {
        map[i] = (size_t) -1;
        if(rows[i].type == ROW_VFILL) {
            rows[i].slot_row = newrow;
            for(k = 0; k < rows[i].pad; k++) {
                buf[o++] = '\n';
                newrow++;
            }
            continue;
        }
        if(rows[i].type != ROW_CONTENT || !rows[i].keep)
            continue;
        memcpy(buf + o, in + rows[i].start, rows[i].len);
        o += rows[i].len;
        map[i] = newrow;
        if(rows[i].newline) {
            buf[o++] = '\n';
            newrow++;
        }
    }
    buf[o] = '\0';

    if(ui != NULL) {
        for(i = 0, k = 0; i < ui->count; i++) {
            MD_ANSI_REGION rg = ui->regions[i];
            size_t h = rg.height > 0 ? (size_t) rg.height : 1, first = (size_t) -1, kept = 0;
            /* a region keeps the rows of it that stay, from the first */
            for(j = rg.row; j < rg.row + h && j < nrows; j++)
                if(map[j] != (size_t) -1) {
                    if(first == (size_t) -1)
                        first = map[j];
                    kept++;
                }
            if(first == (size_t) -1) {
                free(rg.id);
                continue;
            }
            rg.row = first;
            if(rg.height > 0)
                rg.height = (int) kept;
            ui->regions[k++] = rg;
        }
        ui->count = k;
        /* an elastic slot is the blank rows that the layout gave it */
        for(i = 0; i < nrows; i++)
            if(rows[i].slot_id != NULL)
                (void) md_ui_region_add(ui, rows[i].slot_id, rows[i].slot_id_len,
                                        rows[i].slot_row, rows[i].slot_col,
                                        rows[i].slot_width, (int) rows[i].pad,
                                        MD_UI_REGION_SLOT);
        if(ui->count > 1)
            qsort(ui->regions, ui->count, sizeof(ui->regions[0]), region_cmp);
    }
    free(rows);
    free(map);
    *out = buf;
    *out_len = o;
    return 0;

err:
    free(rows);
    free(map);
    free(buf);
    return -1;
}
