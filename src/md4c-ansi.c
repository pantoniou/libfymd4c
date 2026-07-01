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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(unix) || defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    #if !defined(__wasi__) && !defined(__wasm__)
        #define MD4C_ANSI_HAVE_IOCTL 1
        #include <unistd.h>
        #include <sys/ioctl.h>
    #endif
#endif

#include "md4c-ansi.h"
#include "md4c-heal-wrap.h"
#include "md4c-style.h"
#include "entity.h"

#ifdef MD4C_WITH_FYTS
    #include <fyts/fyts.h>
#endif


#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199409L
    #if defined __GNUC__
        #define inline __inline__
    #elif defined _MSC_VER
        #define inline __inline
    #else
        #define inline
    #endif
#endif

#ifdef _WIN32
    #define snprintf _snprintf
#endif


/* All element colours, attributes and glyphs come from the styling config
 * (md4c-style.h / r->style); see stylings/md4c-default.yaml. The only escape
 * sequences hardcoded here are the OSC 8 hyperlink framing (a terminal
 * protocol, not styling).
 *
 * OSC 8 hyperlinks: \033]8;;URL\033\\ to open, \033]8;;\033\\ to close */
#define ANSI_HYPERLINK_OPEN "\033]8;;"
#define ANSI_HYPERLINK_SEP  "\033\\"
#define ANSI_HYPERLINK_CLOSE "\033]8;;\033\\"

/* The table layout is modeled on the glow / charmbracelet lipgloss table
 * renderer (MIT licensed): content-sized columns, a single header separator,
 * vertical column separators, per-column alignment, and fit-to-terminal-width
 * with cell word-wrap. Separator/vertical/cross glyphs come from r->style. */

/* Document margin reserved on each side of every line (like glow). */
#define DOC_MARGIN          2


/* Code block metadata entry (heap-allocated when MD_ANSI_FLAG_CODE_META is set) */
typedef struct MD_ANSI_CODE_META {
    MD_SIZE start;          /* Byte offset: start of code block (before ANSI_DIM) */
    MD_SIZE end;            /* Byte offset: end of code block (after ANSI_DIM_OFF) */
    char lang[64];
    MD_SIZE lang_size;
    char filename[256];
    MD_SIZE filename_size;
    unsigned* highlights;
    unsigned highlight_count;
    char prefix[256];       /* Line indent prefix (captured from render_indent + "  ") */
    MD_SIZE prefix_size;
} MD_ANSI_CODE_META;

/* Buffered table cell/row/table state. Cell content (including ANSI escapes
 * from inline spans) is captured into per-cell buffers while the table is
 * parsed, then laid out and emitted when the table block closes. */
typedef struct MD_ANSI_TCELL {
    char* buf;
    MD_SIZE size;
    MD_SIZE cap;
} MD_ANSI_TCELL;

typedef struct MD_ANSI_TROW {
    MD_ANSI_TCELL* cells;
    int n_cells;
    int cap_cells;
    int is_header;
} MD_ANSI_TROW;

typedef struct MD_ANSI_TABLE {
    MD_ANSI_TROW* rows;
    int n_rows;
    int cap_rows;
    MD_ALIGN* aligns;       /* per-column alignment */
    int n_aligns;
    int cap_aligns;
    int capturing;          /* currently capturing a cell */
    int cur_is_header;      /* rows being created belong to the header */
    MD_ANSI_TCELL* cur;     /* cell currently being captured */
    int oom;                /* allocation failure flag */
} MD_ANSI_TABLE;

typedef struct MD_ANSI_tag MD_ANSI;
struct MD_ANSI_tag {
    void (*process_output)(const MD_CHAR*, MD_SIZE, void*);
    void* userdata;
    unsigned flags;
    int image_nesting_level;
    int quote_depth;
    int list_depth;
    int ol_counter;
    int in_code_block;
    int need_newline;       /* pending newline before next block */
    int need_indent;        /* emit indent prefix on next code text */
    int code_col;           /* display column within the current code line (clip) */
    int code_clip;          /* max code-content columns per line; 0 = no clip */
    int li_opened;          /* just opened a list item (bullet already printed) */

    MD_ANSI_TABLE* table;   /* non-NULL while inside a table block */
    int table_width;        /* >0 fixed, 0 = unlimited, <0 = auto-detect */
    const MD_ANSI_STYLE* style;  /* element styling (never NULL during render) */

    /* Prose word-wrap: content of the current logical line is collected into
     * lbuf (after its indent prefix), then wrapped to wrap_cols on newline. */
    int wrap_cols;          /* resolved wrap width in cols; 0 = no wrapping */
    int wrap_suspend;       /* when set, output bypasses the line buffer */
    char* lbuf;             /* current line content (after indent) */
    MD_SIZE lsize, lcap;
    int line_open;          /* content has been collected on the current line */
    char indent_buf[256];   /* exact bytes of the current line's indent prefix */
    MD_SIZE indent_len;
    int indent_w;           /* display width of indent_buf */

    /* Code block metadata tracking (only active when MD_ANSI_FLAG_CODE_META is set) */
    MD_SIZE output_offset;
    MD_ANSI_CODE_META* code_blocks;
    int n_code_blocks;
    int code_blocks_cap;

    /* Fenced-code syntax highlighting via libfyts: when active, the code text is
     * buffered between block enter/leave and handed to fyts on leave. */
    int code_highlight;     /* highlighting the current code block */
    char code_lang[64];     /* info string (language) of the current code block */
    MD_SIZE code_lang_size;
    char* code_buf;         /* buffered raw code text */
    MD_SIZE code_size, code_cap;

    char* sgr_buf;          /* scratch for filtering escapes out of input text */
    MD_SIZE sgr_cap;
};


/*********************************************
 ***  ANSI rendering helper functions  ***
 *********************************************/

/* Forward declarations (definitions live in the table-layout section). */
typedef struct { MD_SIZE start; MD_SIZE len; int w; } TLINE;
static void table_cell_append(MD_ANSI_TABLE* t, const MD_CHAR* text, MD_SIZE size);

/* Capture buffer for redirecting output (e.g. to measure the indent prefix). */
typedef struct {
    char* buf;
    MD_SIZE size;
    MD_SIZE cap;
} ANSI_CAPTURE_BUF;

static void
ansi_capture_append(const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    ANSI_CAPTURE_BUF* cap = (ANSI_CAPTURE_BUF*) userdata;
    MD_SIZE n = (cap->size + size <= cap->cap) ? size : (cap->cap - cap->size);
    if(n > 0) {
        memcpy(cap->buf + cap->size, text, n);
        cap->size += n;
    }
}

static int ansi_disp_width(const char* buf, MD_SIZE size);
static TLINE* wrap_text(const char* buf, MD_SIZE size, int width, int* n_out);
static void render_indent(MD_ANSI* r);

/* Running SGR (Select Graphic Rendition) state, so wrap continuation lines can
 * close an active style before the newline and re-apply it afterwards (else an
 * open underline/reverse/background bleeds across the break and the indent). */
typedef struct {
    int bold, faint, italic, underline, blink, reverse, conceal, strike, overline;
    char fg[24];   /* SGR params for the foreground, e.g. "31" or "38;5;12"; "" = default */
    char bg[24];   /* SGR params for the background; "" = default */
} SGR_STATE;

static void sgr_scan(SGR_STATE* s, const char* buf, MD_SIZE size);
static size_t sgr_build(const SGR_STATE* s, char* out, size_t cap);

/* Write bytes straight to the output callback (bypassing the line buffer). */
static void
out_direct(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    r->process_output(text, size, r->userdata);
    if(r->flags & MD_ANSI_FLAG_CODE_META)
        r->output_offset += size;
}

/* Append to the current logical line's content buffer (for prose wrapping). */
static void
lbuf_append(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    if(r->lsize + size > r->lcap) {
        MD_SIZE nc = r->lcap ? r->lcap * 2 : 128;
        char* p;
        while(nc < r->lsize + size) nc *= 2;
        p = (char*) realloc(r->lbuf, nc);
        if(p == NULL) return;
        r->lbuf = p;
        r->lcap = nc;
    }
    memcpy(r->lbuf + r->lsize, text, size);
    r->lsize += size;
}

static inline void
render_verbatim(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    /* While capturing a table cell, redirect output into the cell buffer. */
    if(r->table != NULL && r->table->capturing && r->table->cur != NULL) {
        table_cell_append(r->table, text, size);
        return;
    }
    /* When prose wrapping is active, collect the line; flush wraps it later. */
    if(r->wrap_cols > 0 && !r->wrap_suspend && !r->in_code_block) {
        lbuf_append(r, text, size);
        r->line_open = 1;
        return;
    }
    out_direct(r, text, size);
}

#define RENDER_VERBATIM(r, verbatim)                                    \
        render_verbatim((r), (verbatim), (MD_SIZE) (strlen(verbatim)))

static inline void
render_ansi(MD_ANSI* r, const char* code)
{
    if(!(r->flags & MD_ANSI_FLAG_NO_COLOR))
        RENDER_VERBATIM(r, code);
}

/* Emit the per-line indent chrome (document margin + quote/alert/list). */
static void
render_indent_chrome(MD_ANSI* r)
{
    int i;
    /* Global document left margin, applied to every line (like glow). */
    for(i = 0; i < DOC_MARGIN; i++)
        RENDER_VERBATIM(r, " ");
    for(i = 0; i < r->quote_depth; i++) {
        render_ansi(r, r->style->blockquote.on);
        RENDER_VERBATIM(r, r->style->blockquote_bar);
        RENDER_VERBATIM(r, " ");
        render_ansi(r, r->style->blockquote.off);
    }
    for(i = 0; i < r->list_depth; i++) {
        RENDER_VERBATIM(r, "  ");
    }
}

/* Start a new line: emit its indent prefix directly and remember it (so wrap
 * continuation lines can replay the exact same prefix). */
static void
render_indent(MD_ANSI* r)
{
    ANSI_CAPTURE_BUF cap;
    void (*saved_out)(const MD_CHAR*, MD_SIZE, void*) = r->process_output;
    void* saved_ud = r->userdata;
    MD_ANSI_TABLE* saved_table = r->table;
    int saved_suspend = r->wrap_suspend;

    cap.buf = r->indent_buf;
    cap.size = 0;
    cap.cap = sizeof(r->indent_buf);

    /* Capture the prefix bytes (without disturbing real output / wrapping). */
    r->process_output = ansi_capture_append;
    r->userdata = &cap;
    r->table = NULL;
    r->wrap_suspend = 1;
    render_indent_chrome(r);
    r->process_output = saved_out;
    r->userdata = saved_ud;
    r->table = saved_table;
    r->wrap_suspend = saved_suspend;

    r->indent_len = cap.size;
    r->indent_w = ansi_disp_width(r->indent_buf, cap.size);
    out_direct(r, r->indent_buf, r->indent_len);
}

