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

#ifdef _MSC_VER
#include <malloc.h>
#define FYMD_ALLOCA _alloca
#else
#include <alloca.h>
#define FYMD_ALLOCA alloca
#endif

#if defined(unix) || defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    #if !defined(__wasi__) && !defined(__wasm__)
        #define MD4C_ANSI_HAVE_IOCTL 1
        #include <unistd.h>
        #include <sys/ioctl.h>
    #endif
#endif

#include <libfyaml/libfyaml-allocator.h>
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
    int in_code_block;
    int code_footer_pending; /* streaming: trailing code-block footer deferred */
    int need_newline;       /* pending newline before next block */
    int need_indent;        /* emit indent prefix on next code text */
    int code_col;           /* display column within the current code line (clip) */
    int code_clip;          /* max code-content columns per line; 0 = no clip */
    int li_opened;          /* just opened a list item (bullet already printed) */
    int line_dirty;         /* content emitted on the current line, no newline yet */

    /* Stack of open lists (UL/OL), one entry per nesting level, so a nested
     * list's marker type and counter don't leak from its parent. */
#define MD_ANSI_MAX_LIST 32
    struct {
        int ordered;        /* 1 = ordered (numbered), 0 = bullet */
        int counter;        /* next number for ordered lists */
        int tight;          /* md4c is_tight: no blank line between items */
        int seen;           /* an item has already been rendered in this list */
    } lists[MD_ANSI_MAX_LIST];
    int list_sp;            /* number of open lists (stack depth) */

    MD_ANSI_TABLE* table;   /* non-NULL while inside a table block */
    int table_width;        /* >0 fixed, 0 = unlimited, <0 = auto-detect */
    const MD_ANSI_STYLE* style;  /* element styling (never NULL during render) */
    fy_generic template_vars;    /* borrowed raw-fence {key} values */
    size_t template_lines;
    size_t template_plain_lines;
    size_t template_hidden_lines;

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

    /* Whole-document "card" mode (MD_ANSI_FLAG_REVERSE): each output line is
     * given the theme background and padded to the full width. Output is
     * buffered a line at a time and transformed before reaching the real sink. */
    int card;                       /* card mode active (bg from style->reverse.on) */
    void (*real_output)(const MD_CHAR*, MD_SIZE, void*); /* sink when not capturing */
    char* card_buf;                 /* current line being accumulated */
    MD_SIZE card_size, card_cap;
    const char* table_row_on;       /* active complete-row style to replay on reset */
};


/*********************************************
 ***  ANSI rendering helper functions  ***
 *********************************************/

/* Forward declarations (definitions live in the table-layout section). */
typedef struct { MD_SIZE start; MD_SIZE len; int w; } TLINE;
static void table_cell_append(MD_ANSI_TABLE* t, const MD_CHAR* text, MD_SIZE size);
static MD_SIZE ansi_clip_bytes(const char* buf, MD_SIZE size, int width);

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
static MD_SIZE ansi_esc_len(const char* s, MD_SIZE n);

/* True if an "ESC[...m" sequence (given its params, the bytes between "[" and
 * "m") is a full reset (empty, or all zeros) -- the kind that clears the card
 * background and so needs it re-applied afterwards. */
static int
sgr_is_reset(const char* params, MD_SIZE n)
{
    MD_SIZE i;
    for(i = 0; i < n; i++)
        if(params[i] != '0' && params[i] != ';')
            return 0;
    return 1;
}

/* Emit one completed output line (r->card_buf, no trailing newline) as a card:
 * the theme background, the content with the background re-applied after every
 * full reset, then erase-to-end-of-line (which fills the background to the edge)
 * and a reset. Called only when card mode is active and not mid-capture. */
