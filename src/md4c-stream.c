/*
 * MD4C: Markdown parser for C
 * (http://github.com/unjs/md4c)
 *
 * Copyright (c) 2026 Pooya Parsa <pooya@pi0.io>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>

#include "md4c-stream.h"
#include "md4c-ansi.h"

/******************************
 ***  Growable byte buffer  ***
 ******************************/

typedef struct {
    char* data;
    size_t size; /* used */
    size_t cap;  /* allocated */
    int error;   /* sticky allocation failure flag */
} STREAM_BUF;

static void
sbuf_init(STREAM_BUF* b)
{
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
    b->error = 0;
}

static void
sbuf_fini(STREAM_BUF* b)
{
    free(b->data);
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

static void
sbuf_reset(STREAM_BUF* b)
{
    b->size = 0;
    b->error = 0;
}

static int
sbuf_append(STREAM_BUF* b, const char* data, size_t size)
{
    if(size == 0)
        return 0;
    if(b->size + size > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        char* p;
        while(nc < b->size + size) nc *= 2;
        p = (char*) realloc(b->data, nc);
        if(p == NULL) { b->error = 1; return -1; }
        b->data = p;
        b->cap = nc;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}

/* process_output callback for the ANSI renderer: append into a STREAM_BUF. */
static void
sbuf_sink(const char* text, unsigned size, void* userdata)
{
    sbuf_append((STREAM_BUF*) userdata, text, (size_t) size);
}

/********************
 ***  Context     ***
 ********************/

struct MD4C_STREAM {
    unsigned parser_flags;
    unsigned renderer_flags;
    int width;            /* resolved (AUTO replaced by a concrete value at create) */
    int heal;
    int max_active_lines; /* 0 => unlimited; else cap active region to this many input lines */
    const MD_ANSI_STYLE* style;  /* borrowed styling context (may be NULL) */

    STREAM_BUF accum;     /* accumulated input */
    STREAM_BUF tail;      /* render of accum[anchor:] (active region) */
    STREAM_BUF seg;       /* render of a candidate committed segment */
    STREAM_BUF out;       /* slice returned to the caller (committed/preview/finish) */
    STREAM_BUF shown;     /* active region currently displayed (for md4c_stream_render) */
    size_t anchor;        /* input offset of the last confirmed sync point */
    int emitted;          /* non-zero once any committed output has been returned */
    int finished;
};

/* Render `input_size` bytes of input into `buf`. When `heal` is set, the
 * renderer's heal-before-render path closes dangling markers first. 0 / -1. */
static int
stream_render(MD4C_STREAM* s, STREAM_BUF* buf, const char* input,
              size_t input_size, int heal)
{
    unsigned flags = s->renderer_flags;
    int ret;

    if(heal)
        flags |= MD_ANSI_FLAG_HEAL;
    else
        flags &= ~(unsigned) MD_ANSI_FLAG_HEAL;

    sbuf_reset(buf);
    ret = md_ansi_ex_styled(input, (unsigned) input_size,
                            sbuf_sink, buf, s->parser_flags, flags, s->width, s->style);
    if(ret != 0 || buf->error)
        return -1;
    return 0;
}

/* Does the line [s, s+n) begin a list item (after optional indentation)? */
static int
line_is_list_item(const char* s, size_t n)
{
    size_t i = 0, j;
    while(i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if(i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+')) {
        return (i + 1 >= n) || s[i + 1] == ' ' || s[i + 1] == '\t';
    }
    j = i;
    while(j < n && s[j] >= '0' && s[j] <= '9') j++;
    return (j > i && j < n && (s[j] == '.' || s[j] == ')'));
}

/* Case-insensitive test: do the first bytes of s equal the lowercase literal? */
static int
ci_prefix(const char* s, size_t slen, const char* lit)
{
    size_t i;
    for(i = 0; lit[i] != '\0'; i++) {
        char c;
        if(i >= slen) return 0;
        c = s[i];
        if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if(c != lit[i]) return 0;
    }
    return 1;
}

/* Case-insensitive search for the lowercase literal `lit` anywhere in s. */
static int
ci_contains(const char* s, size_t slen, const char* lit)
{
    size_t litlen = strlen(lit), i;
    if(slen < litlen) return 0;
    for(i = 0; i + litlen <= slen; i++)
        if(ci_prefix(s + i, slen - i, lit))
            return 1;
    return 0;
}

/* If a leading-whitespace-trimmed line begins an HTML block that may span blank
 * lines (CommonMark HTML block types 1-5), return its end-condition code (1-5),
 * else 0. Types 6-7 are deliberately excluded: a blank line legitimately closes
 * them, so it stays a valid sync point. */
static int
html_block_start_type(const char* s, size_t m)
{
    static const char* const names[] = { "pre", "script", "style", "textarea" };
    size_t j;
    if(m < 2 || s[0] != '<') return 0;
    if(m >= 4 && s[1] == '!' && s[2] == '-' && s[3] == '-') return 2;   /* <!--     */
    if(ci_prefix(s, m, "<![cdata[")) return 5;                          /* <![CDATA[ */
    if(s[1] == '?') return 3;                                           /* <?       */
    if(m >= 3 && s[1] == '!'
       && ((s[2] >= 'A' && s[2] <= 'Z') || (s[2] >= 'a' && s[2] <= 'z')))
        return 4;                                                      /* <!LETTER */
    for(j = 0; j < sizeof(names) / sizeof(names[0]); j++) {            /* type 1   */
        size_t len = strlen(names[j]);
        if(m - 1 >= len && ci_prefix(s + 1, m - 1, names[j])) {
            char c = (m - 1 > len) ? s[1 + len] : '\0';
            if(c == '\0' || c == ' ' || c == '\t' || c == '>')
                return 1;
        }
    }
    return 0;
}

/* Does this line satisfy the end condition of an open type-`type` HTML block? */
static int
html_block_ends(const char* line, size_t llen, int type)
{
    switch(type) {
        case 1: return ci_contains(line, llen, "</pre>")
                    || ci_contains(line, llen, "</script>")
                    || ci_contains(line, llen, "</style>")
                    || ci_contains(line, llen, "</textarea>");
        case 2: return ci_contains(line, llen, "-->");
        case 3: return ci_contains(line, llen, "?>");
        case 4: return ci_contains(line, llen, ">");
        case 5: return ci_contains(line, llen, "]]>");
    }
    return 0;
}

/* Furthest "safe sync point" offset greater than `from`: the offset just after
 * a blank line that (a) is not inside a fenced code block, and (b) is followed
 * by a complete next line that starts at column 0, is non-blank and is not a
 * list item. At such a point all block containers are closed, so the renderer
 * is in its initial state and the suffix renders standalone identically to the
 * tail of a full render. Returns `from` if there is no such point. */
static size_t
next_sync_offset(const char* data, size_t size, size_t from)
{
    size_t i = 0, line_start = 0, best = from;
    int in_fence = 0;
    char fence_ch = 0;
    int in_html = 0, html_type = 0;

    while(i <= size) {
        if(i == size || data[i] == '\n') {
            const char* line = data + line_start;
            size_t llen = (size_t)(i - line_start);
            size_t k = 0;
            int is_fence;
            while(k < llen && (line[k] == ' ' || line[k] == '\t')) k++;
            is_fence = (llen - k >= 3)
                && ((line[k] == '`' && line[k + 1] == '`' && line[k + 2] == '`')
                    || (line[k] == '~' && line[k + 1] == '~' && line[k + 2] == '~'));

            if(in_fence) {
                if(is_fence && line[k] == fence_ch) in_fence = 0;
            } else if(in_html) {
                /* A blank line inside a type 1-5 HTML block does not close it,
                 * so it is not a sync point; wait for the block's end marker. */
                if(html_block_ends(line, llen, html_type)) in_html = 0;
            } else if(is_fence) {
                in_fence = 1;
                fence_ch = line[k];
            } else {
                int t = html_block_start_type(line + k, llen - k);
                if(t) {
                    /* Enter the block unless it also ends on this same line. */
                    if(!html_block_ends(line, llen, t)) { in_html = 1; html_type = t; }
                } else if(i < size) {
                    /* Blank line (only spaces/tabs/CR)? */
                    int blank = 1;
                    size_t b = line_start;
                    while(b < i) {
                        char c = data[b];
                        if(c != ' ' && c != '\t' && c != '\r') { blank = 0; break; }
                        b++;
                    }
                    if(blank) {
                        size_t nxt = i + 1;
                        if(nxt < size && data[nxt] != ' ' && data[nxt] != '\t' && data[nxt] != '\n') {
                            const char* nl = (const char*) memchr(data + nxt, '\n', size - nxt);
                            if(nl != NULL && !line_is_list_item(data + nxt, (size_t)(nl - (data + nxt))))
                                best = nxt;   /* keep the furthest qualifying point */
                        }
                    }
                }
            }
            line_start = i + 1;
        }
        i++;
    }
    return best;
}

/* Forced cap: if the active region [from, size) spans more than `cap` input
 * lines, return the offset (at a line boundary, > from) that drops the oldest
 * excess lines so exactly `cap` lines remain; otherwise return `from`. This is
 * not a safe sync point -- it bounds the per-push re-render and heal scan when a
 * single block never closes, accepting that a construct straddling the cut is
 * frozen mid-way. `cap` must be > 0.
 *
 * A single line with no newlines (e.g. a long unbroken paragraph) has no line
 * boundary to drop, so a byte budget (cap rows worth of text, ~cap*width bytes)
 * is the safety net: once the region exceeds it, cut at the last whitespace at
 * or under the budget, force-splitting the paragraph. `width` is the resolved
 * render width. */
static size_t
forced_cut_offset(const char* data, size_t size, size_t from, int cap, int width)
{
    size_t i, total = 0, dropped = 0, cut = from, budget;

    for(i = from; i < size; i++)
        if(data[i] == '\n') total++;
    if(total > (size_t) cap) {
        size_t excess = total - (size_t) cap;
        for(i = from; i < size; i++) {
            if(data[i] == '\n' && ++dropped == excess) { cut = i + 1; break; }
        }
    }

    /* Byte-budget safety net for the region remaining after the line drop. */
    budget = (size_t) cap * (size_t)(width > 0 ? width : 80);
    if(size - cut > budget) {
        size_t target = size - budget;   /* keep ~budget trailing bytes */
        size_t ws = 0;
        for(i = cut; i < target; i++)
            if(data[i] == ' ' || data[i] == '\t' || data[i] == '\n') ws = i + 1;
        cut = ws > cut ? ws : target;    /* whitespace split, else a hard cut */
    }
    return cut;
}

MD4C_STREAM*
md4c_stream_create(const MD4C_STREAM_OPTS* opts)
{
    MD4C_STREAM* s = (MD4C_STREAM*) calloc(1, sizeof(MD4C_STREAM));
    int width;

    if(s == NULL)
        return NULL;

    if(opts != NULL) {
        s->parser_flags = opts->parser_flags;
        s->renderer_flags = opts->renderer_flags;
        width = opts->width;
        s->heal = opts->heal;
        s->max_active_lines = opts->max_active_lines > 0 ? opts->max_active_lines : 0;
        s->style = opts->style;
    } else {
        s->parser_flags = MD4C_ANSI_PARSER_FLAGS;
        s->renderer_flags = 0;
        width = MD_ANSI_WIDTH_AUTO;
        s->heal = 1;
        s->max_active_lines = 0;
        s->style = NULL;
    }
    if(opts != NULL && opts->parser_flags == 0)
        s->parser_flags = MD4C_ANSI_PARSER_FLAGS;

    /* Resolve AUTO to a concrete width once, so a mid-stream terminal resize
     * cannot change the layout (and thus the bytes) of already-committed output. */
    if(width == MD_ANSI_WIDTH_AUTO)
        width = md_ansi_detect_width();
    s->width = width;

    sbuf_init(&s->accum);
    sbuf_init(&s->tail);
    sbuf_init(&s->seg);
    sbuf_init(&s->out);
    sbuf_init(&s->shown);
    s->anchor = 0;
    s->emitted = 0;
    s->finished = 0;
    return s;
}

void
md4c_stream_destroy(MD4C_STREAM* s)
{
    if(s == NULL)
        return;
    sbuf_fini(&s->accum);
    sbuf_fini(&s->tail);
    sbuf_fini(&s->seg);
    sbuf_fini(&s->out);
    sbuf_fini(&s->shown);
    free(s);
}

/* Build the returned slice in s->out: an inter-block "\n" separator (when some
 * committed output already preceded it) followed by `data`. */
static int
stream_emit(MD4C_STREAM* s, const char* data, size_t size)
{
    sbuf_reset(&s->out);
    if(s->emitted && sbuf_append(&s->out, "\n", 1) != 0)
        return -1;
    if(sbuf_append(&s->out, data, size) != 0)
        return -1;
    return 0;
}

/* Append another committed segment to s->out (without resetting it), with the
 * inter-block separator. Sets s->emitted so a following segment is separated. */
static int
stream_emit_append(MD4C_STREAM* s, const char* data, size_t size)
{
    if(s->emitted && sbuf_append(&s->out, "\n", 1) != 0)
        return -1;
    if(sbuf_append(&s->out, data, size) != 0)
        return -1;
    s->emitted = 1;
    return 0;
}

int
md4c_stream_push(MD4C_STREAM* s, const char* chunk, size_t len,
                 const char** out, size_t* out_len)
{
    size_t sync;

    if(out) *out = NULL;
    if(out_len) *out_len = 0;
    if(s == NULL || s->finished)
        return -1;

    if(len > 0 && sbuf_append(&s->accum, chunk, len) != 0)
        return -1;

    sbuf_reset(&s->out);

    /* Forced cap: when the active region exceeds max_active_lines input lines
     * (a block that never reaches a sync point), force-commit the oldest excess
     * lines at a line boundary so the re-render below stays O(cap). The dropped
     * segment is committed as the unhealed render of itself. */
    if(s->max_active_lines > 0) {
        size_t cut = forced_cut_offset(s->accum.data, s->accum.size,
                                       s->anchor, s->max_active_lines, s->width);
        if(cut > s->anchor) {
            if(stream_render(s, &s->seg, s->accum.data + s->anchor,
                             cut - s->anchor, 0) != 0)
                return -1;
            if(stream_emit_append(s, s->seg.data, s->seg.size) != 0)
                return -1;
            s->anchor = cut;
        }
    }

    /* Render the active region (from the last sync point) for verification. */
    if(stream_render(s, &s->tail, s->accum.data + s->anchor,
                     s->accum.size - s->anchor, 0) != 0)
        return -1;

    /* Try to advance the anchor to the furthest safe sync point. Commit the
     * segment between the old and new anchor, but only after verifying its
     * standalone render is a true prefix of the active-region render (so a
     * construct that unexpectedly spans the point is never committed). */
    sync = next_sync_offset(s->accum.data, s->accum.size, s->anchor);
    if(sync > s->anchor) {
        if(stream_render(s, &s->seg, s->accum.data + s->anchor,
                         sync - s->anchor, 0) != 0)
            return -1;
        if(s->seg.size <= s->tail.size
           && memcmp(s->seg.data, s->tail.data, s->seg.size) == 0) {
            if(stream_emit_append(s, s->seg.data, s->seg.size) != 0)
                return -1;
            s->anchor = sync;
        }
    }

    if(out) *out = s->out.data;
    if(out_len) *out_len = s->out.size;
    return 0;
}

int
md4c_stream_preview(MD4C_STREAM* s, const char** out, size_t* out_len)
{
    if(out) *out = NULL;
    if(out_len) *out_len = 0;
    if(s == NULL)
        return -1;

    /* Healed render of the active region (everything after the last commit),
     * with the inter-block separator so it sits below committed output. */
    if(stream_render(s, &s->tail, s->accum.data + s->anchor,
                     s->accum.size - s->anchor, s->heal) != 0)
        return -1;
    if(stream_emit(s, s->tail.data, s->tail.size) != 0)
        return -1;

    if(out) *out = s->out.data;
    if(out_len) *out_len = s->out.size;
    return 0;
}

int
md4c_stream_finish(MD4C_STREAM* s, const char** out, size_t* out_len)
{
    if(out) *out = NULL;
    if(out_len) *out_len = 0;
    if(s == NULL)
        return -1;

    /* Render the remaining active region (optionally healed) as the final
     * segment, after the last committed sync point. */
    if(stream_render(s, &s->tail, s->accum.data + s->anchor,
                     s->accum.size - s->anchor, s->heal) != 0)
        return -1;
    if(stream_emit(s, s->tail.data, s->tail.size) != 0)
        return -1;

    s->anchor = s->accum.size;
    s->emitted = 1;
    s->finished = 1;

    if(out) *out = s->out.data;
    if(out_len) *out_len = s->out.size;
    return 0;
}

/* Number of '\n' characters in [data, data+size). */
static size_t
count_lines(const char* data, size_t size)
{
    size_t i, n = 0;
    for(i = 0; i < size; i++)
        if(data[i] == '\n') n++;
    return n;
}

/* Byte offset just past the first `n` newlines of [data, data+size). If there
 * are fewer than `n` newlines, returns `size`. */
static size_t
offset_after_lines(const char* data, size_t size, size_t n)
{
    size_t i, seen = 0;
    if(n == 0)
        return 0;
    for(i = 0; i < size; i++) {
        if(data[i] == '\n' && ++seen == n)
            return i + 1;
    }
    return size;
}

/* Byte offset just past the longest common run of whole lines of `a` and `b`. */
static size_t
common_line_prefix(const STREAM_BUF* a, const STREAM_BUF* b)
{
    size_t n = (a->size < b->size) ? a->size : b->size;
    size_t i = 0, line_start = 0;
    while(i < n && a->data[i] == b->data[i]) {
        if(a->data[i] == '\n') line_start = i + 1;
        i++;
    }
    return line_start;
}

int
md4c_stream_render(MD4C_STREAM* s, const char* chunk, size_t len,
                   MD4C_STREAM_UPDATE* upd)
{
    size_t line_start, sync, freeze_bytes = 0;

    if(upd != NULL) {
        upd->backtrack = 0;
        upd->content = NULL;
        upd->content_len = 0;
        upd->freeze = 0;
    }
    if(s == NULL || s->finished || upd == NULL)
        return -1;

    if(len > 0 && sbuf_append(&s->accum, chunk, len) != 0)
        return -1;

    /* Render the active region for DISPLAY (healed iff opts.heal, so dangling
     * markers in the in-progress tail show closed) into s->seg, then compose
     * s->tail = leading inter-block separator (a blank line, present once
     * anything has been committed above) + that render. The separator is what
     * joins the active region to the committed output. */
    if(stream_render(s, &s->seg, s->accum.data + s->anchor,
                     s->accum.size - s->anchor, s->heal) != 0)
        return -1;
    sbuf_reset(&s->tail);
    if(s->emitted && sbuf_append(&s->tail, "\n", 1) != 0)
        return -1;
    if(sbuf_append(&s->tail, s->seg.data, s->seg.size) != 0)
        return -1;

    /* Diff against what is currently displayed: keep the common leading lines,
     * backtrack over the rest, and re-emit from the first changed line. */
    line_start = common_line_prefix(&s->shown, &s->tail);
    upd->backtrack = count_lines(s->shown.data + line_start, s->shown.size - line_start);
    upd->content = s->tail.data + line_start;
    upd->content_len = s->tail.size - line_start;

    /* Advance the commit anchor to the furthest verified safe sync point. The
     * committed segment uses the UNHEALED render (the truth), and is committed
     * only when it is a true prefix of the displayed (possibly healed) render —
     * so a line is frozen only where healing did not change it. The frozen lines
     * (including the leading separator) become permanent (reported via freeze)
     * and drop out of the mutable active region. */
    sync = next_sync_offset(s->accum.data, s->accum.size, s->anchor);
    if(sync > s->anchor) {
        size_t sep = s->emitted ? 1 : 0;
        if(stream_render(s, &s->out, s->accum.data + s->anchor,
                         sync - s->anchor, 0) != 0)
            return -1;
        if(s->out.size <= s->seg.size
           && memcmp(s->out.data, s->seg.data, s->out.size) == 0) {
            freeze_bytes = sep + s->out.size;
            upd->freeze = count_lines(s->tail.data, freeze_bytes);
            s->anchor = sync;
            s->emitted = 1;
        }
    }

    /* Forced cap: if no safe sync point froze anything yet the active region
     * exceeds max_active_lines, force-freeze the oldest excess lines. They drop
     * out of the mutable region (future backtrack can never reach them); the cut
     * is not a safe sync point, so a straddling construct freezes mid-way. Freeze
     * is counted in DISPLAY lines: the unhealed render of the dropped input gives
     * the line count, and the matching prefix of the (healed) display tail is
     * frozen so the on-screen bytes stay consistent. */
    if(s->max_active_lines > 0 && freeze_bytes == 0) {
        size_t cut = forced_cut_offset(s->accum.data, s->accum.size,
                                       s->anchor, s->max_active_lines, s->width);
        if(cut > s->anchor) {
            size_t sep = s->emitted ? 1 : 0;
            size_t dropped_lines, body;
            if(stream_render(s, &s->out, s->accum.data + s->anchor,
                             cut - s->anchor, 0) != 0)
                return -1;
            dropped_lines = count_lines(s->out.data, s->out.size);
            body = offset_after_lines(s->seg.data, s->seg.size, dropped_lines);
            freeze_bytes = sep + body;
            upd->freeze = count_lines(s->tail.data, freeze_bytes);
            s->anchor = cut;
            s->emitted = 1;
        }
    }

    /* Remember the now-displayed active region (minus the frozen prefix). */
    sbuf_reset(&s->shown);
    if(sbuf_append(&s->shown, s->tail.data + freeze_bytes,
                   s->tail.size - freeze_bytes) != 0)
        return -1;

    return 0;
}