/* Wrap the collected line content to the available width and emit it. */
static void
flush_wrapped(MD_ANSI* r)
{
    int avail = r->wrap_cols - r->indent_w - DOC_MARGIN;  /* reserve right margin */
    int n = 0, k;
    TLINE* lines;
    SGR_STATE st;
    MD_SIZE scanned = 0;

    if(avail < 1) avail = 1;
    lines = wrap_text(r->lbuf, r->lsize, avail, &n);
    memset(&st, 0, sizeof(st));

    for(k = 0; k < n; k++) {
        if(k > 0) {
            char active[128];
            size_t alen;
            /* Fold in everything up to this break so `st` reflects the style
             * active at the wrap point. */
            sgr_scan(&st, r->lbuf + scanned, lines[k].start - scanned);
            scanned = lines[k].start;
            alen = sgr_build(&st, active, sizeof(active));
            if(alen > 0)
                out_direct(r, "\x1b[0m", 4);        /* close before the newline */
            out_direct(r, "\n", 1);
            out_direct(r, r->indent_buf, r->indent_len);  /* replay prefix */
            if(alen > 0)
                out_direct(r, active, alen);        /* re-apply the open style */
        }
        if(lines[k].len > 0)
            out_direct(r, r->lbuf + lines[k].start, lines[k].len);
    }
    out_direct(r, "\n", 1);

    free(lines);
    r->lsize = 0;
    r->line_open = 0;
}

static void
render_newline(MD_ANSI* r)
{
    if(r->wrap_cols > 0 && !r->wrap_suspend && !r->in_code_block && r->line_open)
        flush_wrapped(r);
    else
        out_direct(r, "\n", 1);
}

/* Render a blank separator line with alert bar prefix when inside an alert. */
static void
render_separator(MD_ANSI* r)
{
    render_newline(r);
}

static unsigned
hex_val(char ch)
{
    if('0' <= ch && ch <= '9')
        return ch - '0';
    if('a' <= ch && ch <= 'f')
        return ch - 'a' + 10;
    if('A' <= ch && ch <= 'F')
        return ch - 'A' + 10;
    return 0;
}

static void
render_utf8_codepoint(MD_ANSI* r, unsigned codepoint,
                      void (*fn_append)(MD_ANSI*, const MD_CHAR*, MD_SIZE))
{
    static const MD_CHAR utf8_replacement_char[] = { (char)0xef, (char)0xbf, (char)0xbd };

    unsigned char utf8[4];
    size_t n;

    if(codepoint <= 0x7f) {
        n = 1;
        utf8[0] = codepoint;
    } else if(codepoint <= 0x7ff) {
        n = 2;
        utf8[0] = 0xc0 | ((codepoint >>  6) & 0x1f);
        utf8[1] = 0x80 + ((codepoint >>  0) & 0x3f);
    } else if(codepoint <= 0xffff) {
        n = 3;
        utf8[0] = 0xe0 | ((codepoint >> 12) & 0xf);
        utf8[1] = 0x80 + ((codepoint >>  6) & 0x3f);
        utf8[2] = 0x80 + ((codepoint >>  0) & 0x3f);
    } else {
        n = 4;
        utf8[0] = 0xf0 | ((codepoint >> 18) & 0x7);
        utf8[1] = 0x80 + ((codepoint >> 12) & 0x3f);
        utf8[2] = 0x80 + ((codepoint >>  6) & 0x3f);
        utf8[3] = 0x80 + ((codepoint >>  0) & 0x3f);
    }

    if(0 < codepoint  &&  codepoint <= 0x10ffff)
        fn_append(r, (char*)utf8, (MD_SIZE)n);
    else
        fn_append(r, utf8_replacement_char, 3);
}

static void
render_entity(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size,
              void (*fn_append)(MD_ANSI*, const MD_CHAR*, MD_SIZE))
{
    if(size > 3 && text[1] == '#') {
        unsigned codepoint = 0;

        if(text[2] == 'x' || text[2] == 'X') {
            MD_SIZE i;
            for(i = 3; i < size-1; i++)
                codepoint = 16 * codepoint + hex_val(text[i]);
        } else {
            MD_SIZE i;
            for(i = 2; i < size-1; i++)
                codepoint = 10 * codepoint + (text[i] - '0');
        }

        render_utf8_codepoint(r, codepoint, fn_append);
        return;
    } else {
        const ENTITY* ent;

        ent = entity_lookup(text, size);
        if(ent != NULL) {
            render_utf8_codepoint(r, ent->codepoints[0], fn_append);
            if(ent->codepoints[1])
                render_utf8_codepoint(r, ent->codepoints[1], fn_append);
            return;
        }
    }

    fn_append(r, text, size);
}

static void
render_attribute(MD_ANSI* r, const MD_ATTRIBUTE* attr,
                 void (*fn_append)(MD_ANSI*, const MD_CHAR*, MD_SIZE))
{
    int i;

    for(i = 0; attr->substr_offsets[i] < attr->size; i++) {
        MD_TEXTTYPE type = attr->substr_types[i];
        MD_OFFSET off = attr->substr_offsets[i];
        MD_SIZE size = attr->substr_offsets[i+1] - off;
        const MD_CHAR* text = attr->text + off;

        switch(type) {
            case MD_TEXT_NULLCHAR:  render_utf8_codepoint(r, 0x0000, render_verbatim); break;
            case MD_TEXT_ENTITY:    render_entity(r, text, size, fn_append); break;
            default:                fn_append(r, text, size); break;
        }
    }
}



/*****************************************
 ***  Code block metadata tracking     ***
 *****************************************/

static MD_ANSI_CODE_META*
ansi_code_meta_push(MD_ANSI* r)
{
    if(r->code_blocks == NULL) {
        r->code_blocks = (MD_ANSI_CODE_META*) malloc(8 * sizeof(MD_ANSI_CODE_META));
        if(r->code_blocks == NULL) return NULL;
        r->code_blocks_cap = 8;
    } else if(r->n_code_blocks >= r->code_blocks_cap) {
        int new_cap = r->code_blocks_cap * 2;
        MD_ANSI_CODE_META* p = (MD_ANSI_CODE_META*) realloc(r->code_blocks, new_cap * sizeof(MD_ANSI_CODE_META));
        if(p == NULL) return NULL;
        r->code_blocks = p;
        r->code_blocks_cap = new_cap;
    }
    memset(&r->code_blocks[r->n_code_blocks], 0, sizeof(MD_ANSI_CODE_META));
    return &r->code_blocks[r->n_code_blocks];
}

static void
ansi_code_meta_cleanup(MD_ANSI* r)
{
    if(r->code_blocks != NULL) {
        int i;
        int count = r->n_code_blocks + (r->in_code_block ? 1 : 0);
        for(i = 0; i < count; i++)
            free(r->code_blocks[i].highlights);
        free(r->code_blocks);
    }
}