static void
flush_card_line(MD_ANSI* r)
{
    const char* b = r->card_buf;
    const char* bg = r->style->reverse.on;      /* card background (from YAML) */
    const char* off = r->style->reverse.off;    /* reset (from YAML) */
    MD_SIZE bglen = (MD_SIZE) strlen(bg);
    MD_SIZE n = r->card_size, i = 0;

    r->real_output(bg, bglen, r->userdata);
    while(i < n) {
        MD_SIZE e = ansi_esc_len(b + i, n - i);
        if(e > 0) {
            r->real_output(b + i, e, r->userdata);
            /* A full reset in the content clears the card background; put it back. */
            if(e >= 3 && (unsigned char) b[i] == 0x1b && b[i + 1] == '['
               && b[i + e - 1] == 'm' && sgr_is_reset(b + i + 2, e - 3))
                r->real_output(bg, bglen, r->userdata);
            i += e;
        } else {
            MD_SIZE j = i;
            while(j < n && (unsigned char) b[j] != 0x1b) j++;
            r->real_output(b + i, j - i, r->userdata);
            i = j;
        }
    }
    r->real_output(bg, bglen, r->userdata);          /* bg active for the fill */
    r->real_output("\x1b[K", 3, r->userdata);        /* erase to EOL (structural) */
    r->real_output(off, (MD_SIZE) strlen(off), r->userdata);
    r->real_output("\n", 1, r->userdata);
    r->card_size = 0;
}

/* Append `n` bytes to the current card line buffer, growing it as needed. */
static void
card_append(MD_ANSI* r, const MD_CHAR* text, MD_SIZE n)
{
    if(n == 0)
        return;
    if(r->card_size + n > r->card_cap) {
        MD_SIZE nc = r->card_cap ? r->card_cap : 256;
        char* p;
        while(nc < r->card_size + n) nc *= 2;
        p = (char*) realloc(r->card_buf, nc);
        if(p == NULL) return;
        r->card_buf = p; r->card_cap = nc;
    }
    memcpy(r->card_buf + r->card_size, text, n);
    r->card_size += n;
}

/* Accumulate output into the current line; emit each completed line as a card. */
static void
card_feed(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    MD_SIZE start = 0, i;
    for(i = 0; i < size; i++) {
        if(text[i] != '\n')
            continue;
        card_append(r, text + start, i - start);
        flush_card_line(r);
        start = i + 1;
    }
    card_append(r, text + start, size - start);
}

static void
out_sink(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    if(r->card && r->process_output == r->real_output)
        card_feed(r, text, size);
    else
        r->process_output(text, size, r->userdata);
}

/* Write bytes straight to the output callback (bypassing the line buffer). */
static void
out_direct(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    MD_SIZE i, j, e;

    /* A full reset inside inline cell content also clears the row background.
     * Replay the active row style immediately; card mode then performs its own
     * outer background replay when the completed line is flushed. */
    if(r->table_row_on != NULL && r->table_row_on[0] != '\0' &&
       r->process_output == r->real_output) {
        for(i = 0; i < size; ) {
            e = ansi_esc_len(text + i, size - i);
            if(e > 0) {
                out_sink(r, text + i, e);
                if(e >= 3 && (unsigned char)text[i] == 0x1b && text[i + 1] == '[' &&
                   text[i + e - 1] == 'm' && sgr_is_reset(text + i + 2, e - 3))
                    out_sink(r, r->table_row_on,
                             (MD_SIZE)strlen(r->table_row_on));
                i += e;
            } else {
                for(j = i; j < size && (unsigned char)text[j] != 0x1b; j++)
                    ;
                out_sink(r, text + i, j - i);
                i = j;
            }
        }
    } else {
        out_sink(r, text, size);
    }
    if(size > 0)
        r->line_dirty = (text[size - 1] != '\n');
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
    if(size > 0)
        r->line_dirty = 1;   /* buffered content will be flushed onto this line */
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

/* Render a blank separator line. Inside a blockquote the quote bar(s) are kept
 * so the empty line still reads as part of the quote (like glow / GitHub). */
static void
render_separator(MD_ANSI* r)
{
    if(r->quote_depth > 0) {
        int i, saved = r->wrap_suspend;
        r->wrap_suspend = 1;             /* emit the bars straight to output */
        for(i = 0; i < DOC_MARGIN; i++)
            RENDER_VERBATIM(r, " ");
        for(i = 0; i < r->quote_depth; i++) {
            render_ansi(r, r->style->blockquote.on);
            RENDER_VERBATIM(r, r->style->blockquote_bar);
            render_ansi(r, r->style->blockquote.off);
            if(i + 1 < r->quote_depth)
                RENDER_VERBATIM(r, " ");
        }
        r->wrap_suspend = saved;
    }
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

/* Combining marks (Mn/Me) and zero-width format characters (no column each).
 * Generated by scripts/gen-wcwidth.py; regenerate after a Unicode bump. */
static const CP_RANGE zero_width_ranges[] = {
#include "md4c-wcwidth-zero.h"
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
table_emit_row(MD_ANSI* r, MD_ANSI_TROW* row, const int* widths, int n_cols,
               const MD_STYLE_PAIR* row_style)
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
        if(row_style != NULL && row_style->on[0] != '\0') {
            r->table_row_on = row_style->on;
            render_ansi(r, row_style->on);
        }
        RENDER_VERBATIM(r, " ");                 /* outer left cell padding */
        for(j = 0; j < n_cols; j++) {
            MD_ALIGN align = (j < r->table->n_aligns) ? r->table->aligns[j] : MD_ALIGN_DEFAULT;
            if(j > 0) {
                RENDER_VERBATIM(r, " ");
                if(!r->style->table_border_none)
                    RENDER_VERBATIM(r, r->style->table_vertical);
                RENDER_VERBATIM(r, " ");
            }
            if(k < nlines[j])
                table_emit_slice(r, bufs[j], wrapped[j][k], widths[j], align, row->is_header);
            else
                tbl_spaces(r, widths[j]);
        }
        RENDER_VERBATIM(r, " ");                 /* outer right cell padding */
        if(r->table_row_on != NULL) {
            r->table_row_on = NULL;
            render_ansi(r, row_style->off);
        }
        render_newline(r);
    }

    for(j = 0; j < n_cols; j++)
        free(wrapped[j]);
    free(wrapped); free(nlines); free((void*) bufs);
}

static void
table_emit_separator(MD_ANSI* r, const int* widths, int n_cols,
                     const MD_STYLE_PAIR* row_style)
{
    int j, k;
    const char* h = r->style->table_horizontal;
    render_indent(r);
    if(row_style != NULL && row_style->on[0] != '\0') {
        r->table_row_on = row_style->on;
        render_ansi(r, row_style->on);
    }
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
    if(r->table_row_on != NULL) {
        r->table_row_on = NULL;
        render_ansi(r, row_style->off);
    }
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
    int body_row = 0;

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
        int overhead = 2 + (r->style->table_border_none ? 2 : 3) * (n_cols - 1);
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
            const MD_STYLE_PAIR* row_style;
            if(!row->is_header && any_header && !emitted_sep) {
                if(!r->style->table_border_none)
                    table_emit_separator(r, widths, n_cols,
                                         &r->style->table_header_row);
                emitted_sep = 1;
            }
            if(row->is_header) {
                row_style = &r->style->table_header_row;
            } else {
                row_style = (body_row++ & 1) ? &r->style->table_row_even
                                             : &r->style->table_row_odd;
            }
            table_emit_row(r, row, widths, n_cols, row_style);
        }
        if(any_header && !emitted_sep && !r->style->table_border_none)
            table_emit_separator(r, widths, n_cols,
                                 &r->style->table_header_row);

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

/* Build the template mapping by copying caller values first, then appending
 * renderer values that the caller did not provide. */
static fy_generic
build_code_template_map(const MD_ANSI_STYLE* style, fy_generic vars,
                        const char* lang, MD_SIZE lang_size, int cols,
                        size_t lines, size_t plain_lines, size_t hidden_lines,
                        struct fy_generic_builder** gbp)
{
    fy_generic map, base, renderer_values;
    fy_generic_sized_string language_value, fill_value;
    size_t i;
    char* fill = NULL;
    size_t hlen = strlen(style->table_horizontal), fill_len;
    struct fy_generic_builder* gb;
    char lines_text[32], plain_lines_text[32], hidden_lines_text[32];

    *gbp = NULL;
    snprintf(lines_text, sizeof(lines_text), "%zu", lines);
    snprintf(plain_lines_text, sizeof(plain_lines_text), "%zu", plain_lines);
    snprintf(hidden_lines_text, sizeof(hidden_lines_text), "%zu", hidden_lines);
    language_value.data = lang != NULL ? lang : "";
    language_value.size = lang != NULL ? lang_size : 0;
    fill_value.data = "";
    fill_value.size = 0;
    if(hlen > 0 && (size_t)cols <= (SIZE_MAX - 1) / hlen) {
        fill_len = (size_t)cols * hlen;
        fill = (char*) FYMD_ALLOCA(fill_len);
        for(i = 0; i < (size_t)cols; i++)
            memcpy(fill + i * hlen, style->table_horizontal, hlen);
        fill_value.data = fill;
        fill_value.size = fill_len;
    }

    gb = fy_generic_builder_create(NULL);
    if(gb == NULL)
        return fy_invalid;
    renderer_values = fy_null_filtered_mapping(gb,
            "language", lang != NULL ? fy_value(language_value) : fy_null,
            "rule", style->table_horizontal != NULL
                        ? fy_value(style->table_horizontal) : fy_null,
            "fill", fill != NULL ? fy_value(fill_value) : fy_null,
            "lines", fy_value(lines_text),
            "plain-lines", fy_value(plain_lines_text),
            "hidden-lines", fy_value(hidden_lines_text));
    base = fy_generic_is_mapping(vars) ? vars : fy_map_empty;
    map = fy_merge(gb, base, renderer_values);
    if(fy_generic_is_valid(map))
        *gbp = gb;
    else
        fy_generic_builder_destroy(gb);
    return map;
}

/* Expand every {key} by looking it up in the composed generic mapping.
 * "default" preserves the legacy width-aware rule. */
static size_t
build_code_decoration_text(const MD_ANSI_STYLE* style, fy_generic vars,
                           const char* tmpl,
                           const char* lang, MD_SIZE lang_size, int cols,
                           size_t lines, size_t plain_lines, size_t hidden_lines,
                           char* buf, size_t bufsz)
{
    size_t i = 0, n = 0;
    const char* close;
    const char* value;
    fy_generic_sized_string key;
    struct fy_generic_builder* gb = NULL;
    fy_generic context;

    if(tmpl == NULL || tmpl[0] == '\0')
        return 0;
    if(strcmp(tmpl, "default") == 0)
        return build_code_rule_text(style->table_horizontal, lang, lang_size,
                                    cols, buf, bufsz);
    context = build_code_template_map(style, vars, lang, lang_size, cols,
                                      lines, plain_lines, hidden_lines, &gb);
    if(!fy_generic_is_valid(context))
        return 0;
#define APPEND(p, len) do { size_t _l = (len); if(n + _l <= bufsz) { \
        memcpy(buf + n, (p), _l); n += _l; } } while(0)
    while(tmpl[i] != '\0') {
        if(tmpl[i] == '{') {
            close = strchr(tmpl + i + 1, '}');
            if(close != NULL) {
                key.data = tmpl + i + 1;
                key.size = (size_t)(close - key.data);
                value = fy_get(context, key, "");
                APPEND(value, strlen(value));
                i = (size_t)(close - tmpl) + 1;
                continue;
            }
        }
        APPEND(tmpl + i, 1);
        i++;
    }