static void
ansi_emit_json_str(void (*out)(const MD_CHAR*, MD_SIZE, void*), void* ud,
                   const char* str, MD_SIZE size)
{
    MD_SIZE i, beg = 0;
    out("\"", 1, ud);
    for(i = 0; i < size; i++) {
        unsigned char ch = (unsigned char) str[i];
        if(ch == '"' || ch == '\\' || ch < 0x20) {
            if(i > beg)
                out(str + beg, i - beg, ud);
            if(ch == '"' || ch == '\\') {
                out("\\", 1, ud);
                out(str + i, 1, ud);
            } else if(ch == '\n') {
                out("\\n", 2, ud);
            } else if(ch == '\r') {
                out("\\r", 2, ud);
            } else if(ch == '\t') {
                out("\\t", 2, ud);
            } else if(ch == 0x1b) {
                out("\\u001b", 6, ud);
            } else {
                static const char hex[] = "0123456789abcdef";
                char esc[6] = { '\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0xf] };
                out(esc, 6, ud);
            }
            beg = i + 1;
        }
    }
    if(i > beg)
        out(str + beg, i - beg, ud);
    out("\"", 1, ud);
}

static void
render_ansi_code_meta_json(MD_ANSI* r)
{
    void (*out)(const MD_CHAR*, MD_SIZE, void*) = r->process_output;
    void* ud = r->userdata;
    char buf[64];
    int i, n;

    out("\0", 1, ud);
    out("[", 1, ud);
    for(i = 0; i < r->n_code_blocks; i++) {
        MD_ANSI_CODE_META* m = &r->code_blocks[i];
        if(i > 0) out(",", 1, ud);

        n = snprintf(buf, sizeof(buf), "{\"s\":%u,\"e\":%u",
                     (unsigned)m->start, (unsigned)m->end);
        out(buf, (MD_SIZE)n, ud);

        if(m->lang_size > 0) {
            out(",\"l\":", 5, ud);
            ansi_emit_json_str(out, ud, m->lang, m->lang_size);
        }
        if(m->filename_size > 0) {
            out(",\"f\":", 5, ud);
            ansi_emit_json_str(out, ud, m->filename, m->filename_size);
        }
        if(m->highlight_count > 0) {
            unsigned j;
            out(",\"h\":[", 6, ud);
            for(j = 0; j < m->highlight_count; j++) {
                if(j > 0) out(",", 1, ud);
                n = snprintf(buf, sizeof(buf), "%u", m->highlights[j]);
                out(buf, (MD_SIZE)n, ud);
            }
            out("]", 1, ud);
        }
        if(m->prefix_size > 0) {
            out(",\"i\":", 5, ud);
            ansi_emit_json_str(out, ud, m->prefix, m->prefix_size);
        }
        out("}", 1, ud);
    }
    out("]", 1, ud);
}


/*****************************************
 ***  Table layout (glow / lipgloss)   ***
 *****************************************/

/* Decode one UTF-8 sequence; returns byte length, stores codepoint in *cp. */
static MD_SIZE
ansi_utf8_decode(const char* s, MD_SIZE n, unsigned* cp)
{
    unsigned char c = (unsigned char) s[0];
    if(c < 0x80) { *cp = c; return 1; }
    if((c & 0xe0) == 0xc0 && n >= 2) {
        *cp = ((c & 0x1f) << 6) | ((unsigned char) s[1] & 0x3f);
        return 2;
    }
    if((c & 0xf0) == 0xe0 && n >= 3) {
        *cp = ((c & 0x0f) << 12) | (((unsigned char) s[1] & 0x3f) << 6)
            | ((unsigned char) s[2] & 0x3f);
        return 3;
    }
    if((c & 0xf8) == 0xf0 && n >= 4) {
        *cp = ((c & 0x07) << 18) | (((unsigned char) s[1] & 0x3f) << 12)
            | (((unsigned char) s[2] & 0x3f) << 6) | ((unsigned char) s[3] & 0x3f);
        return 4;
    }
    *cp = c;
    return 1;
}

/* Sorted [lo,hi] ranges of zero-width codepoints: Unicode general categories
 * Cf (format), Mn (nonspacing mark) and Me (enclosing mark), plus the C0/C1
 * control hint. Mirrors the classic Markus Kuhn wcwidth() combining table,
 * extended for later Unicode. Kept here (rather than a generated table) so the
 * ANSI renderer has no dependency on md4c.c's Unicode internals. */
typedef struct { unsigned lo, hi; } CP_RANGE;

static const CP_RANGE zero_width_ranges[] = {
    {0x0300,0x036f},{0x0483,0x0489},{0x0591,0x05bd},{0x05bf,0x05bf},
    {0x05c1,0x05c2},{0x05c4,0x05c5},{0x05c7,0x05c7},{0x0610,0x061a},
    {0x064b,0x065f},{0x0670,0x0670},{0x06d6,0x06dc},{0x06df,0x06e4},
    {0x06e7,0x06e8},{0x06ea,0x06ed},{0x0711,0x0711},{0x0730,0x074a},
    {0x07a6,0x07b0},{0x07eb,0x07f3},{0x0816,0x0819},{0x081b,0x0823},
    {0x0825,0x0827},{0x0829,0x082d},{0x0859,0x085b},{0x08e3,0x0902},
    {0x093a,0x093a},{0x093c,0x093c},{0x0941,0x0948},{0x094d,0x094d},
    {0x0951,0x0957},{0x0962,0x0963},{0x0981,0x0981},{0x09bc,0x09bc},
    {0x09c1,0x09c4},{0x09cd,0x09cd},{0x0a01,0x0a02},{0x0a3c,0x0a3c},
    {0x0a41,0x0a51},{0x0a70,0x0a71},{0x0a75,0x0a75},{0x0abc,0x0abc},
    {0x0ac1,0x0acd},{0x0b01,0x0b01},{0x0b3c,0x0b3c},{0x0b3f,0x0b3f},
    {0x0b41,0x0b56},{0x0b82,0x0b82},{0x0bc0,0x0bc0},{0x0bcd,0x0bcd},
    {0x0c00,0x0c00},{0x0c3e,0x0c40},{0x0c46,0x0c56},{0x0cbc,0x0cbc},
    {0x0ccc,0x0ccd},{0x0e31,0x0e31},{0x0e34,0x0e3a},{0x0e47,0x0e4e},
    {0x0eb1,0x0eb1},{0x0eb4,0x0ebc},{0x0ec8,0x0ecd},{0x0f18,0x0f19},
    {0x0f35,0x0f35},{0x0f37,0x0f37},{0x0f39,0x0f39},{0x0f71,0x0f7e},
    {0x0f80,0x0f84},{0x0f86,0x0f87},{0x0f8d,0x0fbc},{0x0fc6,0x0fc6},
    {0x102d,0x1030},{0x1032,0x1037},{0x1039,0x103a},{0x103d,0x103e},
    {0x1058,0x1059},{0x105e,0x1060},{0x1071,0x1074},{0x1082,0x1082},
    {0x1085,0x1086},{0x108d,0x108d},{0x135d,0x135f},{0x1712,0x1714},
    {0x1732,0x1734},{0x1752,0x1753},{0x1772,0x1773},{0x17b4,0x17b5},
    {0x17b7,0x17bd},{0x17c6,0x17c6},{0x17c9,0x17d3},{0x17dd,0x17dd},
    {0x180b,0x180e},{0x1885,0x1886},{0x18a9,0x18a9},{0x1920,0x1922},
    {0x1927,0x1928},{0x1932,0x1932},{0x1939,0x193b},{0x1a17,0x1a18},
    {0x1a1b,0x1a1b},{0x1a56,0x1a56},{0x1a58,0x1a60},{0x1a62,0x1a62},
    {0x1a65,0x1a6c},{0x1a73,0x1a7c},{0x1a7f,0x1a7f},{0x1ab0,0x1aff},
    {0x1b00,0x1b03},{0x1b34,0x1b34},{0x1b36,0x1b3a},{0x1b3c,0x1b3c},
    {0x1b42,0x1b42},{0x1b6b,0x1b73},{0x1b80,0x1b81},{0x1ba2,0x1ba5},
    {0x1ba8,0x1ba9},{0x1bab,0x1bad},{0x1be6,0x1be6},{0x1be8,0x1be9},
    {0x1bed,0x1bed},{0x1bef,0x1bf1},{0x1c2c,0x1c33},{0x1c36,0x1c37},
    {0x1cd0,0x1cd2},{0x1cd4,0x1ce0},{0x1ce2,0x1ce8},{0x1ced,0x1ced},
    {0x1cf4,0x1cf4},{0x1cf8,0x1cf9},{0x1dc0,0x1dff},{0x200b,0x200f},
    {0x202a,0x202e},{0x2060,0x2064},{0x2066,0x206f},{0x20d0,0x20f0},
    {0x2cef,0x2cf1},{0x2d7f,0x2d7f},{0x2de0,0x2dff},{0x302a,0x302d},
    {0x3099,0x309a},{0xa66f,0xa672},{0xa674,0xa67d},{0xa69e,0xa69f},
    {0xa6f0,0xa6f1},{0xa802,0xa802},{0xa806,0xa806},{0xa80b,0xa80b},
    {0xa825,0xa826},{0xa8c4,0xa8c5},{0xa8e0,0xa8f1},{0xa926,0xa92d},
    {0xa947,0xa951},{0xa980,0xa982},{0xa9b3,0xa9b3},{0xa9b6,0xa9b9},
    {0xa9bc,0xa9bc},{0xa9e5,0xa9e5},{0xaa29,0xaa2e},{0xaa31,0xaa32},
    {0xaa35,0xaa36},{0xaa43,0xaa43},{0xaa4c,0xaa4c},{0xaa7c,0xaa7c},
    {0xaab0,0xaab0},{0xaab2,0xaab4},{0xaab7,0xaab8},{0xaabe,0xaabf},
    {0xaac1,0xaac1},{0xaaec,0xaaed},{0xaaf6,0xaaf6},{0xabe5,0xabe5},
    {0xabe8,0xabe8},{0xabed,0xabed},{0xfb1e,0xfb1e},{0xfe00,0xfe0f},
    {0xfe20,0xfe2f},{0xfeff,0xfeff},{0xfff9,0xfffb},{0x101fd,0x101fd},
    {0x102e0,0x102e0},{0x10376,0x1037a},{0x10a01,0x10a0f},{0x10a38,0x10a3f},
    {0x11000,0x11002},{0x11038,0x11046},{0x1107f,0x11082},{0x110b3,0x110ba},
    {0x11100,0x11102},{0x11127,0x1112b},{0x1112d,0x11134},{0x11180,0x11181},
    {0x111b6,0x111be},{0x1122f,0x11231},{0x11234,0x11237},{0x112df,0x112ea},
    {0x11300,0x11301},{0x1133c,0x1133c},{0x11340,0x11340},{0x11366,0x11374},
    {0x114b3,0x114be},{0x115b2,0x115c0},{0x11633,0x1163a},{0x1163d,0x1163d},
    {0x1163f,0x11640},{0x116ab,0x116b7},{0x1171d,0x1172b},{0x16af0,0x16af4},
    {0x16b30,0x16b36},{0x16f8f,0x16f92},{0x1bc9d,0x1bc9e},{0x1d165,0x1d169},
    {0x1d16d,0x1d182},{0x1d185,0x1d18b},{0x1d1aa,0x1d1ad},{0x1d242,0x1d244},
    {0x1da00,0x1da36},{0x1da3b,0x1da6c},{0x1da75,0x1da75},{0x1da84,0x1da84},
    {0x1da9b,0x1daaf},{0x1e000,0x1e02a},{0x1e8d0,0x1e8d6},{0x1e944,0x1e94a},
    {0xe0001,0xe01ef}
};

/* East Asian Wide/Fullwidth + emoji-presentation ranges (two columns each).
 * Generated by scripts/gen-wcwidth-wide.py; regenerate after a Unicode bump. */
static const CP_RANGE wide_ranges[] = {
#include "md4c-wcwidth-wide.h"
};

static int
cp_range_contains(const CP_RANGE* ranges, int n, unsigned cp)
{
    int lo = 0, hi = n - 1;
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        if(cp < ranges[mid].lo)      hi = mid - 1;
        else if(cp > ranges[mid].hi) lo = mid + 1;
        else                         return 1;
    }
    return 0;
}

/* Display width (columns) of a Unicode codepoint: 0 for combining/format
 * marks, 2 for East Asian Wide/Fullwidth and most emoji, 1 otherwise.
 *
 * This is a per-codepoint approximation; it does not perform grapheme-cluster
 * segmentation, so ZWJ emoji sequences (e.g. family/flag emoji) may measure
 * wider than they render in terminals that collapse them. */
static int
cp_width(unsigned cp)
{
    if(cp == 0)
        return 0;
    if(cp < 0x20 || (cp >= 0x7f && cp < 0xa0))   /* C0 / C1 controls */
        return 0;
    if(cp < 0x0300)                               /* fast path: Latin etc. */
        return 1;
    if(cp_range_contains(zero_width_ranges,
                         (int)(sizeof(zero_width_ranges)/sizeof(zero_width_ranges[0])), cp))
        return 0;
    if(cp_range_contains(wide_ranges,
                         (int)(sizeof(wide_ranges)/sizeof(wide_ranges[0])), cp))
        return 2;
    return 1;
}

/* Length in bytes of an ANSI escape sequence starting at s, or 0 if none. */
static MD_SIZE
ansi_esc_len(const char* s, MD_SIZE n)
{
    MD_SIZE i;
    if(n == 0 || (unsigned char) s[0] != 0x1b)
        return 0;
    if(n >= 2 && s[1] == '[') {            /* CSI: ESC [ ... final(0x40-0x7e) */
        i = 2;
        while(i < n && !((unsigned char) s[i] >= 0x40 && (unsigned char) s[i] <= 0x7e))
            i++;
        if(i < n) i++;
        return i;
    }
    if(n >= 2 && s[1] == ']') {            /* OSC: ESC ] ... (BEL | ESC \) */
        i = 2;
        while(i < n) {
            if((unsigned char) s[i] == 0x07) { i++; break; }
            if((unsigned char) s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\') { i += 2; break; }
            i++;
        }
        return i;
    }
    if(n >= 2 && s[1] == '\\')             /* ST */
        return 2;
    return 1;
}

/* Apply one SGR sequence (the params between "ESC[" and the final 'm', already
 * tokenised into vals[0..count)) to the running state. Returns nothing; the
 * extended colour forms 38/48;5;n and 38/48;2;r;g;b consume trailing params. */
static void
sgr_apply(SGR_STATE* s, const int* vals, int count)
{
    int j;
    for(j = 0; j < count; j++) {
        int v = vals[j];
        if(v == 0) { memset(s, 0, sizeof(*s)); continue; }
        switch(v) {
            case 1:  s->bold = 1; break;
            case 2:  s->faint = 1; break;
            case 22: s->bold = s->faint = 0; break;
            case 3:  s->italic = 1; break;
            case 23: s->italic = 0; break;
            case 4:  s->underline = 1; break;
            case 24: s->underline = 0; break;
            case 5: case 6: s->blink = 1; break;
            case 25: s->blink = 0; break;
            case 7:  s->reverse = 1; break;
            case 27: s->reverse = 0; break;
            case 8:  s->conceal = 1; break;
            case 28: s->conceal = 0; break;
            case 9:  s->strike = 1; break;
            case 29: s->strike = 0; break;
            case 53: s->overline = 1; break;
            case 55: s->overline = 0; break;
            case 39: s->fg[0] = '\0'; break;
            case 49: s->bg[0] = '\0'; break;
            default:
                if((v >= 30 && v <= 37) || (v >= 90 && v <= 97))
                    snprintf(s->fg, sizeof(s->fg), "%d", v);
                else if((v >= 40 && v <= 47) || (v >= 100 && v <= 107))
                    snprintf(s->bg, sizeof(s->bg), "%d", v);
                else if(v == 38 || v == 48) {
                    char* dst = (v == 38) ? s->fg : s->bg;
                    if(j + 1 < count && vals[j + 1] == 5 && j + 2 < count) {
                        snprintf(dst, sizeof(s->fg), "%d;5;%d", v, vals[j + 2]);
                        j += 2;
                    } else if(j + 1 < count && vals[j + 1] == 2 && j + 4 < count) {
                        snprintf(dst, sizeof(s->fg), "%d;2;%d;%d;%d", v,
                                 vals[j + 2], vals[j + 3], vals[j + 4]);
                        j += 4;
                    }
                }
                break;
        }
    }
}

/* Walk a byte range, folding every SGR escape it contains into the state. */
static void
sgr_scan(SGR_STATE* s, const char* buf, MD_SIZE size)
{
    MD_SIZE i = 0;
    while(i < size) {
        MD_SIZE e = ansi_esc_len(buf + i, size - i);
        if(e == 0) { i++; continue; }
        /* Only SGR (CSI ... 'm') sequences affect style state. */
        if(e >= 3 && (unsigned char) buf[i] == 0x1b && buf[i + 1] == '['
           && buf[i + e - 1] == 'm') {
            int vals[32], count = 0, cur = 0, have = 0;
            MD_SIZE p;
            for(p = i + 2; p < i + e - 1; p++) {
                char c = buf[p];
                if(c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); have = 1; }
                else if(c == ';') {
                    if(count < (int)(sizeof(vals)/sizeof(vals[0]))) vals[count++] = cur;
                    cur = 0; have = 1;
                }
            }
            if(have && count < (int)(sizeof(vals)/sizeof(vals[0]))) vals[count++] = cur;
            if(count == 0) { memset(s, 0, sizeof(*s)); }  /* "ESC[m" == reset */
            else sgr_apply(s, vals, count);
        }
        i += e;
    }
}

/* Serialise the active state into a single "ESC[...m" sequence in out (NUL-
 * terminated). Returns its byte length, or 0 when no style is active. */
static size_t
sgr_build(const SGR_STATE* s, char* out, size_t cap)
{
    char params[96];
    size_t n = 0;
    #define SGR_PUT(str)                                              \
        do {                                                          \
            size_t l = strlen(str);                                   \
            if(n && n + 1 < sizeof(params)) params[n++] = ';';        \
            if(n + l < sizeof(params)) { memcpy(params + n, (str), l); n += l; } \
        } while(0)
    if(s->bold)      SGR_PUT("1");
    if(s->faint)     SGR_PUT("2");
    if(s->italic)    SGR_PUT("3");
    if(s->underline) SGR_PUT("4");
    if(s->blink)     SGR_PUT("5");
    if(s->reverse)   SGR_PUT("7");
    if(s->conceal)   SGR_PUT("8");
    if(s->strike)    SGR_PUT("9");
    if(s->overline)  SGR_PUT("53");
    if(s->fg[0])     SGR_PUT(s->fg);
    if(s->bg[0])     SGR_PUT(s->bg);
    #undef SGR_PUT
    params[n] = '\0';
    if(n == 0)
        return 0;
    return (size_t) snprintf(out, cap, "\x1b[%sm", params);
}

/* Filter ANSI escape sequences out of a run of input text into `out` (which
 * must have room for at least `size` bytes), per the renderer's SGR policy:
 *   strip (neither flag) : drop every escape sequence.
 *   SAFE                 : keep only SGR (CSI ... 'm'); drop the rest.
 *   KEEP                 : handled by the caller (no filtering).
 * Returns the number of bytes written to `out`. */
static MD_SIZE
sgr_filter_input(unsigned flags, const char* in, MD_SIZE size, char* out)
{
    MD_SIZE i = 0, o = 0;
    int safe = (flags & MD_ANSI_FLAG_SGR_SAFE) != 0;
    while(i < size) {
        MD_SIZE e = ansi_esc_len(in + i, size - i);
        if(e == 0) { out[o++] = in[i++]; continue; }
        /* SAFE keeps SGR (CSI sequences ending in 'm'); everything else drops. */
        if(safe && e >= 3 && (unsigned char) in[i] == 0x1b && in[i + 1] == '['
           && in[i + e - 1] == 'm') {
            memcpy(out + o, in + i, e);
            o += e;
        }
        i += e;
    }
    return o;
}

/* Display width of a buffer, ignoring ANSI escape sequences. */
static int
ansi_disp_width(const char* buf, MD_SIZE size)
{
    MD_SIZE i = 0;
    int w = 0;
    while(i < size) {
        MD_SIZE e = ansi_esc_len(buf + i, size - i);
        unsigned cp;
        MD_SIZE cl;
        if(e > 0) { i += e; continue; }
        cl = ansi_utf8_decode(buf + i, size - i, &cp);
        w += cp_width(cp);
        i += cl;
    }
    return w;
}

/* Detect the terminal width: $COLUMNS, then TIOCGWINSZ, else 80. */
static int
table_term_width(void)
{
    const char* env = getenv("COLUMNS");
    if(env != NULL && *env != '\0') {
        int w = atoi(env);
        if(w > 0) return w;
    }
#ifdef MD4C_ANSI_HAVE_IOCTL
    {
        struct winsize ws;
        if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            return (int) ws.ws_col;
    }
#endif
    return 80;
}

int
md_ansi_detect_width(void)
{
    return table_term_width();
}

static void
table_cell_append(MD_ANSI_TABLE* t, const MD_CHAR* text, MD_SIZE size)
{
    MD_ANSI_TCELL* c = t->cur;
    if(c == NULL || size == 0)
        return;
    if(c->size + size > c->cap) {
        MD_SIZE nc = c->cap ? c->cap * 2 : 64;
        char* p;
        while(nc < c->size + size) nc *= 2;
        p = (char*) realloc(c->buf, nc);
        if(p == NULL) { t->oom = 1; return; }
        c->buf = p;
        c->cap = nc;
    }
    memcpy(c->buf + c->size, text, size);
    c->size += size;
}

static MD_ANSI_TROW*
table_push_row(MD_ANSI_TABLE* t, int is_header)
{
    MD_ANSI_TROW* row;
    if(t->n_rows >= t->cap_rows) {
        int nc = t->cap_rows ? t->cap_rows * 2 : 8;
        MD_ANSI_TROW* p = (MD_ANSI_TROW*) realloc(t->rows, nc * sizeof(MD_ANSI_TROW));
        if(p == NULL) { t->oom = 1; return NULL; }
        t->rows = p;
        t->cap_rows = nc;
    }
    row = &t->rows[t->n_rows++];
    memset(row, 0, sizeof(*row));
    row->is_header = is_header;
    return row;
}

static MD_ANSI_TCELL*
table_push_cell(MD_ANSI_TABLE* t)
{
    MD_ANSI_TROW* row;
    MD_ANSI_TCELL* cell;
    if(t->n_rows == 0)
        return NULL;
    row = &t->rows[t->n_rows - 1];
    if(row->n_cells >= row->cap_cells) {
        int nc = row->cap_cells ? row->cap_cells * 2 : 4;
        MD_ANSI_TCELL* p = (MD_ANSI_TCELL*) realloc(row->cells, nc * sizeof(MD_ANSI_TCELL));
        if(p == NULL) { t->oom = 1; return NULL; }
        row->cells = p;
        row->cap_cells = nc;
    }
    cell = &row->cells[row->n_cells++];
    memset(cell, 0, sizeof(*cell));
    return cell;
}

static void
table_set_align(MD_ANSI_TABLE* t, int col, MD_ALIGN align)
{
    if(col < 0)
        return;
    if(col >= t->cap_aligns) {
        int nc = t->cap_aligns ? t->cap_aligns : 8;
        MD_ALIGN* p;
        int k;
        while(nc <= col) nc *= 2;
        p = (MD_ALIGN*) realloc(t->aligns, nc * sizeof(MD_ALIGN));
        if(p == NULL) { t->oom = 1; return; }
        for(k = t->cap_aligns; k < nc; k++) p[k] = MD_ALIGN_DEFAULT;
        t->aligns = p;
        t->cap_aligns = nc;
    }
    if(col >= t->n_aligns)
        t->n_aligns = col + 1;
    /* Header defines alignment; keep first non-default value seen. */
    if(t->aligns[col] == MD_ALIGN_DEFAULT)
        t->aligns[col] = align;
}