#undef APPEND
    if(ansi_disp_width(buf, (MD_SIZE)n) > cols)
        n = ansi_clip_bytes(buf, (MD_SIZE)n, cols);
    fy_generic_builder_destroy(gb);
    return n;
}

/* Emit a themed header/footer line delimiting a fenced code block. */
static void
render_code_rule(MD_ANSI* r, const char* lang, MD_SIZE lang_size)
{
    const char* tmpl = lang != NULL ? r->style->code_header
                                    : r->style->code_footer;
    int width, avail;
    char buf[1024];
    size_t n;

    if(tmpl == NULL || tmpl[0] == '\0')
        return;
    render_indent(r);
    width = (r->wrap_cols > 0) ? r->wrap_cols : table_term_width();
    avail = width - r->indent_w - DOC_MARGIN;   /* match prose right margin */
    if(avail < 4) avail = 4;

    n = build_code_decoration_text(r->style, r->template_vars,
                                   tmpl, lang, lang_size,
                                   avail, r->template_lines,
                                   r->template_plain_lines,
                                   r->template_hidden_lines,
                                   buf, sizeof(buf));
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
    int prefixw = ansi_disp_width(r->style->code_prefix,
                                  (MD_SIZE)strlen(r->style->code_prefix));
    int avail = (r->wrap_cols > 0)
                ? r->wrap_cols - ansi_indent_width(r) - prefixw - DOC_MARGIN : 0;
    if(r->wrap_cols > 0 && avail < 1) avail = 1;
    render_ansi(r, r->style->code_block.on);
    for(i = 0; i <= r->code_size; i++) {
        if(i == r->code_size || r->code_buf[i] == '\n') {
            if(i > start) {
                MD_SIZE len = i - start;
                render_indent(r);
                RENDER_VERBATIM(r, r->style->code_prefix);
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
emit_highlighted_code(MD_ANSI* r, int styled)
{
    struct fyts_config cfg;
    char lang[64];
    char prefix[256];
    char header[1024], footer[1024];
    ANSI_CAPTURE_BUF cap = { prefix, 0, sizeof(prefix) - 1 };
    void (*saved_out)(const MD_CHAR*, MD_SIZE, void*);
    void* saved_ud;
    /* The fenced-code bubble is a special case: in whole-document card mode it
     * is suppressed, so code is highlighted normally and sits on the card. */
    int rc, reverse = styled && r->style->code_reverse && !r->card;
    /* Clip width for fyts: 0 (no wrap / MD_ANSI_WIDTH_INF) means no clipping,
     * matching prose. fyts subtracts the line_prefix (indent + 2-space margin)
     * itself, so reserving DOC_MARGIN here lands the content inside the rule box.
     * The header/footer rules below still need a finite width, so they fall back
     * to the terminal width separately. */
    int clip_width = (r->wrap_cols > 0)
                   ? r->wrap_cols - (styled ? DOC_MARGIN : 0) : 0;

    if(r->code_lang_size == 0 || r->code_size == 0)
        return 0;

    /* NUL-terminated language string. */
    memcpy(lang, r->code_lang, r->code_lang_size);
    lang[r->code_lang_size] = '\0';

    /* Capture the per-line indent prefix (chrome + themed code prefix). */
    saved_out = r->process_output;
    saved_ud = r->userdata;
    r->process_output = ansi_capture_append;
    r->userdata = &cap;
    if(styled) {
        render_indent(r);
        RENDER_VERBATIM(r, r->style->code_prefix);
    }
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
    /* code.theme selects the fyts styling: a value containing '/' is a path to
     * a styling YAML file; any other non-empty value is the name of a libfyts
     * built-in styling (including "default"). Empty/NULL leaves fyts on its
     * embedded default. */
    if(r->style->code_theme != NULL && r->style->code_theme[0] != '\0') {
        if(strchr(r->style->code_theme, '/') != NULL)
            cfg.styling_path = r->style->code_theme;
        else
            cfg.styling_name = r->style->code_theme;
    }
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
        hn = build_code_decoration_text(r->style, r->template_vars,
                                        r->style->code_header,
                                        r->code_lang, r->code_lang_size,
                                        avail, r->template_lines,
                                        r->template_plain_lines,
                                        r->template_hidden_lines,
                                        header, sizeof(header) - 1);
        fn = build_code_decoration_text(r->style, r->template_vars,
                                        r->style->code_footer,
                                        NULL, 0, avail, r->template_lines,
                                        r->template_plain_lines,
                                        r->template_hidden_lines,
                                        footer, sizeof(footer) - 1);
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

    /* Another block follows a deferred trailing code block, so it was not the
     * end of input after all: draw the footer we held back before laying it out. */
    if(r->code_footer_pending) {
        r->code_footer_pending = 0;
        render_code_rule(r, NULL, 0);
    }

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
        case MD_BLOCK_OL:
            if(r->need_newline && r->list_depth == 0) {
                render_separator(r);
                r->need_newline = 0;
            }
            /* A nested list starts on its own line: end the parent item's line
             * (tight items carry their text with no closing paragraph). */
            if(r->list_sp > 0 && r->line_dirty)
                render_newline(r);
            if(r->list_sp < MD_ANSI_MAX_LIST) {
                r->lists[r->list_sp].ordered = (type == MD_BLOCK_OL);
                r->lists[r->list_sp].counter =
                    (type == MD_BLOCK_OL) ? ((MD_BLOCK_OL_DETAIL*)detail)->start : 0;
                r->lists[r->list_sp].tight = (type == MD_BLOCK_OL)
                    ? ((MD_BLOCK_OL_DETAIL*)detail)->is_tight
                    : ((MD_BLOCK_UL_DETAIL*)detail)->is_tight;
                r->lists[r->list_sp].seen = 0;
            }
            r->list_sp++;
            break;

        case MD_BLOCK_LI: {
            const MD_BLOCK_LI_DETAIL* li = (const MD_BLOCK_LI_DETAIL*)detail;
            int top = r->list_sp - 1;
            /* Loose lists put a blank line between items (after the first). */
            if(top >= 0 && top < MD_ANSI_MAX_LIST && r->lists[top].seen
               && !r->lists[top].tight)
                render_separator(r);
            if(top >= 0 && top < MD_ANSI_MAX_LIST)
                r->lists[top].seen = 1;
            render_indent(r);
            if(li->is_task) {
                if(li->task_mark == 'x' || li->task_mark == 'X') {
                    render_ansi(r, r->style->task_done.on);
                    RENDER_VERBATIM(r, "[x] ");
                    render_ansi(r, r->style->task_done.off);
                } else {
                    RENDER_VERBATIM(r, "[ ] ");
                }
            } else if(top >= 0 && top < MD_ANSI_MAX_LIST && r->lists[top].ordered) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d. ", r->lists[top].counter);
                render_ansi(r, r->style->list_marker.on);
                RENDER_VERBATIM(r, buf);
                render_ansi(r, r->style->list_marker.off);
                r->lists[top].counter++;
            } else {
                render_ansi(r, r->style->list_marker.on);
                RENDER_VERBATIM(r, r->style->list_bullet);
                RENDER_VERBATIM(r, " ");
                render_ansi(r, r->style->list_marker.off);
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
            if(!(r->code_highlight && r->style->code_reverse && !r->card))
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
                        RENDER_VERBATIM(r, r->style->code_prefix);
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
        case MD_BLOCK_OL:
            if(r->list_sp > 0)
                r->list_sp--;
            r->li_opened = 0;
            /* Only a top-level list forces a blank line before the next block. */
            if(r->list_sp == 0)
                r->need_newline = 1;
            break;

        case MD_BLOCK_LI:
            r->list_depth--;
            /* End the item's own line; if it already ended (e.g. with a nested
             * list or a closing paragraph) don't add a spurious blank line. */
            if(r->line_dirty)
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
                done = emit_highlighted_code(r, 1);
#endif
                if(done == 0) {
                    /* Fall back to plain. In reverse mode the header was deferred
                     * to here, so draw it (plain) before the body. */
                    if(r->style->code_reverse && !r->card)
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
            if(done != 2) {
                /* Streaming: defer a top-level trailing code block's footer so a
                 * still-open fence does not flap its bottom rule on every push.
                 * The footer is flushed on the next enter_block (if another block
                 * follows) or silently dropped at end-of-input. */
                if((r->flags & MD_ANSI_FLAG_STREAM_OPEN_CODE)
                   && r->list_depth == 0 && r->quote_depth == 0)
                    r->code_footer_pending = 1;
                else
                    render_code_rule(r, NULL, 0);   /* footer (label-less) */
            }
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
                        RENDER_VERBATIM(r, r->style->code_prefix);
                        r->need_indent = 0;
                        r->code_col = 0;
                        /* Content sits inside the rule box: indent + 2-space code
                         * margin on the left, prose right margin on the right. */
                        r->code_clip = (r->wrap_cols > 0)
                            ? r->wrap_cols - ansi_indent_width(r)
                              - ansi_disp_width(r->style->code_prefix,
                                    (MD_SIZE)strlen(r->style->code_prefix))
                              - DOC_MARGIN : 0;
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
    render.real_output = process_output;
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
    /* Whole-document card: fill every line with the theme background from the
     * style (style->reverse.on), unless colour is disabled or no bg is set. */
    if((renderer_flags & MD_ANSI_FLAG_REVERSE) && !(renderer_flags & MD_ANSI_FLAG_NO_COLOR)
       && style->reverse.on != NULL && style->reverse.on[0] != '\0')
        render.card = 1;
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
        /* Emit any trailing card line the output did not terminate with '\n'. */
        if(render.card && render.card_size > 0)
            flush_card_line(&render);
        free(render.lbuf);
        free(render.code_buf);
        free(render.sgr_buf);
        free(render.card_buf);

        /* Free any table left dangling by an aborted parse. */
        if(render.table != NULL)
            table_free(render.table);

        if(owned_style != NULL)
            md_ansi_style_destroy(owned_style);
        return ret;
    }
}

/* Emit raw code one physical line at a time. Styled blocks use the same
 * two-column inset, clipping and code_block style as Markdown fences. */
static void
emit_raw_code(MD_ANSI* r, int styled)
{
    MD_SIZE start = 0, end;
    int avail = 0;
    int prefixw = ansi_disp_width(r->style->code_prefix,
                                  (MD_SIZE)strlen(r->style->code_prefix));

    if(styled) {
        avail = (r->wrap_cols > 0)
              ? r->wrap_cols - ansi_indent_width(r) - prefixw - DOC_MARGIN : 0;
        if(r->wrap_cols > 0 && avail < 1)
            avail = 1;
        render_ansi(r, r->style->code_block.on);
    } else if(r->wrap_cols > 0) {
        avail = r->wrap_cols;
    }

    while(start < r->code_size) {
        MD_SIZE len;
        for(end = start; end < r->code_size && r->code_buf[end] != '\n'; end++)
            ;
        len = end - start;
        if(styled) {
            render_indent(r);
            RENDER_VERBATIM(r, r->style->code_prefix);
        }
        if(avail > 0)
            len = ansi_clip_bytes(r->code_buf + start, len, avail);
        if(len > 0)
            render_verbatim(r, r->code_buf + start, len);
        render_newline(r);
        start = end < r->code_size ? end + 1 : end;
    }
    if(styled)
        render_ansi(r, r->style->code_block.off);
}

int
md_ansi_fenced_styled(const MD_CHAR* input, MD_SIZE input_size,
                       const char* language, fy_generic template_vars,
                       size_t lines, size_t plain_lines, size_t hidden_lines,
                       unsigned fence_flags,
                       void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                       void* userdata, unsigned renderer_flags, int width,
                       const struct MD_ANSI_STYLE* style)
{
    MD_ANSI render;
    MD_ANSI_STYLE* owned_style = NULL;
    char* filtered = NULL;
    const char* code = input;
    MD_SIZE code_size = input_size;
    int styled = (fence_flags & MD_ANSI_FENCE_STYLE) != 0;
    int done = 0, ret = 0;

    if(process_output == NULL || (input == NULL && input_size > 0))
        return -1;
    memset(&render, 0, sizeof(render));
    render.process_output = process_output;
    render.real_output = process_output;
    render.userdata = userdata;
    /* Highlighted output and raw code rows must bypass the prose line buffer.
     * Without this, the prefix capture leaves spaces buffered ahead of only
     * the first highlighted line and the highlighter's framed rows are wrapped
     * a second time. */
    render.in_code_block = 1;
    render.flags = renderer_flags & ~(unsigned)(MD_ANSI_FLAG_HEAL |
                                                 MD_ANSI_FLAG_CODE_META |
                                                 MD_ANSI_FLAG_STREAM_OPEN_CODE);
    render.table_width = width;
    if(style == NULL) {
        owned_style = md_ansi_style_create(NULL, 0, NULL);
        if(owned_style == NULL)
            return -1;
        style = owned_style;
    }
    render.style = style;
    render.template_vars = template_vars;
    render.template_lines = lines;
    render.template_plain_lines = plain_lines;
    render.template_hidden_lines = hidden_lines;
    if((renderer_flags & MD_ANSI_FLAG_REVERSE) &&
       !(renderer_flags & MD_ANSI_FLAG_NO_COLOR) &&
       style->reverse.on != NULL && style->reverse.on[0] != '\0')
        render.card = 1;
    if(width == MD_ANSI_WIDTH_INF)
        render.wrap_cols = 0;
    else if(width > 0)
        render.wrap_cols = width;
    else
        render.wrap_cols = table_term_width();

    /* Apply the same embedded-escape policy as Markdown code text. */
    if(!(render.flags & MD_ANSI_FLAG_SGR_KEEP) && input_size > 0 &&
       memchr(input, 0x1b, input_size) != NULL) {
        filtered = (char*) malloc(input_size);
        if(filtered == NULL) {
            ret = -1;
            goto out;
        }
        code_size = sgr_filter_input(render.flags, input, input_size, filtered);
        code = filtered;
    }
    if(code_size > 0) {
        render.code_buf = (char*) malloc(code_size);
        if(render.code_buf == NULL) {
            ret = -1;
            goto out;
        }
        memcpy(render.code_buf, code, code_size);
        render.code_size = code_size;
        render.code_cap = code_size;
    }
    if(language != NULL && language[0] != '\0') {
        size_t n = strlen(language);
        if(n >= sizeof(render.code_lang))
            n = sizeof(render.code_lang) - 1;
        memcpy(render.code_lang, language, n);
        render.code_lang[n] = '\0';
        render.code_lang_size = (MD_SIZE) n;
    }
#ifdef MD4C_WITH_FYTS
    if((fence_flags & MD_ANSI_FENCE_HIGHLIGHT) && style->code_enabled &&
       render.code_lang_size > 0 && fyts_language_supported(render.code_lang))
        render.code_highlight = 1;
#endif

    if(styled && !(render.code_highlight && style->code_reverse && !render.card))
        render_code_rule(&render, render.code_lang, render.code_lang_size);
    if(render.code_highlight) {
#ifdef MD4C_WITH_FYTS
        done = emit_highlighted_code(&render, styled);
#endif
        if(done == 0) {
            if(styled && style->code_reverse && !render.card)
                render_code_rule(&render, render.code_lang, render.code_lang_size);
            emit_raw_code(&render, styled);
        }
    } else {
        emit_raw_code(&render, styled);
    }
    if(styled && done != 2) {
        if(render.line_dirty)
            render_newline(&render);
        render_code_rule(&render, NULL, 0);
    }

out:
    if(render.line_open)
        flush_wrapped(&render);
    if(render.card && render.card_size > 0)
        flush_card_line(&render);
    free(filtered);
    free(render.lbuf);
    free(render.code_buf);
    free(render.sgr_buf);
    free(render.card_buf);
    if(owned_style != NULL)
        md_ansi_style_destroy(owned_style);
    return ret;
}