static void
table_free(MD_ANSI_TABLE* t)
{
    int i, j;
    if(t == NULL)
        return;
    for(i = 0; i < t->n_rows; i++) {
        for(j = 0; j < t->rows[i].n_cells; j++)
            free(t->rows[i].cells[j].buf);
        free(t->rows[i].cells);
    }
    free(t->rows);
    free(t->aligns);
    free(t);
}

/* Display width of the current line-indent prefix (quote/list/alert chrome). */
static int
ansi_indent_width(MD_ANSI* r)
{
    char buf[256];
    ANSI_CAPTURE_BUF cap;
    void (*saved_out)(const MD_CHAR*, MD_SIZE, void*) = r->process_output;
    void* saved_ud = r->userdata;
    MD_ANSI_TABLE* saved_table = r->table;
    int saved_suspend = r->wrap_suspend;
    int w;

    cap.buf = buf;
    cap.size = 0;
    cap.cap = sizeof(buf);

    r->table = NULL;                 /* prevent cell-capture redirect */
    r->wrap_suspend = 1;             /* prevent line-buffer redirect */
    r->process_output = ansi_capture_append;
    r->userdata = &cap;
    render_indent_chrome(r);
    r->process_output = saved_out;
    r->userdata = saved_ud;
    r->table = saved_table;
    r->wrap_suspend = saved_suspend;

    w = ansi_disp_width(buf, cap.size);
    return w;
}

static void
tbl_spaces(MD_ANSI* r, int n)
{
    while(n-- > 0)
        RENDER_VERBATIM(r, " ");
}

/* Greedy word-wrap of a UTF-8 buffer to `width` display columns, like glow.
 * Returns a malloc'd array of line slices into `buf` (count in *n_out); the
 * caller frees it. ANSI escapes are zero-width and stay attached to the line
 * they appear in. Always returns at least one (possibly empty) line. */
static TLINE*
wrap_text(const char* buf, MD_SIZE size, int width, int* n_out)
{
    TLINE* lines = NULL;
    int n = 0, cap = 0;
    MD_SIZE i = 0, line_start = 0, last_space = (MD_SIZE) -1;
    int line_w = 0, w_at_space = 0;

    if(width < 1)
        width = 1;

#define TLINE_PUSH(s, e, wd)                                                 \
    do {                                                                     \
        if(n >= cap) {                                                       \
            int nc = cap ? cap * 2 : 4;                                      \
            TLINE* p = (TLINE*) realloc(lines, (size_t) nc * sizeof(TLINE)); \
            if(p == NULL) { *n_out = n; return lines; }                      \
            lines = p; cap = nc;                                             \
        }                                                                    \
        lines[n].start = (s); lines[n].len = (MD_SIZE) ((e) - (s));          \
        lines[n].w = (wd); n++;                                              \
    } while(0)

    while(i < size) {
        MD_SIZE e = ansi_esc_len(buf + i, size - i);
        unsigned cp;
        MD_SIZE cl;
        int cw;

        if(e > 0) { i += e; continue; }     /* escape stays on the current line */

        cl = ansi_utf8_decode(buf + i, size - i, &cp);
        cw = cp_width(cp);

        if(cp == ' ') {
            last_space = i;
            w_at_space = line_w;
        }

        if(line_w + cw > width && i > line_start) {
            if(last_space != (MD_SIZE) -1 && last_space > line_start) {
                TLINE_PUSH(line_start, last_space, w_at_space);
                i = last_space;
                while(i < size && buf[i] == ' ') i++;   /* skip the break spaces */
                line_start = i;
                line_w = 0;
                last_space = (MD_SIZE) -1;
                continue;
            } else {
                TLINE_PUSH(line_start, i, line_w);       /* hard break long word */
                line_start = i;
                line_w = 0;
                last_space = (MD_SIZE) -1;
            }
        }
        line_w += cw;
        i += cl;
    }

    /* Final line, with trailing spaces trimmed. */
    {
        MD_SIZE e = size;
        int tw = line_w;
        while(e > line_start && buf[e - 1] == ' ') { e--; tw--; }
        if(e > line_start || n == 0)
            TLINE_PUSH(line_start, e, tw < 0 ? 0 : tw);
    }
#undef TLINE_PUSH

    *n_out = n;
    return lines;
}

/* Emit one wrapped line of a cell, aligned and padded to `width`. */
static void
table_emit_slice(MD_ANSI* r, const char* buf, TLINE ln, int width,
                 MD_ALIGN align, int is_header)
{
    int pad = width - ln.w;
    int lpad = 0, rpad;
    if(pad < 0) pad = 0;
    rpad = pad;
    if(align == MD_ALIGN_RIGHT)       { lpad = pad; rpad = 0; }
    else if(align == MD_ALIGN_CENTER) { lpad = pad / 2; rpad = pad - lpad; }

    tbl_spaces(r, lpad);
    if(is_header) render_ansi(r, r->style->table_header.on);
    if(ln.len > 0) render_verbatim(r, buf + ln.start, ln.len);
    if(is_header) render_ansi(r, r->style->table_header.off);
    tbl_spaces(r, rpad);
}

static void
table_emit_row(MD_ANSI* r, MD_ANSI_TROW* row, const int* widths, int n_cols)
{
    TLINE** wrapped = (TLINE**) calloc((size_t) n_cols, sizeof(TLINE*));
    int* nlines = (int*) calloc((size_t) n_cols, sizeof(int));
    const char** bufs = (const char**) calloc((size_t) n_cols, sizeof(char*));
    int height = 1, j, k;

    if(wrapped == NULL || nlines == NULL || bufs == NULL) {
        free(wrapped); free(nlines); free((void*) bufs);
        return;
    }

    for(j = 0; j < n_cols; j++) {
        const MD_ANSI_TCELL* cell = (j < row->n_cells) ? &row->cells[j] : NULL;
        bufs[j] = (cell != NULL) ? cell->buf : NULL;
        wrapped[j] = wrap_text(bufs[j], (cell != NULL) ? cell->size : 0,
                               widths[j], &nlines[j]);
        if(nlines[j] > height) height = nlines[j];
    }

    for(k = 0; k < height; k++) {
        render_indent(r);
        RENDER_VERBATIM(r, " ");                 /* outer left cell padding */
        for(j = 0; j < n_cols; j++) {
            MD_ALIGN align = (j < r->table->n_aligns) ? r->table->aligns[j] : MD_ALIGN_DEFAULT;
            if(j > 0) {
                RENDER_VERBATIM(r, " ");
                RENDER_VERBATIM(r, r->style->table_vertical);
                RENDER_VERBATIM(r, " ");
            }
            if(k < nlines[j])
                table_emit_slice(r, bufs[j], wrapped[j][k], widths[j], align, row->is_header);
            else
                tbl_spaces(r, widths[j]);
        }
        RENDER_VERBATIM(r, " ");                 /* outer right cell padding */
        render_newline(r);
    }

    for(j = 0; j < n_cols; j++)
        free(wrapped[j]);
    free(wrapped); free(nlines); free((void*) bufs);
}

static void
table_emit_separator(MD_ANSI* r, const int* widths, int n_cols)
{
    int j, k;
    const char* h = r->style->table_horizontal;
    render_indent(r);
    RENDER_VERBATIM(r, h);                    /* under outer left padding */
    for(j = 0; j < n_cols; j++) {
        if(j > 0) {
            RENDER_VERBATIM(r, h);
            RENDER_VERBATIM(r, r->style->table_cross);
            RENDER_VERBATIM(r, h);
        }
        for(k = 0; k < widths[j]; k++)
            RENDER_VERBATIM(r, h);
    }
    RENDER_VERBATIM(r, h);                    /* under outer right padding */
    render_newline(r);
}

/* Lay out and emit the buffered table. */
static void
table_emit(MD_ANSI* r)
{
    MD_ANSI_TABLE* t = r->table;
    int n_cols = 0, i, j;
    int* widths;
    int indent_w, total = 0;
    int any_header = 0, emitted_sep = 0;

    if(t == NULL)
        return;

    for(i = 0; i < t->n_rows; i++) {
        if(t->rows[i].n_cells > n_cols) n_cols = t->rows[i].n_cells;
        if(t->rows[i].is_header) any_header = 1;
    }
    if(n_cols == 0)
        return;

    widths = (int*) calloc((size_t) n_cols, sizeof(int));
    if(widths == NULL)
        return;

    /* Natural column widths = max cell display width per column. */
    for(i = 0; i < t->n_rows; i++) {
        MD_ANSI_TROW* row = &t->rows[i];
        for(j = 0; j < row->n_cells; j++) {
            int w = ansi_disp_width(row->cells[j].buf, row->cells[j].size);
            if(w > widths[j]) widths[j] = w;
        }
    }

    /* Fit columns to a target width (like glow): shrink the widest columns
     * when too wide, expand the narrowest to fill when too narrow.
     *
     * Non-content overhead per line = document margin + the two outer cell
     * paddings + the " │ " gaps (3 cols each). Width mode: >0 fixed,
     * INF(0) = unlimited (natural widths), <0 = auto-detect. */
    indent_w = ansi_indent_width(r);

    /* The document margin and any blockquote/list chrome are already part of
     * indent_w (emitted by render_indent), so the only extra per-line overhead
     * here is the two outer cell paddings plus the " │ " gaps. */
    if(r->table_width != MD_ANSI_WIDTH_INF) {
        int wtarget = (r->table_width > 0) ? r->table_width : table_term_width();
        int overhead = 2 + 3 * (n_cols - 1);
        int content_avail = wtarget - indent_w - overhead - DOC_MARGIN; /* right margin */
        if(content_avail < n_cols) content_avail = n_cols;  /* >= 1 col each */

        total = 0;
        for(j = 0; j < n_cols; j++) {
            if(widths[j] < 1) widths[j] = 1;
            total += widths[j];
        }

        /* Shrink the widest column until it fits. */
        while(total > content_avail) {
            int wi = 0;
            for(j = 1; j < n_cols; j++)
                if(widths[j] > widths[wi]) wi = j;
            if(widths[wi] <= 1) break;
            widths[wi]--;
            total--;
        }
        /* Expand the narrowest column to fill the remaining width -- unless
         * fit-to-content is requested, in which case columns keep their natural
         * widths (already capped by the shrink loop above) and the table grows
         * only up to what its content needs. */
        if(!(r->flags & MD_ANSI_FLAG_TABLE_FIT_CONTENT)) {
            while(total < content_avail) {
                int wi = 0;
                for(j = 1; j < n_cols; j++)
                    if(widths[j] < widths[wi]) wi = j;
                widths[wi]++;
                total++;
            }
        }
    }

    /* The table emits its own pre-wrapped lines; bypass prose line wrapping. */
    {
        int saved_suspend = r->wrap_suspend;
        r->wrap_suspend = 1;

        for(i = 0; i < t->n_rows; i++) {
            MD_ANSI_TROW* row = &t->rows[i];
            if(!row->is_header && any_header && !emitted_sep) {
                table_emit_separator(r, widths, n_cols);
                emitted_sep = 1;
            }
            table_emit_row(r, row, widths, n_cols);
        }
        if(any_header && !emitted_sep)
            table_emit_separator(r, widths, n_cols);

        r->wrap_suspend = saved_suspend;
    }

    free(widths);
}


/**************************************
 ***  ANSI renderer implementation  ***
 **************************************/

/* Append raw code text to the per-block buffer (for syntax highlighting). */
static void
code_buf_append(MD_ANSI* r, const char* text, MD_SIZE size)
{
    if(r->code_size + size > r->code_cap) {
        MD_SIZE nc = r->code_cap ? r->code_cap * 2 : 1024;
        char* p;
        while(nc < r->code_size + size) nc *= 2;
        p = (char*) realloc(r->code_buf, nc);
        if(p == NULL) return;   /* drop highlighting on OOM; never fatal */
        r->code_buf = p;
        r->code_cap = nc;
    }
    memcpy(r->code_buf + r->code_size, text, size);
    r->code_size += size;
}

/* Build the dash text of a code-block rule (no indent, no colour, no newline)
 * into buf: for the header a language label is inset as "-- lang ----...";
 * otherwise it is all dashes spanning `cols` display columns. Used both for the
 * dim rules md4c draws and for the strings handed to fyts (which frames them in
 * reverse mode). Returns the byte length written. */
static size_t
build_code_rule_text(const char* gl_horiz, const char* lang, MD_SIZE lang_size,
                     int cols, char* buf, size_t bufsz)
{
    size_t hlen = strlen(gl_horiz);
    size_t n = 0;
    int i, langw = 0;

    if(lang != NULL && lang_size > 0) {
        langw = (int) lang_size;   /* labels are ASCII-ish; width ~= bytes */
        if(langw > cols - 4) langw = 0;
    }
    #define PUT(s, l) do { if(n + (l) <= bufsz) { memcpy(buf + n, (s), (l)); n += (l); } } while(0)
    if(langw > 0) {
        PUT(gl_horiz, hlen); PUT(gl_horiz, hlen); PUT(" ", 1);
        PUT(lang, (size_t) lang_size); PUT(" ", 1);
        for(i = 0; i < cols - 4 - langw; i++) PUT(gl_horiz, hlen);
    } else {
        for(i = 0; i < cols; i++) PUT(gl_horiz, hlen);
    }
    #undef PUT
    return n;
}

/* Emit a dim rule line delimiting a fenced code block, at the document margin. */
static void
render_code_rule(MD_ANSI* r, const char* lang, MD_SIZE lang_size)
{
    int width, avail;
    char buf[1024];
    size_t n;

    render_indent(r);
    width = (r->wrap_cols > 0) ? r->wrap_cols : table_term_width();
    avail = width - r->indent_w - DOC_MARGIN;   /* match prose right margin */
    if(avail < 4) avail = 4;

    n = build_code_rule_text(r->style->table_horizontal, lang, lang_size,
                             avail, buf, sizeof(buf));
    render_ansi(r, r->style->rule.on);
    render_verbatim(r, buf, (MD_SIZE) n);
    render_ansi(r, r->style->rule.off);
    render_newline(r);
}

/* Byte length of the longest prefix of buf (raw UTF-8, no ANSI escapes) that
 * fits within `width` display columns. */
static MD_SIZE
ansi_clip_bytes(const char* buf, MD_SIZE size, int width)
{
    MD_SIZE i = 0;
    int w = 0;
    while(i < size) {
        unsigned cp;
        MD_SIZE cl = ansi_utf8_decode(buf + i, size - i, &cp);
        int cw = cp_width(cp);
        if(w + cw > width)
            break;
        w += cw;
        i += cl;
    }
    return i;
}

/* Emit the buffered code block as plain dim text (the pre-highlighting path,
 * also the fallback when highlighting is unavailable or fails). Long lines are
 * clipped to the prose right margin (like glow); wrap_cols == 0 = no clip. */
static void
emit_plain_code(MD_ANSI* r)
{
    MD_SIZE i, start = 0;
    int avail = (r->wrap_cols > 0)
                ? r->wrap_cols - ansi_indent_width(r) - 2 - DOC_MARGIN : 0;
    if(r->wrap_cols > 0 && avail < 1) avail = 1;
    render_ansi(r, r->style->code_block.on);
    for(i = 0; i <= r->code_size; i++) {
        if(i == r->code_size || r->code_buf[i] == '\n') {
            if(i > start) {
                MD_SIZE len = i - start;
                render_indent(r);
                RENDER_VERBATIM(r, "  ");
                if(avail > 0)
                    len = ansi_clip_bytes(r->code_buf + start, len, avail);
                render_verbatim(r, r->code_buf + start, len);
            }
            if(i < r->code_size)
                render_newline(r);
            start = i + 1;
        }
    }
    render_ansi(r, r->style->code_block.off);
}

#ifdef MD4C_WITH_FYTS
/* fyts write sink: emit highlighted output verbatim into the renderer. */
static int
ansi_fyts_write(const void* data, size_t len, void* user)
{
    MD_ANSI* r = (MD_ANSI*) user;
    render_verbatim(r, (const char*) data, (MD_SIZE) len);
    return (int) len;
}

/* Highlight the buffered code block with libfyts. Each line is prefixed with the
 * current indent chrome (blockquote/list) plus the 2-space code margin via
 * fyts's line_prefix. In reverse (bubble) mode the header/footer rules are
 * handed to fyts as prolog/epilog so fyts frames them on the same background as
 * the code -- one cohesive bubble. Returns:
 *   0 = fall back to plain;  1 = emitted code (caller draws the footer);
 *   2 = emitted the whole block including header and footer. */
static int
emit_highlighted_code(MD_ANSI* r)
{
    struct fyts_config cfg;
    char lang[64];
    char prefix[256];
    char header[1024], footer[1024];
    ANSI_CAPTURE_BUF cap = { prefix, 0, sizeof(prefix) - 1 };
    void (*saved_out)(const MD_CHAR*, MD_SIZE, void*);
    void* saved_ud;
    int rc, reverse = r->style->code_reverse;
    /* Clip width for fyts: 0 (no wrap / MD_ANSI_WIDTH_INF) means no clipping,
     * matching prose. fyts subtracts the line_prefix (indent + 2-space margin)
     * itself, so reserving DOC_MARGIN here lands the content inside the rule box.
     * The header/footer rules below still need a finite width, so they fall back
     * to the terminal width separately. */
    int clip_width = (r->wrap_cols > 0) ? r->wrap_cols - DOC_MARGIN : 0;

    if(r->code_lang_size == 0 || r->code_size == 0)
        return 0;

    /* NUL-terminated language string. */
    memcpy(lang, r->code_lang, r->code_lang_size);
    lang[r->code_lang_size] = '\0';

    /* Capture the per-line indent prefix (chrome + 2-space code margin). */
    saved_out = r->process_output;
    saved_ud = r->userdata;
    r->process_output = ansi_capture_append;
    r->userdata = &cap;
    render_indent(r);
    RENDER_VERBATIM(r, "  ");
    r->process_output = saved_out;
    r->userdata = saved_ud;
    prefix[cap.size] = '\0';

    memset(&cfg, 0, sizeof(cfg));
    cfg.lang = lang;
    cfg.color_mode = (r->flags & MD_ANSI_FLAG_NO_COLOR) ? FYTS_COLOR_OFF : FYTS_COLOR_ON;
    switch(r->style->code_background) {
        case MD_STYLE_BG_DARK:  cfg.background_mode = FYTS_BACKGROUND_DARK;  break;
        case MD_STYLE_BG_LIGHT: cfg.background_mode = FYTS_BACKGROUND_LIGHT; break;
        default:                cfg.background_mode = FYTS_BACKGROUND_AUTO;  break;
    }
    /* "default" (or empty) uses fyts's embedded theme; any other value is a
     * path to a styling YAML file. */
    if(r->style->code_theme != NULL && r->style->code_theme[0] != '\0'
       && strcmp(r->style->code_theme, "default") != 0)
        cfg.styling_path = r->style->code_theme;
    cfg.reverse = reverse;
    cfg.line_prefix = prefix;
    /* fyts clips each line to `width`, subtracting the line_prefix width itself;
     * this leaves a content width matching the prose/header right margin. */
    cfg.width = clip_width;
    cfg.write = ansi_fyts_write;
    cfg.write_user = r;

    if(reverse) {
        /* Hand the header/footer rules to fyts so it frames them on the bubble
         * background (and extends them to the end of the line). */
        int width = (r->wrap_cols > 0) ? r->wrap_cols : table_term_width();
        int avail = width - r->indent_w - DOC_MARGIN;
        size_t hn, fn;
        if(avail < 4) avail = 4;
        hn = build_code_rule_text(r->style->table_horizontal, r->code_lang,
                                  r->code_lang_size, avail, header, sizeof(header) - 1);
        fn = build_code_rule_text(r->style->table_horizontal, NULL, 0,
                                  avail, footer, sizeof(footer) - 1);
        header[hn] = '\0';
        footer[fn] = '\0';
        cfg.prolog = header;
        cfg.epilog = footer;
    }

    rc = fyts_highlight_source(&cfg, r->code_buf, r->code_size);
    if(rc < 0)
        return 0;            /* 0 => caller emits the plain fallback */
    return reverse ? 2 : 1;  /* reverse: header+code+footer all done by fyts */
}
#endif /* MD4C_WITH_FYTS */

static int
enter_block_callback(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    MD_ANSI* r = (MD_ANSI*) userdata;

    switch(type) {
        case MD_BLOCK_DOC:
            break;

        case MD_BLOCK_QUOTE:
            if(r->need_newline) {
                render_separator(r);
                r->need_newline = 0;
            }
            r->quote_depth++;
            break;

        case MD_BLOCK_UL:
            if(r->need_newline && r->list_depth == 0) {
                render_separator(r);
                r->need_newline = 0;
            }
            break;

        case MD_BLOCK_OL:
            if(r->need_newline && r->list_depth == 0) {
                render_separator(r);
                r->need_newline = 0;
            }
            r->ol_counter = ((MD_BLOCK_OL_DETAIL*)detail)->start;
            break;

        case MD_BLOCK_LI: {
            const MD_BLOCK_LI_DETAIL* li = (const MD_BLOCK_LI_DETAIL*)detail;
            render_indent(r);
            if(li->is_task) {
                if(li->task_mark == 'x' || li->task_mark == 'X') {
                    render_ansi(r, r->style->task_done.on);
                    RENDER_VERBATIM(r, "[x] ");
                    render_ansi(r, r->style->task_done.off);
                } else {
                    RENDER_VERBATIM(r, "[ ] ");
                }
            } else {
                /* Check parent: is this inside OL or UL? We track via ol_counter. */
                if(r->ol_counter > 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d. ", r->ol_counter);
                    render_ansi(r, r->style->list_marker.on);
                    RENDER_VERBATIM(r, buf);
                    render_ansi(r, r->style->list_marker.off);
                    r->ol_counter++;
                } else {
                    render_ansi(r, r->style->list_marker.on);
                    RENDER_VERBATIM(r, r->style->list_bullet);
                    RENDER_VERBATIM(r, " ");
                    render_ansi(r, r->style->list_marker.off);
                }
            }
            r->list_depth++;
            r->li_opened = 1;
            break;
        }

        case MD_BLOCK_HR:
            if(r->need_newline) {
                render_separator(r);
                r->need_newline = 0;
            }
            render_code_rule(r, NULL, 0);   /* width-aware dim rule */
            r->need_newline = 1;
            break;

        case MD_BLOCK_H:
            if(r->need_newline) {
                render_separator(r);
                r->need_newline = 0;
            }
            render_indent(r);
            render_ansi(r, r->style->heading.on);
            break;

        case MD_BLOCK_CODE:
            if(r->need_newline) {
                render_separator(r);
                r->need_newline = 0;
            }
            r->in_code_block = 1;
            r->need_indent = 1;

            /* Capture the info string (language) and arm syntax highlighting. */
            r->code_highlight = 0;
            r->code_lang_size = 0;
            r->code_size = 0;
            {
                const MD_BLOCK_CODE_DETAIL* det = (const MD_BLOCK_CODE_DETAIL*) detail;
                if(det->lang.text != NULL && det->lang.size > 0) {
                    MD_SIZE sz = det->lang.size < sizeof(r->code_lang)
                               ? det->lang.size : sizeof(r->code_lang) - 1;
                    memcpy(r->code_lang, det->lang.text, sz);
                    r->code_lang_size = sz;
                }
            }
#ifdef MD4C_WITH_FYTS
            /* Highlight when the info string names a language libfyts supports;
             * the text is then buffered until the block closes (the plain
             * ANSI_DIM styling is skipped while doing so). Checking support up
             * front avoids fyts emitting an "unknown language" diagnostic. */
            if(r->style->code_enabled && r->code_lang_size > 0) {
                r->code_lang[r->code_lang_size] = '\0';
                if(fyts_language_supported(r->code_lang))
                    r->code_highlight = 1;
            }
#endif

            /* Header rule (with the language label, when present). In reverse
             * mode the header is deferred to leave, where it is drawn on fyts's
             * frame background together with the code. */
            if(!(r->code_highlight && r->style->code_reverse))
                render_code_rule(r, r->code_lang, r->code_lang_size);

            if(r->flags & MD_ANSI_FLAG_CODE_META) {
                MD_ANSI_CODE_META* meta = ansi_code_meta_push(r);
                if(meta != NULL) {
                    const MD_BLOCK_CODE_DETAIL* det = (const MD_BLOCK_CODE_DETAIL*) detail;
                    meta->start = r->output_offset;
                    if(det->lang.text != NULL && det->lang.size > 0) {
                        MD_SIZE sz = det->lang.size < sizeof(meta->lang) ? det->lang.size : sizeof(meta->lang) - 1;
                        memcpy(meta->lang, det->lang.text, sz);
                        meta->lang_size = sz;
                    }
                    /* Capture the indent prefix by temporarily redirecting output. */
                    {
                        char pfx_buf[256];
                        ANSI_CAPTURE_BUF cap = { pfx_buf, 0, sizeof(pfx_buf) };
                        void (*saved_out)(const MD_CHAR*, MD_SIZE, void*) = r->process_output;
                        void* saved_ud = r->userdata;
                        r->process_output = ansi_capture_append;
                        r->userdata = &cap;
                        render_indent(r);
                        RENDER_VERBATIM(r, "  ");
                        r->process_output = saved_out;
                        r->userdata = saved_ud;
                        if(cap.size <= sizeof(meta->prefix)) {
                            memcpy(meta->prefix, pfx_buf, cap.size);
                            meta->prefix_size = cap.size;
                        }
                    }
                }
            }
            if(!r->code_highlight)
                render_ansi(r, r->style->code_block.on);
            break;

        case MD_BLOCK_HTML:
            break;

        case MD_BLOCK_P:
            if(r->need_newline && !r->li_opened) {
                render_separator(r);
                r->need_newline = 0;
            }
            if(!r->li_opened)
                render_indent(r);
            r->li_opened = 0;
            break;

        case MD_BLOCK_TABLE:
            if(r->need_newline) {
                render_separator(r);
                r->need_newline = 0;
            }
            /* Begin buffering: cells are captured, then laid out on leave. */
            r->table = (MD_ANSI_TABLE*) calloc(1, sizeof(MD_ANSI_TABLE));
            break;

        case MD_BLOCK_THEAD:
            if(r->table != NULL) r->table->cur_is_header = 1;
            break;

        case MD_BLOCK_TBODY:
            if(r->table != NULL) r->table->cur_is_header = 0;
            break;

        case MD_BLOCK_TR:
            if(r->table != NULL)
                table_push_row(r->table, r->table->cur_is_header);
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            if(r->table != NULL) {
                MD_ANSI_TCELL* cell = table_push_cell(r->table);
                const MD_BLOCK_TD_DETAIL* td = (const MD_BLOCK_TD_DETAIL*) detail;
                if(r->table->n_rows > 0) {
                    int col = r->table->rows[r->table->n_rows - 1].n_cells - 1;
                    if(td != NULL) table_set_align(r->table, col, td->align);
                }
                r->table->cur = cell;
                r->table->capturing = 1;
            }
            break;

        default:
            /* md4c types we don't enable/render (footnotes, admonitions, etc.) */
            break;
    }

    return 0;
}

static int
leave_block_callback(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    MD_ANSI* r = (MD_ANSI*) userdata;

    (void) detail;

    switch(type) {
        case MD_BLOCK_DOC:
            break;

        case MD_BLOCK_QUOTE:
            r->quote_depth--;
            break;

        case MD_BLOCK_UL:
            r->ol_counter = 0;
            r->li_opened = 0;
            r->need_newline = 1;
            break;

        case MD_BLOCK_OL:
            r->ol_counter = 0;
            r->li_opened = 0;
            r->need_newline = 1;
            break;

        case MD_BLOCK_LI:
            r->list_depth--;
            render_newline(r);
            break;

        case MD_BLOCK_HR:
            break;

        case MD_BLOCK_H:
            render_ansi(r, r->style->heading.off);
            render_newline(r);
            r->need_newline = 1;
            break;

        case MD_BLOCK_CODE: {
            /* done: 0 = not highlighted / fell back, 1 = code emitted (draw the
             * footer here), 2 = whole block (header+code+footer) already emitted. */
            int done = 0;
            if(r->code_highlight) {
#ifdef MD4C_WITH_FYTS
                done = emit_highlighted_code(r);
#endif
                if(done == 0) {
                    /* Fall back to plain. In reverse mode the header was deferred
                     * to here, so draw it (plain) before the body. */
                    if(r->style->code_reverse)
                        render_code_rule(r, r->code_lang, r->code_lang_size);
                    emit_plain_code(r);
                }
                r->code_highlight = 0;
            } else {
                render_ansi(r, r->style->code_block.off);
            }
            if((r->flags & MD_ANSI_FLAG_CODE_META) && r->n_code_blocks < r->code_blocks_cap) {
                r->code_blocks[r->n_code_blocks].end = r->output_offset;
                r->n_code_blocks++;
            }
            if(done != 2)
                render_code_rule(r, NULL, 0);   /* footer (label-less) */
            r->in_code_block = 0;
            r->need_newline = 1;
            break;
        }

        case MD_BLOCK_HTML:
            break;

        case MD_BLOCK_P:
            render_newline(r);
            r->need_newline = 1;
            break;

        case MD_BLOCK_TABLE:
            if(r->table != NULL) {
                if(!r->table->oom)
                    table_emit(r);
                table_free(r->table);
                r->table = NULL;
            }
            r->need_newline = 1;
            break;

        case MD_BLOCK_THEAD:
            break;

        case MD_BLOCK_TBODY:
            break;

        case MD_BLOCK_TR:
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            if(r->table != NULL) {
                r->table->capturing = 0;
                r->table->cur = NULL;
            }
            break;

        default:
            break;
    }

    return 0;
}

static int
enter_span_callback(MD_SPANTYPE type, void* detail, void* userdata)
{
    MD_ANSI* r = (MD_ANSI*) userdata;

    if(type == MD_SPAN_IMG)
        r->image_nesting_level++;

    if(r->image_nesting_level > 0 && type != MD_SPAN_IMG)
        return 0;

    switch(type) {
        case MD_SPAN_EM:                render_ansi(r, r->style->emphasis.on); break;
        case MD_SPAN_STRONG:            render_ansi(r, r->style->strong.on); break;
        case MD_SPAN_U:                 render_ansi(r, r->style->underline.on); break;
        case MD_SPAN_A: {
            const MD_SPAN_A_DETAIL* a = (const MD_SPAN_A_DETAIL*) detail;
            /* OSC 8 hyperlink: makes text clickable in supported terminals */
            if(!(r->flags & MD_ANSI_FLAG_NO_COLOR) && a->href.size > 0) {
                RENDER_VERBATIM(r, ANSI_HYPERLINK_OPEN);
                render_attribute(r, &a->href, render_verbatim);
                RENDER_VERBATIM(r, ANSI_HYPERLINK_SEP);
            }
            render_ansi(r, r->style->link.on);
            break;
        }
        case MD_SPAN_IMG:
        //   render_ansi(r, ANSI_DIM);
        //     RENDER_VERBATIM(r, "[image: ");
        /* Images are suppressed — alt text is silently skipped via image_nesting_level */
            break;
        case MD_SPAN_CODE:              render_ansi(r, r->style->code.on); break;
        case MD_SPAN_DEL:               render_ansi(r, r->style->strikethrough.on); break;
        case MD_SPAN_LATEXMATH:         render_ansi(r, r->style->math.on); break;
        case MD_SPAN_LATEXMATH_DISPLAY: render_ansi(r, r->style->math.on); break;
        case MD_SPAN_WIKILINK:          render_ansi(r, r->style->wikilink.on); break;
        default:                        break;
    }

    return 0;
}

static int
leave_span_callback(MD_SPANTYPE type, void* detail, void* userdata)
{
    MD_ANSI* r = (MD_ANSI*) userdata;

    if(type == MD_SPAN_IMG)
        r->image_nesting_level--;

    if(r->image_nesting_level > 0)
        return 0;

    switch(type) {
        case MD_SPAN_EM:                render_ansi(r, r->style->emphasis.off); break;
        case MD_SPAN_STRONG:            render_ansi(r, r->style->strong.off); break;
        case MD_SPAN_U:                 render_ansi(r, r->style->underline.off); break;
        case MD_SPAN_A: {
            const MD_SPAN_A_DETAIL* a = (const MD_SPAN_A_DETAIL*) detail;
            render_ansi(r, r->style->link.off);
            /* Close OSC 8 hyperlink */
            if(!(r->flags & MD_ANSI_FLAG_NO_COLOR) && a->href.size > 0)
                RENDER_VERBATIM(r, ANSI_HYPERLINK_CLOSE);
            /* Show URL as dim fallback for terminals without OSC 8 */
            if((r->flags & MD_ANSI_FLAG_SHOW_URLS) && a->href.size > 0 && !a->is_autolink) {
                render_ansi(r, r->style->link_url.on);
                RENDER_VERBATIM(r, " (");
                render_attribute(r, &a->href, render_verbatim);
                RENDER_VERBATIM(r, ")");
                render_ansi(r, r->style->link_url.off);
            }
            break;
        }
        case MD_SPAN_IMG:
            break;
        case MD_SPAN_CODE:              render_ansi(r, r->style->code.off); break;
        case MD_SPAN_DEL:               render_ansi(r, r->style->strikethrough.off); break;
        case MD_SPAN_LATEXMATH:         render_ansi(r, r->style->math.off); break;
        case MD_SPAN_LATEXMATH_DISPLAY: render_ansi(r, r->style->math.off); break;
        case MD_SPAN_WIKILINK:          render_ansi(r, r->style->wikilink.off); break;
        default:                        break;
    }

    return 0;
}

static int
text_callback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    MD_ANSI* r = (MD_ANSI*) userdata;

    /* Inside a table cell, line breaks collapse to a single space. */
    if(r->table != NULL && r->table->capturing
       && (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR)) {
        RENDER_VERBATIM(r, " ");
        return 0;
    }

    /* Filter ANSI escape sequences carried in the raw input text (normal text,
     * code, math) unless passthrough (KEEP) is requested. Escapes never appear
     * in the synthetic text types (BR/entity/...), so only these carry them. */
    if((type == MD_TEXT_NORMAL || type == MD_TEXT_CODE || type == MD_TEXT_LATEXMATH)
       && !(r->flags & MD_ANSI_FLAG_SGR_KEEP)
       && size > 0 && memchr(text, 0x1b, size) != NULL) {
        if(r->sgr_cap < size) {
            char* p = (char*) realloc(r->sgr_buf, size);
            if(p != NULL) { r->sgr_buf = p; r->sgr_cap = size; }
        }
        if(r->sgr_cap >= size) {
            size = sgr_filter_input(r->flags, text, size, r->sgr_buf);
            text = r->sgr_buf;
            if(size == 0)
                return 0;   /* the chunk was entirely stripped escapes */
        }
    }

    switch(type) {
        case MD_TEXT_NULLCHAR:
            render_utf8_codepoint(r, 0x0000, render_verbatim);
            break;

        case MD_TEXT_BR:
            render_newline(r);
            render_indent(r);
            break;

        case MD_TEXT_SOFTBR:
            if(r->image_nesting_level != 0) {
                RENDER_VERBATIM(r, " ");
            } else if(r->wrap_cols > 0 && !r->in_code_block) {
                /* When wrapping, a soft break becomes a space so the whole
                 * paragraph reflows to the target width (like glow). */
                RENDER_VERBATIM(r, " ");
            } else {
                render_newline(r);
                render_indent(r);
            }
            break;

        case MD_TEXT_HTML:
            /* Raw HTML: suppress in terminal output */
            break;

        case MD_TEXT_ENTITY:
            render_entity(r, text, size, render_verbatim);
            break;

        case MD_TEXT_CODE:
            if(r->in_code_block && r->code_highlight) {
                /* Buffer the raw code; it is highlighted as a whole on leave. */
                code_buf_append(r, text, size);
            } else if(r->in_code_block) {
                /* Inside code block: the parser sends each line and its \n
                 * as separate callbacks. We use need_indent to track when
                 * we need to emit the indent prefix at line start. */
                if(size == 1 && text[0] == '\n') {
                    render_newline(r);
                    r->need_indent = 1;
                } else {
                    if(r->need_indent) {
                        render_indent(r);
                        RENDER_VERBATIM(r, "  ");
                        r->need_indent = 0;
                        r->code_col = 0;
                        /* Content sits inside the rule box: indent + 2-space code
                         * margin on the left, prose right margin on the right. */
                        r->code_clip = (r->wrap_cols > 0)
                            ? r->wrap_cols - ansi_indent_width(r) - 2 - DOC_MARGIN : 0;
                        if(r->wrap_cols > 0 && r->code_clip < 1) r->code_clip = 1;
                    }
                    /* Clip the line to the prose right margin (wrap_cols == 0 =
                     * unlimited); a line may arrive over several callbacks. */
                    if(r->code_clip > 0) {
                        int budget = r->code_clip - r->code_col;
                        MD_SIZE len;
                        if(budget <= 0)
                            break;   /* rest of this line is past the margin */
                        len = ansi_clip_bytes(text, size, budget);
                        r->code_col += ansi_disp_width(text, len);
                        render_verbatim(r, text, len);
                    } else {
                        render_verbatim(r, text, size);
                    }
                }
            } else {
                /* Inline code span */
                render_verbatim(r, text, size);
            }
            break;

        default:
            render_verbatim(r, text, size);
            break;
    }

    return 0;
}

static void
debug_log_callback(const char* msg, void* userdata)
{
    MD_ANSI* r = (MD_ANSI*) userdata;
    if(r->flags & MD_ANSI_FLAG_DEBUG)
        fprintf(stderr, "MD4C: %s\n", msg);
}

int
md_ansi(const MD_CHAR* input, MD_SIZE input_size,
        void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
        void* userdata, unsigned parser_flags, unsigned renderer_flags)
{
    return md_ansi_ex(input, input_size, process_output, userdata,
                      parser_flags, renderer_flags, MD_ANSI_WIDTH_AUTO);
}

int
md_ansi_ex(const MD_CHAR* input, MD_SIZE input_size,
           void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
           void* userdata, unsigned parser_flags, unsigned renderer_flags,
           int width)
{
    return md_ansi_ex_styled(input, input_size, process_output, userdata,
                             parser_flags, renderer_flags, width, NULL);
}

int
md_ansi_ex_styled(const MD_CHAR* input, MD_SIZE input_size,
                  void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                  void* userdata, unsigned parser_flags, unsigned renderer_flags,
                  int width, const struct MD_ANSI_STYLE* style)
{
    MD_ANSI render;
    MD_PARSER parser;
    MD_ANSI_STYLE* owned_style = NULL;

    /* Heal-before-render: run md_heal first, then render the healed output. */
    if(renderer_flags & MD_ANSI_FLAG_HEAL) {
        MD4C_HEAL_BUF hbuf;
        int ret;
        if(md4c_heal_input(input, input_size, &hbuf) != 0) {
            free(hbuf.data);
            return -1;
        }
        ret = md_ansi_ex_styled(hbuf.data, hbuf.size, process_output, userdata,
                                parser_flags, renderer_flags & ~MD_ANSI_FLAG_HEAL,
                                width, style);
        free(hbuf.data);
        return ret;
    }

    memset(&parser, 0, sizeof(parser));
    parser.flags = parser_flags;
    parser.enter_block = enter_block_callback;
    parser.leave_block = leave_block_callback;
    parser.enter_span = enter_span_callback;
    parser.leave_span = leave_span_callback;
    parser.text = text_callback;
    parser.debug_log = debug_log_callback;

    memset(&render, 0, sizeof(render));
    render.process_output = process_output;
    render.userdata = userdata;
    render.flags = renderer_flags;
    render.table_width = width;
    if(style == NULL) {
        owned_style = md_ansi_style_create(NULL, 0, NULL);
        if(owned_style == NULL)
            return -1;
        style = owned_style;
    }
    render.style = style;
    /* Resolve the prose wrap width: fixed, auto-detected, or 0 (no wrap). */
    if(width == MD_ANSI_WIDTH_INF)
        render.wrap_cols = 0;
    else if(width > 0)
        render.wrap_cols = width;
    else
        render.wrap_cols = table_term_width();

    /* Consider skipping UTF-8 byte order mark (BOM). */
    if(renderer_flags & MD_ANSI_FLAG_SKIP_UTF8_BOM  &&  sizeof(MD_CHAR) == 1) {
        static const MD_CHAR bom[3] = { (char)0xef, (char)0xbb, (char)0xbf };
        if(input_size >= sizeof(bom)  &&  memcmp(input, bom, sizeof(bom)) == 0) {
            input += sizeof(bom);
            input_size -= sizeof(bom);
        }
    }

    {
        int ret = md_parse(input, input_size, &parser, (void*) &render);

        if(renderer_flags & MD_ANSI_FLAG_CODE_META) {
            if(ret == 0)
                render_ansi_code_meta_json(&render);
            ansi_code_meta_cleanup(&render);
        }

        /* Flush any line still buffered by the wrapper. */
        if(render.line_open)
            flush_wrapped(&render);
        free(render.lbuf);
        free(render.code_buf);
        free(render.sgr_buf);

        /* Free any table left dangling by an aborted parse. */
        if(render.table != NULL)
            table_free(render.table);

        if(owned_style != NULL)
            md_ansi_style_destroy(owned_style);
        return ret;
    }
}
