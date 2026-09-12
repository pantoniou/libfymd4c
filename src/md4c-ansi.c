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
#include <libfymd4c/libfymd4c-width.h>
#include "md4c-ansi.h"
#include "md4c-heal-wrap.h"
#include "md4c-style.h"
#include "md4c-ui.h"
#include "entity.h"

#ifdef MD4C_WITH_FYPALETTE
    #include <libfypalette.h>
#endif

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

/* Document margin reserved on each side of every line (like glow). The style
 * sets it; a palette takes it from its gutter. */
#define DOC_MARGIN          ((r)->ui_col_depth ? 0 : (r)->style->doc_margin)


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
/* An open fy-columns block: the widths and the output of each column. */
#define MD_UI_COLS_MAX 8
typedef struct MD_ANSI_COLUMNS {
    int n;
    int cur;
    int gap;
    int capturing;
    int width[MD_UI_COLS_MAX];
    char* buf[MD_UI_COLS_MAX];
    MD_SIZE size[MD_UI_COLS_MAX];
    MD_SIZE cap[MD_UI_COLS_MAX];
    /* the render state a capturing column replaces */
    void (*saved_out)(const MD_CHAR*, MD_SIZE, void*);
    void* saved_ud;
    int saved_wrap;
    int saved_table_width;
    int saved_quote;
    int saved_list;
    int saved_need_newline;
    size_t saved_row;
    int saved_row_open;
    MD_ANSI_MARGIN_FN saved_margin;
} MD_ANSI_COLUMNS;

#define MD_UI_ROLE_MAX 8

struct MD_ANSI_tag {
    void (*process_output)(const MD_CHAR*, MD_SIZE, void*);
    void* userdata;
    unsigned flags;
    int image_nesting_level;
    int quote_depth;
    int list_depth;
    unsigned heading_level; /* level of the open heading, 1-6; 0 outside one */
    int in_code_block;
    int code_footer_pending; /* streaming: trailing code-block footer deferred */
    int need_newline;       /* pending newline before next block */
    int need_indent;        /* emit indent prefix on next code text */
    int code_col;           /* display column within the current code line (clip) */
    int code_clip;          /* max code-content columns per line; 0 = no clip */
    int li_opened;          /* just opened a list item (bullet already printed) */
    int line_dirty;         /* content emitted on the current line, no newline yet */
    MD_ANSI_MARGIN_FN margin_fn;
    void* margin_userdata;
    size_t output_row;      /* newlines emitted so far == index of current row */
    int row_open;           /* bytes emitted on the current (unterminated) row */

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
    int code_fyts;          /* libfyts has a grammar for the current block */
    const MD_BLOCK_RENDERER* code_custom; /* renderer of the current block, or NULL */
    int code_on_pending;    /* code_block.on deferred until the first body
                               byte, so an EMPTY body emits no stray on/off
                               pair (it breaks streamed-vs-one-shot byte
                               identity when a continuation render starts at
                               the closing fence) */
    char code_lang[64];     /* info string (language) of the current code block */
    MD_SIZE code_lang_size;
    int code_diff;          /* current block is a diff/patch rendered GitHub-style */
    char code_diff_lang[64];/* explicit inner language from the info string ("") */
    char* code_buf;         /* buffered raw code text */
    MD_SIZE code_size, code_cap;
    struct fyts_ctx** fyts_ctx;

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

    /* UI Markdown (MD_ANSI_FLAG_UI) */
    MD_ANSI_UI* ui;                 /* region sink, or NULL */
    int ui_col;                     /* display column of the current output row */
    int ui_act;                     /* inside an fy-act */
    int ui_act_seg;                 /* column the open region starts at, or -1 */
    char ui_act_id[64];
    size_t ui_act_len;
    const char* ui_role_off[MD_UI_ROLE_MAX];
    int ui_role_depth;
    int in_html_block;
    char* html_buf;                 /* the text of the open HTML block */
    MD_SIZE html_size, html_cap;
    int ui_col_depth;               /* inside a capturing fy-col */
    MD_ANSI_COLUMNS* ui_cols;       /* the open fy-columns, or NULL */
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
static MD_SIZE ansi_esc_len(const char* s, MD_SIZE n);
static MD_SIZE ansi_utf8_decode(const char* s, MD_SIZE n, unsigned* cp);

/* The pair of the open heading: its level's pair when the style has one. */
static const MD_STYLE_PAIR*
heading_pair(const MD_ANSI* r)
{
    if(r->heading_level >= 1 && r->heading_level <= 6 &&
       r->style->heading_level[r->heading_level - 1].on != NULL)
        return &r->style->heading_level[r->heading_level - 1];
    return &r->style->heading;
}
static TLINE* wrap_text(const char* buf, MD_SIZE size, int width, int* n_out);
static void render_indent(MD_ANSI* r);

/* Running SGR (Select Graphic Rendition) state, so wrap continuation lines can
 * close an active style before the newline and re-apply it afterwards (else an
 * open underline/reverse/background bleeds across the break and the indent). */
typedef struct {
    int bold, faint, italic, underline, blink, reverse, conceal, strike, overline;
    char fg[24];   /* SGR params for the foreground, e.g. "31" or "38;5;12"; "" = default */
    char bg[24];   /* SGR params for the background; "" = default */
    char ul[24];   /* SGR params for the underline colour, e.g. "58;5;12"; "" = default */
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
out_sink_raw(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    MD_SIZE i;

    /* Every byte of the rendered document funnels through here (prose, code
     * blocks, tables, cards), so this is the one place row counting is exact.
     * It runs even without a sink, so measuring can render into /dev/null. */
    for(i = 0; i < size; i++) {
        if(text[i] == '\n')
            r->output_row++;
    }
    if(size > 0)
        r->row_open = (text[size - 1] != '\n');

    if (!r->process_output)
        return;

    if(r->card && r->process_output == r->real_output)
        card_feed(r, text, size);
    else
        r->process_output(text, size, r->userdata);
}

/* Track the output column of @text and the regions of an open fy-act. */
static void
ui_track(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    size_t row = r->output_row;
    MD_SIZE i = 0, e, cl;
    unsigned cp;

    while(i < size) {
        if(text[i] == '\n') {
            if(r->ui_act && r->ui_act_seg >= 0 && r->ui_col > r->ui_act_seg)
                (void) md_ui_region_add(r->ui, r->ui_act_id, r->ui_act_len, row,
                                        r->ui_act_seg, r->ui_col - r->ui_act_seg,
                                        1, MD_UI_REGION_ACT);
            if(r->ui_act)
                r->ui_act_seg = -1;     /* a wrapped label reopens on the next row */
            r->ui_col = 0;
            row++;
            i++;
            continue;
        }
        e = ansi_esc_len(text + i, size - i);
        if(e > 0) {
            i += e;
            continue;
        }
        cl = ansi_utf8_decode(text + i, size - i, &cp);
        if(r->ui_act && r->ui_act_seg < 0 && cp != ' ')
            r->ui_act_seg = r->ui_col;
        r->ui_col += fymd_cp_width(cp);
        i += cl ? cl : 1;
    }
}

/*
 * Every byte of the document reaches the real sink through here. In a UI
 * render the layout markers end here: a fill that no layout resolved becomes a
 * space, an fy-act records its region, and a row marker stays only for the
 * vertical pass. Output that a column or an indent captures keeps its markers
 * until it reaches the real sink.
 */
static void
out_sink(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    const char* kind;
    const char* arg;
    size_t kl, al;
    MD_SIZE i, start, m;

    if(!(r->flags & MD_ANSI_FLAG_UI) || size == 0 ||
       (r->process_output != NULL && r->process_output != r->real_output)) {
        out_sink_raw(r, text, size);
        return;
    }
    for(i = 0, start = 0; i < size; ) {
        if((unsigned char) text[i] != 0x1b ||
           (m = (MD_SIZE) md_ui_marker(text + i, size - i, &kind, &kl, &arg, &al)) == 0) {
            i++;
            continue;
        }
        if(i > start) {
            ui_track(r, text + start, i - start);
            out_sink_raw(r, text + start, i - start);
        }
        if(md_ui_marker_is(kind, kl, "fill")) {
            /* a fill that no layout resolved is one cell */
            const char* g = (arg != NULL && al > 0) ? arg : " ";
            MD_SIZE gl = (arg != NULL && al > 0) ? (MD_SIZE) al : 1;
            ui_track(r, g, gl);
            out_sink_raw(r, g, gl);
        } else if(md_ui_marker_is(kind, kl, "act")) {
            r->ui_act = 1;
            r->ui_act_len = al < sizeof(r->ui_act_id) ? al : sizeof(r->ui_act_id) - 1;
            memcpy(r->ui_act_id, arg, r->ui_act_len);
            r->ui_act_seg = -1;
        } else if(md_ui_marker_is(kind, kl, "/act")) {
            if(r->ui_act && r->ui_act_seg >= 0 && r->ui_col > r->ui_act_seg)
                (void) md_ui_region_add(r->ui, r->ui_act_id, r->ui_act_len,
                                        r->output_row, r->ui_act_seg,
                                        r->ui_col - r->ui_act_seg,
                                        1, MD_UI_REGION_ACT);
            r->ui_act = 0;
        } else if(md_ui_marker_is(kind, kl, "keep")) {
            /* holds a blank row of a slot through a column; ends here */
        } else if(md_ui_marker_is(kind, kl, "slot")) {
            /* the cells of a slot start here, at the column of the output */
            size_t idl;
            int sw, sh;
            if(md_ui_slot_arg(arg, al, &idl, &sw, &sh) == 0)
                (void) md_ui_region_add(r->ui, arg, idl, r->output_row,
                                        r->ui_col, sw, sh, MD_UI_REGION_SLOT);
        } else if(r->flags & MD_ANSI_FLAG_UI_ROWS) {
            out_sink_raw(r, text + i, m);   /* a row marker, zero width */
        }
        i += m;
        start = i;
    }
    if(start < size) {
        ui_track(r, text + start, size - start);
        out_sink_raw(r, text + start, size - start);
    }
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
                /* i + e <= size holds by construction; stated so the compiler
                 * can see the accesses below are in bounds. */
                if(e >= 3 && i + e <= size &&
                   (unsigned char)text[i] == 0x1b && text[i + 1] == '[' &&
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
/* The quote bar: a card has a bar of its own when the style gives one. */
static const char*
quote_bar(const MD_ANSI* r)
{
    if(r->card && r->style->card_bar != NULL)
        return r->style->card_bar;
    return r->style->blockquote_bar;
}

static void
render_document_margin(MD_ANSI* r)
{
    const char* margin = r->margin_fn
                       ? r->margin_fn(r->margin_userdata, r->output_row) : NULL;
    int i;
    if(margin != NULL)
        RENDER_VERBATIM(r, margin);
    else
        for(i = 0; i < DOC_MARGIN; i++)
            RENDER_VERBATIM(r, " ");
}

static void
render_indent_chrome(MD_ANSI* r)
{
    int i;
    render_document_margin(r);
    for(i = 0; i < r->quote_depth; i++) {
        render_ansi(r, r->style->blockquote.on);
        RENDER_VERBATIM(r, quote_bar(r));
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

/* The number of fill markers in @buf. */
static int
ui_fill_count(const char* buf, MD_SIZE size)
{
    const char* kind;
    const char* arg;
    size_t kl, al, m;
    MD_SIZE i;
    int n = 0;

    for(i = 0; i < size; i++) {
        if((unsigned char) buf[i] != 0x1b)
            continue;
        m = md_ui_marker(buf + i, size - i, &kind, &kl, &arg, &al);
        if(m != 0 && md_ui_marker_is(kind, kl, "fill")) {
            n++;
            i += m - 1;
        }
    }
    return n;
}

/*
 * Emit @buf with its fills replaced by @extra columns of blanks, shared out
 * from the first fill. @direct selects out_direct() over render_verbatim().
 */
static void render_verbatim(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size);

static void
ui_emit_filled(MD_ANSI* r, const char* buf, MD_SIZE size, int extra, int direct)
{
    const char* kind;
    const char* arg;
    size_t kl, al, m;
    MD_SIZE i, start;
    int n, k = 0, pad, j;

    n = ui_fill_count(buf, size);
    if(extra < 0)
        extra = 0;
    for(i = 0, start = 0; i < size; i++) {
        if((unsigned char) buf[i] != 0x1b)
            continue;
        m = md_ui_marker(buf + i, size - i, &kind, &kl, &arg, &al);
        if(m == 0 || !md_ui_marker_is(kind, kl, "fill"))
            continue;
        if(i > start) {
            if(direct) out_direct(r, buf + start, i - start);
            else render_verbatim(r, buf + start, i - start);
        }
        pad = extra / n + (k < extra % n ? 1 : 0);
        k++;
        {
            /* the glyph of the fill, a blank without one */
            const char* g = (arg != NULL && al > 0) ? arg : " ";
            MD_SIZE gl = (arg != NULL && al > 0) ? (MD_SIZE) al : 1;
            for(j = 0; j < pad; j++) {
                if(direct) out_direct(r, g, gl);
                else render_verbatim(r, g, gl);
            }
        }
        i += m - 1;
        start = i + 1;
    }
    if(start < size) {
        if(direct) out_direct(r, buf + start, size - start);
        else render_verbatim(r, buf + start, size - start);
    }
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
            render_indent(r);
            if(alen > 0)
                out_direct(r, active, alen);        /* re-apply the open style */
        }
        if(lines[k].len > 0) {
            if((r->flags & MD_ANSI_FLAG_UI) &&
               ui_fill_count(r->lbuf + lines[k].start, lines[k].len) > 0)
                ui_emit_filled(r, r->lbuf + lines[k].start, lines[k].len,
                               avail - lines[k].w, 1);
            else
                out_direct(r, r->lbuf + lines[k].start, lines[k].len);
        }
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
    else {
        out_direct(r, "\n", 1);
    }
}

/* Render a blank separator line. Inside a blockquote the quote bar(s) are kept
 * so the empty line still reads as part of the quote (like glow / GitHub). */
static void
render_separator(MD_ANSI* r)
{
    if(r->quote_depth > 0) {
        int i, saved = r->wrap_suspend;
        r->wrap_suspend = 1;             /* emit the bars straight to output */
        render_document_margin(r);
        for(i = 0; i < r->quote_depth; i++) {
            render_ansi(r, r->style->blockquote.on);
            RENDER_VERBATIM(r, quote_bar(r));
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

/* The width tables live in md4c-width.c, behind fymd_cp_width(), so that the
 * ANSI renderer and the public measurement share one copy of Unicode. */

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
    if(n >= 2 && s[1] == '_') {            /* APC: ESC _ ... ESC \ (layout markers) */
        i = 2;
        while(i < n) {
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
            case 59: s->ul[0] = '\0'; break;
            default:
                if((v >= 30 && v <= 37) || (v >= 90 && v <= 97))
                    snprintf(s->fg, sizeof(s->fg), "%d", v);
                else if((v >= 40 && v <= 47) || (v >= 100 && v <= 107))
                    snprintf(s->bg, sizeof(s->bg), "%d", v);
                else if(v == 38 || v == 48 || v == 58) {
                    char* dst = (v == 38) ? s->fg : (v == 48) ? s->bg : s->ul;
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
            int vals[32], count = 0, cur = 0, have = 0, sub = 0, subval = 0;
            MD_SIZE p;
            /* A ':' sub-parameter (ISO 8613-6) refines its parameter: 4:3 is
             * a curly underline and stays an underline here. Only 4:0 turns
             * the underline off. */
            for(p = i + 2; p < i + e - 1; p++) {
                char c = buf[p];
                if(c >= '0' && c <= '9') {
                    if(sub) subval = subval * 10 + (c - '0');
                    else cur = cur * 10 + (c - '0');
                    have = 1;
                }
                else if(c == ':') { sub = 1; subval = 0; }
                else if(c == ';') {
                    if(sub && cur == 4 && subval == 0) cur = 24;
                    if(count < (int)(sizeof(vals)/sizeof(vals[0]))) vals[count++] = cur;
                    cur = 0; have = 1; sub = 0;
                }
            }
            if(sub && cur == 4 && subval == 0) cur = 24;
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
    char params[128];
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
    if(s->ul[0])     SGR_PUT(s->ul);
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
        w += fymd_cp_width(cp);
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
        cw = fymd_cp_width(cp);

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

    if((r->flags & MD_ANSI_FLAG_UI) && ln.len > 0 &&
       ui_fill_count(buf + ln.start, ln.len) > 0) {
        /* a fill takes the padding of its cell */
        if(is_header) render_ansi(r, r->style->table_header.on);
        ui_emit_filled(r, buf + ln.start, ln.len, pad, 0);
        if(is_header) render_ansi(r, r->style->table_header.off);
        return;
    }
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
        int cw = fymd_cp_width(cp);
        if(w + cw > width)
            break;
        w += cw;
        i += cl;
    }
    return i;
}

/* Is the first-row marker in play? The styling may carry a marker while leaving
 * it switched off, so both the flag and a non-empty string are required. */
static int
code_marker_active(MD_ANSI* r)
{
    return r->style->code_marker_enabled &&
           r->style->code_marker != NULL &&
           r->style->code_marker[0] != '\0';
}

/* Width reserved before fenced content: the marker and the continuation prefix
 * are padded to a common column, so both are accounted for. */
static int
code_prefix_width(MD_ANSI* r)
{
    int pw = ansi_disp_width(r->style->code_prefix,
                             (MD_SIZE) strlen(r->style->code_prefix));
    int mw;
    if(!code_marker_active(r))
        return pw;
    mw = ansi_disp_width(r->style->code_marker,
                         (MD_SIZE) strlen(r->style->code_marker));
    return mw > pw ? mw : pw;
}

/* Prefix of one fenced-content row. Without code.decoration.marker every row
 * gets code_prefix, as before. With a marker the FIRST row carries it and the
 * rest are code_prefix padded out to the marker's width, so the block reads as
 * one hanging-indent item:
 *
 *     |- int main(void)
 *        {
 *            return 0;
 *        }
 *
 * `pad` is caller-provided scratch for the padded continuation prefix. */
static const char*
code_row_prefix(MD_ANSI* r, int first, char* pad, size_t pad_size)
{
    const char* marker = r->style->code_marker;
    const char* prefix = r->style->code_prefix;
    size_t n;
    int mw, pw, i;

    if(!code_marker_active(r))
        return prefix;
    if(first)
        return marker;
    mw = ansi_disp_width(marker, (MD_SIZE) strlen(marker));
    pw = ansi_disp_width(prefix, (MD_SIZE) strlen(prefix));
    if(pw >= mw)
        return prefix;
    n = strlen(prefix);
    if(n + (size_t)(mw - pw) >= pad_size)
        return prefix;
    memcpy(pad, prefix, n);
    for(i = 0; i < mw - pw; i++)
        pad[n + i] = ' ';
    pad[n + (mw - pw)] = '\0';
    return pad;
}

/* Emit the buffered code block as plain dim text (the pre-highlighting path,
 * also the fallback when highlighting is unavailable or fails). Long lines are
 * clipped to the prose right margin (like glow); wrap_cols == 0 = no clip. */
static void
emit_plain_code(MD_ANSI* r)
{
    MD_SIZE i, start = 0;
    int first = 1;
    char pad[64];
    int prefixw = code_prefix_width(r);
    int avail = (r->wrap_cols > 0)
                ? r->wrap_cols - ansi_indent_width(r) - prefixw - DOC_MARGIN : 0;
    if(r->wrap_cols > 0 && avail < 1) avail = 1;
    /* An empty body must emit nothing at all: a bare on/off style pair is
     * zero-width, but it breaks the streamed-equals-one-shot byte identity
     * when a continuation render starts directly at the closing fence. */
    if(r->code_size == 0)
        return;
    render_ansi(r, r->style->code_block.on);
    for(i = 0; i <= r->code_size; i++) {
        if(i == r->code_size || r->code_buf[i] == '\n') {
            if(i > start) {
                MD_SIZE len = i - start;
                render_indent(r);
                RENDER_VERBATIM(r, code_row_prefix(r, first, pad, sizeof(pad)));
                first = 0;
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
    /* With a first-row marker the per-line prefix varies, which fyts's constant
     * line_prefix cannot express: take its output as a buffer and lay the rows
     * out here instead. The bubble (reverse) mode frames its own background and
     * keeps the constant-prefix path. */
    int marker_mode = styled && !reverse && code_marker_active(r);
    /* Clip width for fyts: 0 (no wrap / MD_ANSI_WIDTH_INF) means no clipping,
     * matching prose. fyts subtracts the line_prefix (indent + 2-space margin)
     * itself, so reserving DOC_MARGIN here lands the content inside the rule box.
     * The header/footer rules below still need a finite width, so they fall back
     * to the terminal width separately. */
    int clip_width = (r->wrap_cols > 0)
                   ? r->wrap_cols - (styled ? DOC_MARGIN : 0) : 0;

    if(marker_mode && r->wrap_cols > 0) {
        clip_width = r->wrap_cols - ansi_indent_width(r)
                   - code_prefix_width(r) - DOC_MARGIN;
        if(clip_width < 1)
            clip_width = 1;
    }

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
    if(styled && !marker_mode) {
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

    /* A palette is given to a context, so a palette render needs one. */
    if(marker_mode || r->fyts_ctx != NULL || r->style->palette != NULL) {
        char* output = NULL;
        size_t output_len = 0;
        struct fyts_ctx* own = NULL;
        /* Without a retained context (one-shot renders) a throwaway one gives
         * the same heap-buffer output. */
        struct fyts_ctx** ctxp = (r->fyts_ctx != NULL) ? r->fyts_ctx : &own;

        if(*ctxp != NULL && fyts_ctx_configure(*ctxp, &cfg) != 0) {
            fyts_ctx_destroy(*ctxp);
            *ctxp = NULL;
        }
        if(*ctxp == NULL)
            *ctxp = fyts_ctx_create(&cfg);
#ifdef MD4C_FYTS_PALETTE
        /* A highlighter that cannot take the palette keeps its styling. */
        if(*ctxp != NULL)
            (void) fyts_ctx_set_palette(*ctxp, r->style->palette);
#endif
        if(*ctxp == NULL ||
           fyts_ctx_highlight_source(*ctxp, r->code_buf, r->code_size,
                                     &output, &output_len) != 0) {
            free(output);
            if(own != NULL)
                fyts_ctx_destroy(own);
            return 0;
        }
        if(marker_mode) {
            /* Lay out the highlighted rows: marker on the first, the padded
             * continuation prefix on the rest. */
            size_t ls = 0, i2;
            int first = 1;
            char pad[64];
            for(i2 = 0; i2 <= output_len; i2++) {
                if(i2 < output_len && output[i2] != '\n')
                    continue;
                if(i2 == output_len && i2 == ls)
                    break;                       /* trailing newline, no row */
                render_indent(r);
                RENDER_VERBATIM(r, code_row_prefix(r, first, pad, sizeof(pad)));
                first = 0;
                if(i2 > ls)
                    render_verbatim(r, output + ls, (MD_SIZE)(i2 - ls));
                render_newline(r);
                ls = i2 + 1;
            }
        } else {
            render_verbatim(r, output, (MD_SIZE) output_len);
        }
        free(output);
        if(own != NULL)
            fyts_ctx_destroy(own);
        rc = 0;
    } else {
        rc = fyts_highlight_source(&cfg, r->code_buf, r->code_size);
    }
    if(rc < 0)
        return 0;            /* 0 => caller emits the plain fallback */
    return reverse ? 2 : 1;  /* reverse: header+code+footer all done by fyts */
}
#endif /* MD4C_WITH_FYTS */

/*********************************************
 ***  Diff / patch blocks (GitHub-like)  ***
 *********************************************/

/* Row kinds of a unified diff. */
#define DIFF_CTX   0    /* unchanged line inside a hunk (leading space) */
#define DIFF_ADD   1    /* '+' line */
#define DIFF_DEL   2    /* '-' line */
#define DIFF_HUNK  3    /* "@@ -a,b +c,d @@" */
#define DIFF_FILE  4    /* "diff --git", "index", "--- a/x", "+++ b/x", "\ No newline" */
#define DIFF_META  5    /* anything outside a hunk: commit/author/date, the
                           commit message, "--", trailers -- carries no marker
                           column, so it is emitted verbatim */

typedef struct {
    MD_SIZE start, len;  /* the raw line inside r->code_buf */
    int kind;
    long lineno;         /* new-side line number, 0 = no number for this row */
    const char* hl;      /* highlighted payload (marker stripped), or NULL */
    MD_SIZE hl_len;
} DIFF_LINE;

/* Does the fence info string select the diff renderer? Accepted forms are
 * "diff" / "patch", optionally naming the patched file's language as
 * "diff c", "diff:c" or "patch=c". Returns 1 and fills `inner` (possibly with
 * an empty string) when it does. */
static int
ansi_diff_info(const char* info, MD_SIZE size, char* inner, size_t inner_size)
{
    MD_SIZE i = 0, s;

    inner[0] = '\0';
    if(info == NULL)
        return 0;
    while(i < size && (info[i] == ' ' || info[i] == '\t'))
        i++;
    s = i;
    while(i < size && info[i] != ' ' && info[i] != '\t' &&
          info[i] != ':' && info[i] != '=')
        i++;
    if(!((i - s == 4 && memcmp(info + s, "diff", 4) == 0) ||
         (i - s == 5 && memcmp(info + s, "patch", 5) == 0)))
        return 0;

    /* Optional inner language. */
    while(i < size && (info[i] == ' ' || info[i] == '\t' ||
                       info[i] == ':' || info[i] == '='))
        i++;
    s = i;
    while(i < size && info[i] != ' ' && info[i] != '\t')
        i++;
    if(i > s && (size_t) (i - s) < inner_size) {
        memcpy(inner, info + s, i - s);
        inner[i - s] = '\0';
    }
    return 1;
}

/* Classify one raw diff line. `st` carries the scan state: whether we are
 * inside a hunk body, and whether any +/- row has been seen yet.
 *
 * Only inside a hunk (or, for an informal snippet with no "@@" header, once a
 * +/- row has appeared) does the first column carry a +/-/space marker. The
 * preamble of a "git show" / "git format-patch" -- commit, Author:, Date:, the
 * indented commit message, mail trailers -- carries no marker, and stripping
 * one there would eat the first character of the text ("commit" -> "ommit"). */
typedef struct { int in_hunk; int seen_change; } DIFF_SCAN;

static int
diff_classify(const char* p, MD_SIZE len, DIFF_SCAN* st)
{
    if(len >= 2 && p[0] == '@' && p[1] == '@') {
        st->in_hunk = 1;
        st->seen_change = 1;
        return DIFF_HUNK;
    }
    if((len >= 3 && (memcmp(p, "+++", 3) == 0 || memcmp(p, "---", 3) == 0)) ||
       (len >= 4 && memcmp(p, "diff", 4) == 0) ||
       (len >= 5 && memcmp(p, "index", 5) == 0) ||
       (len >= 1 && p[0] == '\\')) {
        st->in_hunk = 0;
        return DIFF_FILE;
    }
    if(len > 0 && p[0] == '+') {
        st->seen_change = 1;
        return DIFF_ADD;
    }
    if(len > 0 && p[0] == '-') {
        st->seen_change = 1;
        return DIFF_DEL;
    }
    /* A space-led line is a context row only once the block has shown itself
     * to be diff body; before that it is preamble text (e.g. the indented
     * commit message), which must not lose its first column. */
    if(st->in_hunk || st->seen_change) {
        if(len == 0 || p[0] == ' ')
            return DIFF_CTX;
        st->in_hunk = 0;      /* no marker inside a hunk: the hunk ended */
    }
    return DIFF_META;
}

/* New-side start line of a hunk header: "@@ -a,b +c,d @@" -> c (0 if absent). */
static long
diff_hunk_newline_start(const char* p, MD_SIZE len)
{
    MD_SIZE i;
    for(i = 0; i + 1 < len; i++) {
        if(p[i] == '+' && p[i+1] >= '0' && p[i+1] <= '9') {
            long v = 0;
            for(i++; i < len && p[i] >= '0' && p[i] <= '9'; i++)
                v = v * 10 + (p[i] - '0');
            return v;
        }
    }
    return 0;
}

/* Extract the patched file's path from a "+++ b/path" / "--- a/path" header
 * into `out` (NUL-terminated). Returns 1 on success. The a//b/ prefix and any
 * trailing tab-separated metadata (timestamp) are stripped. */
static int
diff_header_path(const char* p, MD_SIZE len, char* out, size_t out_size)
{
    MD_SIZE i = 3, end;
    while(i < len && (p[i] == ' ' || p[i] == '\t'))
        i++;
    if(i >= len)
        return 0;
    end = i;
    while(end < len && p[end] != '\t')
        end++;
    while(end > i && p[end-1] == ' ')
        end--;
    if(end - i >= 2 && (p[i] == 'a' || p[i] == 'b') && p[i+1] == '/')
        i += 2;
    if(end <= i || (end - i) >= (MD_SIZE) out_size)
        return 0;
    if(end - i == 7 && memcmp(p + i, "dev/null", 7) == 0)
        return 0;
    memcpy(out, p + i, end - i);
    out[end - i] = '\0';
    return 1;
}

/* Emit `size` bytes of (possibly highlighted) payload, re-applying `bg` after
 * any escape that would clear the row background (SGR 0 / 49). */
static void
render_diff_payload(MD_ANSI* r, const char* buf, MD_SIZE size, const char* bg)
{
    MD_SIZE i = 0, start = 0;

    if(bg == NULL || bg[0] == '\0' || (r->flags & MD_ANSI_FLAG_NO_COLOR)) {
        render_verbatim(r, buf, size);
        return;
    }
    while(i < size) {
        MD_SIZE e = ansi_esc_len(buf + i, size - i);
        if(e == 0) { i++; continue; }
        /* Only SGR sequences can touch the background; a reset (0/empty) or an
         * explicit default-background (49) drops the band, so restore it. */
        if(buf[i + e - 1] == 'm') {
            const char* body = buf + i + 2;          /* after ESC '[' */
            MD_SIZE blen = e - 3;
            int clears = (blen == 0) ||
                         (blen == 1 && body[0] == '0') ||
                         (blen >= 2 && body[0] == '4' && body[1] == '9' &&
                          (blen == 2 || body[2] == ';'));
            if(!clears && blen >= 2) {
                MD_SIZE k;
                for(k = 0; k + 1 < blen; k++) {
                    if(body[k] == ';' &&
                       ((body[k+1] == '0' && (k + 2 == blen || body[k+2] == ';')) ||
                        (body[k+1] == '4' && k + 2 < blen && body[k+2] == '9')))
                        { clears = 1; break; }
                }
            }
            if(clears) {
                render_verbatim(r, buf + start, i + e - start);
                RENDER_VERBATIM(r, bg);
                start = i + e;
            }
        }
        i += e;
    }
    if(size > start)
        render_verbatim(r, buf + start, size - start);
}

#ifdef MD4C_WITH_FYTS
/* Highlight the payload rows of one file segment of a diff -- lines[from:to),
 * which all belong to the same patched file -- as `lang`, and point each row's
 * `hl` slice into the returned buffer (which the caller frees).
 *
 * The rows are highlighted in a single pass over their concatenated,
 * marker-stripped text so the grammar sees whole constructs rather than
 * isolated lines. Returns NULL when the segment cannot be highlighted, leaving
 * every `hl` NULL so the rows render as plain text. */
static char*
diff_highlight_segment(MD_ANSI* r, DIFF_LINE* lines, MD_SIZE from, MD_SIZE to,
                       const char* lang, int avail)
{
    struct fyts_config cfg;
    char* payload;
    char* hl = NULL;
    size_t hl_size = 0;
    MD_SIZE i, payload_size = 0, want = 0, j, seen = 0;

    if(lang == NULL || *lang == '\0' || !fyts_language_supported(lang))
        return NULL;

    for(i = from; i < to; i++)
        if(lines[i].kind <= DIFF_DEL)
            want++;
    if(want == 0)
        return NULL;

    payload = (char*) malloc(r->code_size + want + 1);
    if(payload == NULL)
        return NULL;
    for(i = from; i < to; i++) {
        if(lines[i].kind > DIFF_DEL)
            continue;
        if(lines[i].len > 1) {
            memcpy(payload + payload_size,
                   r->code_buf + lines[i].start + 1, lines[i].len - 1);
            payload_size += lines[i].len - 1;
        }
        payload[payload_size++] = '\n';
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.lang = lang;
    cfg.color_mode = FYTS_COLOR_ON;
    switch(r->style->code_background) {
        case MD_STYLE_BG_DARK:  cfg.background_mode = FYTS_BACKGROUND_DARK;  break;
        case MD_STYLE_BG_LIGHT: cfg.background_mode = FYTS_BACKGROUND_LIGHT; break;
        default:                cfg.background_mode = FYTS_BACKGROUND_AUTO;  break;
    }
    if(r->style->code_theme != NULL && r->style->code_theme[0] != '\0') {
        if(strchr(r->style->code_theme, '/') != NULL)
            cfg.styling_path = r->style->code_theme;
        else
            cfg.styling_name = r->style->code_theme;
    }
    cfg.width = avail;   /* fyts clips each line to the content width */

    /* The retained context is reused when the renderer has one (it is
     * reconfigured per segment anyway); otherwise a throwaway context gives the
     * same heap-buffer output without an output sink. */
    if(r->fyts_ctx != NULL) {
        if(*r->fyts_ctx != NULL && fyts_ctx_configure(*r->fyts_ctx, &cfg) != 0) {
            fyts_ctx_destroy(*r->fyts_ctx);
            *r->fyts_ctx = NULL;
        }
        if(*r->fyts_ctx == NULL)
            *r->fyts_ctx = fyts_ctx_create(&cfg);
#ifdef MD4C_FYTS_PALETTE
        if(*r->fyts_ctx != NULL)
            (void) fyts_ctx_set_palette(*r->fyts_ctx, r->style->palette);
#endif
        if(*r->fyts_ctx == NULL ||
           fyts_ctx_highlight_source(*r->fyts_ctx, payload, payload_size,
                                     &hl, &hl_size) != 0) {
            free(hl);
            hl = NULL;
        }
    } else {
        struct fyts_ctx* ctx = fyts_ctx_create(&cfg);
        if(ctx != NULL) {
#ifdef MD4C_FYTS_PALETTE
            (void) fyts_ctx_set_palette(ctx, r->style->palette);
#endif
            if(fyts_ctx_highlight_source(ctx, payload, payload_size,
                                         &hl, &hl_size) != 0) {
                free(hl);
                hl = NULL;
            }
            fyts_ctx_destroy(ctx);
        }
    }
    free(payload);
    if(hl == NULL)
        return NULL;

    /* Hand each row its own line of the highlighted text. A line-count
     * mismatch means the mapping is untrustworthy, so drop it entirely. */
    j = 0;
    for(i = from; i < to && j <= (MD_SIZE) hl_size; i++) {
        MD_SIZE end = j;
        if(lines[i].kind > DIFF_DEL)
            continue;
        while(end < (MD_SIZE) hl_size && hl[end] != '\n')
            end++;
        lines[i].hl = hl + j;
        lines[i].hl_len = end - j;
        seen++;
        j = end + 1;
    }
    if(seen != want || j < (MD_SIZE) hl_size) {
        for(i = from; i < to; i++) {
            lines[i].hl = NULL;
            lines[i].hl_len = 0;
        }
        free(hl);
        return NULL;
    }
    return hl;
}

/* Owned highlight buffers of a block (one per file segment). */
typedef struct { char** v; MD_SIZE n, cap; } DIFF_BUFS;

static void
diff_bufs_free(DIFF_BUFS* b)
{
    MD_SIZE i;
    for(i = 0; i < b->n; i++)
        free(b->v[i]);
    free(b->v);
    b->v = NULL;
    b->n = b->cap = 0;
}

/* Highlight one file segment and keep its buffer alive until the block is
 * emitted. Any failure (unknown language, allocation) simply leaves the rows
 * unhighlighted. */
static void
diff_flush_segment(MD_ANSI* r, DIFF_LINE* lines, MD_SIZE from, MD_SIZE to,
                   const char* path, int avail, DIFF_BUFS* bufs)
{
    const char* lang;
    char* detected = NULL;
    char* hl;

    if(r->code_diff_lang[0] != '\0') {
        lang = r->code_diff_lang;          /* explicit info-string override */
    } else {
        if(path == NULL || path[0] == '\0')
            return;
        detected = fyts_detect_language_for_path(path);  /* heap */
        lang = detected;
    }
    hl = diff_highlight_segment(r, lines, from, to, lang, avail);
    free(detected);
    if(hl == NULL)
        return;
    if(bufs->n == bufs->cap) {
        MD_SIZE nc = bufs->cap ? bufs->cap * 2 : 8;
        char** nv = (char**) realloc(bufs->v, nc * sizeof(*nv));
        if(nv == NULL) {          /* cannot track it -- drop the highlighting */
            MD_SIZE i;
            for(i = from; i < to; i++) { lines[i].hl = NULL; lines[i].hl_len = 0; }
            free(hl);
            return;
        }
        bufs->v = nv;
        bufs->cap = nc;
    }
    bufs->v[bufs->n++] = hl;
}
#endif /* MD4C_WITH_FYTS */

/* Render the buffered code block as a GitHub-like unified diff: a new-side
 * line-number gutter, a full-width background band per added/removed row, and
 * the row content highlighted as the language of the file being patched.
 * Returns 1 when the block was emitted, 0 to fall back to the normal paths. */
static int
emit_diff_code(MD_ANSI* r)
{
    DIFF_LINE* lines = NULL;
    MD_SIZE n = 0, cap = 0, i, start;
    long lineno = 0, maxno = 0;
    DIFF_SCAN scan;
    int gutter_w = 0, prefixw, avail, sepw = 0;
    int numbers = r->style->diff_line_numbers;
    int first_row = 1;
    char pad[64];
    char numbuf[24];
#ifdef MD4C_WITH_FYTS
    DIFF_BUFS bufs;
#endif

    scan.in_hunk = 0;
    scan.seen_change = 0;
#ifdef MD4C_WITH_FYTS
    bufs.v = NULL;
    bufs.n = bufs.cap = 0;
#endif

    if(r->code_size == 0)
        return 0;

    /* Split into lines and classify, tracking the new-side line number. */
    for(i = 0; i <= r->code_size; i++) {
        if(i < r->code_size && r->code_buf[i] != '\n')
            continue;
        if(n == cap) {
            MD_SIZE nc = cap ? cap * 2 : 64;
            DIFF_LINE* nl = (DIFF_LINE*) realloc(lines, nc * sizeof(*lines));
            if(nl == NULL) { free(lines); return 0; }
            lines = nl;
            cap = nc;
        }
        start = (n == 0) ? 0 : lines[n-1].start + lines[n-1].len + 1;
        lines[n].start = start;
        lines[n].len = i - start;
        lines[n].kind = diff_classify(r->code_buf + start, lines[n].len, &scan);
        lines[n].lineno = 0;
        lines[n].hl = NULL;
        lines[n].hl_len = 0;
        switch(lines[n].kind) {
            case DIFF_HUNK:
                lineno = diff_hunk_newline_start(r->code_buf + start, lines[n].len);
                break;
            case DIFF_ADD:
            case DIFF_CTX:
                if(lineno > 0) {
                    lines[n].lineno = lineno++;
                    if(lines[n].lineno > maxno)
                        maxno = lines[n].lineno;
                }
                break;
            default:
                break;
        }
        n++;
        if(i == r->code_size)
            break;
    }
    if(n == 0) { free(lines); return 0; }

    /* Gutter width: widest number in the block, at least 3 columns. */
    if(numbers) {
        gutter_w = 1;
        while(maxno >= 10) { maxno /= 10; gutter_w++; }
        if(gutter_w < 3)
            gutter_w = 3;
        sepw = ansi_disp_width(r->style->diff_gutter_sep,
                               (MD_SIZE) strlen(r->style->diff_gutter_sep));
    }

    prefixw = code_prefix_width(r);
    avail = (r->wrap_cols > 0)
          ? r->wrap_cols - ansi_indent_width(r) - prefixw - DOC_MARGIN
            - (numbers ? gutter_w + sepw + 1 : 0)
          : 0;
    if(r->wrap_cols > 0 && avail < 1)
        avail = 1;

#ifdef MD4C_WITH_FYTS
    /* Highlight each file's rows as that file's own language: a patch touching
     * several files carries a language per segment, so the block is split at
     * every file header and highlighted a segment at a time. */
    if(r->style->diff_inner_highlight && r->style->code_enabled &&
       !(r->flags & MD_ANSI_FLAG_NO_COLOR)) {
        MD_SIZE seg_start = 0;
        char path[512];
        int have_path = 0;

        path[0] = '\0';
        for(i = 0; i <= n; i++) {
            const char* lp = (i < n) ? r->code_buf + lines[i].start : NULL;
            MD_SIZE llen = (i < n) ? lines[i].len : 0;
            int boundary = (i == n);

            if(i < n && lines[i].kind == DIFF_FILE && llen >= 4) {
                /* "diff --git ..." always opens a file; a "--- " does too, but
                 * only once the current segment has already named its file. */
                if(memcmp(lp, "diff", 4) == 0 ||
                   (memcmp(lp, "---", 3) == 0 && have_path))
                    boundary = 1;
            }
            if(boundary && i > seg_start) {
                diff_flush_segment(r, lines, seg_start, i, path, avail, &bufs);
                path[0] = '\0';
                have_path = 0;
                seg_start = i;
            }
            if(i == n)
                break;
            /* Record the segment's file: "+++ b/x" (the post-image) wins over
             * "--- a/x", which may be /dev/null for a newly added file. */
            if(lines[i].kind == DIFF_FILE && llen >= 4 &&
               (memcmp(lp, "+++", 3) == 0 ||
                (memcmp(lp, "---", 3) == 0 && !have_path))) {
                if(diff_header_path(lp, llen, path, sizeof(path)))
                    have_path = (lp[0] == '+');
                else
                    path[0] = '\0';
            }
        }
    }
#endif /* MD4C_WITH_FYTS */

    /* Emit the rows. */
    for(i = 0; i < n; i++) {
        const char* lp = r->code_buf + lines[i].start;
        MD_SIZE llen = lines[i].len;
        const MD_STYLE_PAIR* row;
        const char* body;
        MD_SIZE body_len;
        int w;

        /* A trailing empty line is the block's final newline, not a row. */
        if(i + 1 == n && llen == 0)
            break;

        switch(lines[i].kind) {
            case DIFF_ADD:  row = &r->style->diff_added;   break;
            case DIFF_DEL:  row = &r->style->diff_removed; break;
            case DIFF_HUNK: row = &r->style->diff_hunk;    break;
            case DIFF_FILE: row = &r->style->diff_file;    break;
            case DIFF_META: row = &r->style->diff_context; break;
            default:        row = &r->style->diff_context; break;
        }

        render_indent(r);
        RENDER_VERBATIM(r, code_row_prefix(r, first_row, pad, sizeof(pad)));
        first_row = 0;

        if(numbers) {
            int k, pad;
            size_t nlen = 0;
            if(lines[i].lineno > 0) {
                long v = lines[i].lineno;
                char tmp[24];
                size_t t = 0;
                while(v > 0) { tmp[t++] = (char) ('0' + (v % 10)); v /= 10; }
                while(t > 0) numbuf[nlen++] = tmp[--t];
            }
            numbuf[nlen] = '\0';
            pad = gutter_w - (int) nlen;
            render_ansi(r, r->style->diff_gutter.on);
            for(k = 0; k < pad; k++)
                RENDER_VERBATIM(r, " ");
            render_verbatim(r, numbuf, (MD_SIZE) nlen);
            RENDER_VERBATIM(r, r->style->diff_gutter_sep);
            render_ansi(r, r->style->diff_gutter.off);
            RENDER_VERBATIM(r, " ");
        }

        /* Row content: the marker is kept, the payload may be highlighted. */
        body = NULL;
        body_len = 0;
        if(lines[i].kind <= DIFF_DEL) {
            if(lines[i].hl != NULL) {
                body = lines[i].hl;
                body_len = lines[i].hl_len;
            } else if(llen > 1) {
                body = lp + 1;
                body_len = llen - 1;
                if(avail > 0)
                    body_len = ansi_clip_bytes(body, body_len, avail - 1);
            }
        } else {
            body = lp;
            body_len = llen;
            if(avail > 0)
                body_len = ansi_clip_bytes(body, body_len, avail);
        }

        render_ansi(r, row->on);
        if(lines[i].kind <= DIFF_DEL) {
            char marker = (lines[i].kind == DIFF_ADD) ? '+'
                        : (lines[i].kind == DIFF_DEL) ? '-' : ' ';
            render_verbatim(r, &marker, 1);
            w = 1;
        } else {
            w = 0;
        }
        if(body_len > 0) {
            render_diff_payload(r, body, body_len, row->on);
            w += ansi_disp_width(body, body_len);
        }
        /* Pad the band out to the right margin so the row reads as one block. */
        if(avail > 0 && row->on[0] != '\0' && !(r->flags & MD_ANSI_FLAG_NO_COLOR)) {
            while(w < avail) { RENDER_VERBATIM(r, " "); w++; }
        }
        render_ansi(r, row->off);
        render_newline(r);
    }

    free(lines);
#ifdef MD4C_WITH_FYTS
    diff_bufs_free(&bufs);
#endif
    return 1;
}

/* The rows a block renderer emits, collected until it answers. */
typedef struct {
    char* data;
    size_t size;
    size_t cap;
    int oom;
} ANSI_BLOCK_BUF;

static void
custom_block_emit(void* emit_ctx, const char* data, size_t len)
{
    ANSI_BLOCK_BUF* b = (ANSI_BLOCK_BUF*) emit_ctx;
    size_t cap;
    char* nd;

    if(b->oom || len == 0)
        return;
    if(b->size + len > b->cap) {
        cap = b->cap ? b->cap : 256;
        while(cap < b->size + len)
            cap *= 2;
        nd = (char*) realloc(b->data, cap);
        if(nd == NULL) {
            b->oom = 1;
            return;
        }
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->size, data, len);
    b->size += len;
}

/* Hand a closed fenced block to its block renderer and lay out the rows it
 * emits under the current indent. Returns 3 when the renderer drew the block,
 * or 0 to render it as code. */
static int
emit_custom_block(MD_ANSI* r)
{
    const MD_BLOCK_RENDERER* br = r->code_custom;
    ANSI_BLOCK_BUF buf;
    char lang[sizeof(r->code_lang)];
    size_t i, start;
    int width, rc;

    /* A fence that the stream has not closed is not a block yet. */
    if((r->flags & MD_ANSI_FLAG_STREAM_OPEN_CODE)
       && r->list_depth == 0 && r->quote_depth == 0)
        return 0;

    memset(&buf, 0, sizeof(buf));
    memcpy(lang, r->code_lang, r->code_lang_size);
    lang[r->code_lang_size] = '\0';
    width = (r->wrap_cols > 0) ? r->wrap_cols - ansi_indent_width(r) - DOC_MARGIN : 0;
    if(r->wrap_cols > 0 && width < 1)
        width = 1;
    rc = br->fn(br->userdata, lang, r->code_buf ? r->code_buf : "", r->code_size,
                width, (r->flags & MD_ANSI_FLAG_NO_COLOR) ? FYMD_BF_NO_COLOR : 0,
                custom_block_emit, &buf);
    if(rc != 0 || buf.oom) {
        free(buf.data);
        return 0;
    }
    for(i = 0, start = 0; i <= buf.size; i++) {
        if(i < buf.size && buf.data[i] != '\n')
            continue;
        if(i == buf.size && i == start)
            break;                       /* trailing newline, no row */
        render_indent(r);
        if(i > start)
            render_verbatim(r, buf.data + start, (MD_SIZE) (i - start));
        render_newline(r);
        start = i + 1;
    }
    free(buf.data);
    return 3;
}


/* ---- UI Markdown tags ---- */

static void
ui_html_append(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    MD_SIZE nc;
    char* p;

    if(r->html_size + size > r->html_cap) {
        nc = r->html_cap ? r->html_cap * 2 : 256;
        while(nc < r->html_size + size)
            nc *= 2;
        p = (char*) realloc(r->html_buf, nc);
        if(p == NULL)
            return;
        r->html_buf = p;
        r->html_cap = nc;
    }
    memcpy(r->html_buf + r->html_size, text, size);
    r->html_size += size;
}

static void
ui_marker(MD_ANSI* r, const char* kind, const char* arg, size_t arg_len, int direct)
{
    char buf[128];
    int n;

    n = snprintf(buf, sizeof(buf), MD_UI_MARK_OPEN "%s%s%.*s" MD_UI_MARK_CLOSE,
                 kind, arg ? "=" : "", (int) arg_len, arg ? arg : "");
    if(n <= 0 || (size_t) n >= sizeof(buf))
        return;
    if(direct)
        out_direct(r, buf, (MD_SIZE) n);
    else
        render_verbatim(r, buf, (MD_SIZE) n);
}

static void ui_slot_inline(MD_ANSI* r, const MD_UI_TAG* t);
static void ui_size_parse(const char* s, size_t n, int* kind, int* val);

/* An inline fy-* tag: fill, act, role, glyph or slot. */
static void
ui_inline_tag(MD_ANSI* r, const MD_CHAR* text, MD_SIZE size)
{
    MD_UI_TAG t;
    const char* v;
    size_t vl;
#ifdef MD4C_WITH_FYPALETTE
    const struct fypal_role* role;
    const char* glyph;
    char name[128];
#endif

    if(md_ui_tag_parse(text, size, &t) != 0)
        return;
    if(md_ui_tag_is(&t, "fill")) {
        if(t.closing)
            return;
        /* char="X": the fill is drawn with one glyph of one column */
        v = md_ui_tag_attr(&t, "char", &vl);
        if(v != NULL && vl > 0 && vl <= 4 && memchr(v, 0x1b, vl) == NULL) {
            unsigned cp;
            MD_SIZE cl = ansi_utf8_decode(v, (MD_SIZE) vl, &cp);
            if(cl == vl && fymd_cp_width(cp) == 1 && cp != '\\') {
                ui_marker(r, "fill", v, vl, 0);
                return;
            }
        }
        ui_marker(r, "fill", NULL, 0, 0);
    } else if(md_ui_tag_is(&t, "act")) {
        if(t.closing) {
            ui_marker(r, "/act", NULL, 0, 0);
            render_ansi(r, r->style->action.off);
        } else {
            v = md_ui_tag_attr(&t, "id", &vl);
            if(v == NULL || !md_ui_id_valid(v, vl))
                return;
            render_ansi(r, r->style->action.on);
            ui_marker(r, "act", v, vl, 0);
        }
    } else if(md_ui_tag_is(&t, "role")) {
        if(t.closing) {
            if(r->ui_role_depth > 0) {
                r->ui_role_depth--;
                if(r->ui_role_depth < MD_UI_ROLE_MAX &&
                   r->ui_role_off[r->ui_role_depth] != NULL)
                    render_ansi(r, r->ui_role_off[r->ui_role_depth]);
            }
            return;
        }
        if(r->ui_role_depth < MD_UI_ROLE_MAX)
            r->ui_role_off[r->ui_role_depth] = NULL;
#ifdef MD4C_WITH_FYPALETTE
        v = md_ui_tag_attr(&t, "name", &vl);
        if(v != NULL && vl < sizeof(name) && r->style->palette != NULL &&
           r->ui_role_depth < MD_UI_ROLE_MAX) {
            memcpy(name, v, vl);
            name[vl] = '\0';
            role = fypal_ctx_role(r->style->palette, name);
            if(role != NULL) {
                render_ansi(r, fypal_role_on(r->style->palette, role));
                r->ui_role_off[r->ui_role_depth] =
                    fypal_role_off(r->style->palette, role);
            }
        }
#endif
        r->ui_role_depth++;
    } else if(md_ui_tag_is(&t, "glyph")) {
        if(t.closing)
            return;
#ifdef MD4C_WITH_FYPALETTE
        v = md_ui_tag_attr(&t, "name", &vl);
        if(v != NULL && vl < sizeof(name) && r->style->palette != NULL) {
            memcpy(name, v, vl);
            name[vl] = '\0';
            glyph = fypal_ctx_glyph(r->style->palette, name,
                                    r->style->palette_ascii != 0);
            if(glyph != NULL) {
                RENDER_VERBATIM(r, glyph);
                return;
            }
        }
#endif
        v = md_ui_tag_attr(&t, "fallback", &vl);
        if(v != NULL)
            render_verbatim(r, v, (MD_SIZE) vl);
    } else if(md_ui_tag_is(&t, "slot")) {
        if(!t.closing)
            ui_slot_inline(r, &t);
    }
}

static void
ui_col_append(const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    MD_ANSI_COLUMNS* c = (MD_ANSI_COLUMNS*) userdata;
    int j = c->cur;
    MD_SIZE nc;
    char* p;

    if(size == 0)
        return;
    if(c->size[j] + size > c->cap[j]) {
        nc = c->cap[j] ? c->cap[j] * 2 : 256;
        while(nc < c->size[j] + size)
            nc *= 2;
        p = (char*) realloc(c->buf[j], nc);
        if(p == NULL)
            return;
        c->buf[j] = p;
        c->cap[j] = nc;
    }
    memcpy(c->buf[j] + c->size[j], text, size);
    c->size[j] += size;
}

/* A size: "N" columns (kind 0), "N%" of the width (1), or "*" and "N*", a
 * weighted share of the rest (2). */
static void
ui_size_parse(const char* s, size_t n, int* kind, int* val)
{
    size_t i = 0;

    while(i < n && s[i] == ' ')
        i++;
    while(n > i && s[n - 1] == ' ')
        n--;
    *kind = 0;
    *val = 0;
    for(; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        if(*val < 100000)
            *val = *val * 10 + (s[i] - '0');
    if(i < n && s[i] == '*') {
        *kind = 2;
        if(*val == 0)
            *val = 1;
    } else if(i < n && s[i] == '%') {
        *kind = 1;
    }
}

/*
 * The column widths of widths="20,30%,2*,*" in @avail columns with @gap
 * between them. min="8" is the least width of each column that is not fixed;
 * min="0,8,4" gives each column its own.
 */
static int
ui_columns_widths(MD_ANSI_COLUMNS* c, const char* spec, size_t len,
                  const char* mins, size_t mins_len, int avail)
{
    int kind[MD_UI_COLS_MAX] = {0}, val[MD_UI_COLS_MAX] = {0};
    int mn[MD_UI_COLS_MAX] = {0}, w[MD_UI_COLS_MAX] = {0};
    int wmin[MD_UI_COLS_MAX] = {0}, share[MD_UI_COLS_MAX] = {0};
    int idx[MD_UI_COLS_MAX] = {0};
    int n = 0, nmin = 0, used, rest, j, k, nw;
    size_t i = 0, start;

    while(i <= len && n < MD_UI_COLS_MAX) {
        start = i;
        while(i < len && spec[i] != ',')
            i++;
        ui_size_parse(spec + start, i - start, &kind[n], &val[n]);
        n++;
        i++;
    }
    for(i = 0; mins != NULL && i <= mins_len && nmin < MD_UI_COLS_MAX; i++) {
        start = i;
        while(i < mins_len && mins[i] != ',')
            i++;
        ui_size_parse(mins + start, i - start, &k, &mn[nmin]);
        nmin++;
    }
    if(n == 0)
        return -1;
    for(j = 0; j < n; j++)
        if(nmin == 0)
            mn[j] = 0;
        else if(nmin == 1)
            mn[j] = mn[0];
        else if(j >= nmin)
            mn[j] = 0;
    avail -= c->gap * (n - 1);
    if(avail < n)
        avail = n;
    for(j = 0, used = 0, nw = 0; j < n; j++) {
        if(kind[j] == 2) {
            idx[nw] = j;
            w[nw] = val[j];
            wmin[nw] = mn[j];
            nw++;
            continue;
        }
        c->width[j] = kind[j] == 1 ? avail * val[j] / 100 : val[j];
        if(kind[j] == 1 && c->width[j] < mn[j])
            c->width[j] = mn[j];
        if(c->width[j] < 1)
            c->width[j] = 1;
        used += c->width[j];
    }
    rest = avail - used;
    md_ui_share(rest > 0 ? rest : 0, w, wmin, nw, share);
    for(k = 0; k < nw; k++)
        c->width[idx[k]] = share[k] > 0 ? share[k] : 1;
    c->n = n;
    return 0;
}

static void
ui_column_begin(MD_ANSI* r)
{
    MD_ANSI_COLUMNS* c = r->ui_cols;

    if(r->line_open)
        flush_wrapped(r);
    c->saved_out = r->process_output;
    c->saved_ud = r->userdata;
    c->saved_wrap = r->wrap_cols;
    c->saved_table_width = r->table_width;
    c->saved_quote = r->quote_depth;
    c->saved_list = r->list_depth;
    c->saved_need_newline = r->need_newline;
    c->saved_row = r->output_row;
    c->saved_row_open = r->row_open;
    c->saved_margin = r->margin_fn;
    c->size[c->cur] = 0;
    r->process_output = ui_col_append;
    r->userdata = c;
    r->wrap_cols = c->width[c->cur];
    r->table_width = c->width[c->cur];     /* a table fits its column */
    r->quote_depth = 0;
    r->list_depth = 0;
    r->need_newline = 0;
    r->margin_fn = NULL;
    r->ui_col_depth = 1;
    c->capturing = 1;
}

static void
ui_column_end(MD_ANSI* r)
{
    MD_ANSI_COLUMNS* c = r->ui_cols;

    if(r->line_open)
        flush_wrapped(r);
    r->process_output = c->saved_out;
    r->userdata = c->saved_ud;
    r->wrap_cols = c->saved_wrap;
    r->table_width = c->saved_table_width;
    r->quote_depth = c->saved_quote;
    r->list_depth = c->saved_list;
    r->need_newline = c->saved_need_newline;
    r->output_row = c->saved_row;
    r->row_open = c->saved_row_open;
    r->margin_fn = c->saved_margin;
    r->ui_col_depth = 0;
    c->capturing = 0;
    c->cur++;
}

/* A row holds text when it has a character that is not a blank. */
static int
ui_row_blank(const char* p, MD_SIZE n)
{
    MD_SIZE i = 0, e;

    while(i < n) {
        e = ansi_esc_len(p + i, n - i);
        if(e > 0) {
            if(e > MD_UI_MARK_OPEN_LEN &&
               memcmp(p + i, MD_UI_MARK_OPEN, MD_UI_MARK_OPEN_LEN) == 0)
                return 0;   /* a slot row: the column keeps it */
            i += e;
            continue;
        }
        if(p[i] != ' ' && p[i] != '\r')
            return 0;
        i++;
    }
    return 1;
}

/* Place the captured columns side by side, row by row. */
static void
ui_columns_emit(MD_ANSI* r)
{
    MD_ANSI_COLUMNS* c = r->ui_cols;
    const char* rows_p[MD_UI_COLS_MAX];
    MD_SIZE rows_n[MD_UI_COLS_MAX], first[MD_UI_COLS_MAX], last[MD_UI_COLS_MAX];
    int nrows[MD_UI_COLS_MAX];
    int j, height = 0, k, w, pad;
    MD_SIZE pos, end, rs;
    const char* nl;

    /* the rows of each column without its leading and trailing blank rows */
    for(j = 0; j < c->n; j++) {
        const char* b = c->buf[j];
        MD_SIZE size = j < c->cur ? c->size[j] : 0;
        first[j] = 0;
        last[j] = 0;
        nrows[j] = 0;
        for(pos = 0; pos < size; pos = end + 1) {
            nl = (const char*) memchr(b + pos, '\n', size - pos);
            end = nl ? (MD_SIZE)(nl - b) : size;
            if(ui_row_blank(b + pos, end - pos)) {
                if(nrows[j] == 0)
                    first[j] = end + 1;
                if(!nl)
                    break;
                continue;
            }
            nrows[j] = 1;
            last[j] = nl ? end + 1 : end;
            if(!nl)
                break;
        }
        nrows[j] = 0;
        for(pos = first[j]; pos < last[j]; pos = end + 1) {
            nl = (const char*) memchr(b + pos, '\n', last[j] - pos);
            end = nl ? (MD_SIZE)(nl - b) : last[j];
            nrows[j]++;
            if(!nl)
                break;
        }
        rows_p[j] = b;
        rows_n[j] = first[j];
        if(nrows[j] > height)
            height = nrows[j];
    }

    if(r->need_newline) {
        render_separator(r);
        r->need_newline = 0;
    }
    for(k = 0; k < height; k++) {
        render_indent(r);
        for(j = 0; j < c->n; j++) {
            if(j > 0)
                for(pad = 0; pad < c->gap; pad++)
                    out_direct(r, " ", 1);
            w = 0;
            if(k < nrows[j]) {
                pos = rows_n[j];
                nl = (const char*) memchr(rows_p[j] + pos, '\n', last[j] - pos);
                end = nl ? (MD_SIZE)(nl - rows_p[j]) : last[j];
                rs = end - pos;
                if(rs > 0) {
                    out_direct(r, rows_p[j] + pos, rs);
                    w = ansi_disp_width(rows_p[j] + pos, rs);
                    if(memchr(rows_p[j] + pos, 0x1b, rs) != NULL &&
                       !(r->flags & MD_ANSI_FLAG_NO_COLOR))
                        out_direct(r, "\x1b[0m", 4);
                }
                rows_n[j] = end + 1;
            }
            if(j + 1 < c->n)
                for(pad = w; pad < c->width[j]; pad++)
                    out_direct(r, " ", 1);
        }
        out_direct(r, "\n", 1);
    }
    r->need_newline = 1;
}

static void
ui_columns_free(MD_ANSI* r)
{
    int j;

    if(r->ui_cols == NULL)
        return;
    for(j = 0; j < MD_UI_COLS_MAX; j++)
        free(r->ui_cols->buf[j]);
    free(r->ui_cols);
    r->ui_cols = NULL;
}

/* A row marker for the vertical pass: its own row, zero width. */
static void
ui_row_marker(MD_ANSI* r, const char* kind, const char* arg, size_t arg_len)
{
    if(!(r->flags & MD_ANSI_FLAG_UI_ROWS) || r->ui_col_depth)
        return;
    if(r->line_open)
        flush_wrapped(r);
    if(r->need_newline) {
        render_separator(r);
        r->need_newline = 0;
    }
    ui_marker(r, kind, arg, arg_len, 1);
    out_direct(r, "\n", 1);
}


#define MD_UI_SLOT_MAX_WIDTH  512
#define MD_UI_SLOT_MAX_HEIGHT 1000

/* The bytes of @buf that fit in @width columns. An escape takes no column and
 * stays with the text before the cut. */
static MD_SIZE
ui_clip_cols(const char* buf, MD_SIZE size, int width)
{
    MD_SIZE i = 0, e, cl;
    unsigned cp;
    int w = 0, cw;

    while(i < size) {
        e = ansi_esc_len(buf + i, size - i);
        if(e > 0) {
            i += e;
            continue;
        }
        cl = ansi_utf8_decode(buf + i, size - i, &cp);
        cw = fymd_cp_width(cp);
        if(w + cw > width)
            break;
        w += cw;
        i += cl ? cl : 1;
    }
    return i;
}

/* Ask the slot renderer for the content of a slot; NULL leaves it blank. */
static char*
ui_slot_content(MD_ANSI* r, const char* id, int width, int height, size_t* len)
{
    ANSI_BLOCK_BUF buf;
    int rc;

    *len = 0;
    if(r->style->slot_fn == NULL)
        return NULL;
    memset(&buf, 0, sizeof(buf));
    rc = r->style->slot_fn(r->style->slot_userdata, id, width, height,
                           (r->flags & MD_ANSI_FLAG_NO_COLOR) ? FYMD_BF_NO_COLOR : 0,
                           custom_block_emit, &buf);
    if(rc != 0 || buf.oom) {
        free(buf.data);
        return NULL;
    }
    *len = buf.size;
    return buf.data;
}

/* The id and the dimension attribute of a slot tag. Returns 0, or -1. */
static int
ui_slot_attrs(const MD_UI_TAG* t, const char* dim, char* id, size_t id_size,
              int* value, int* star)
{
    const char* v;
    size_t vl;

    v = md_ui_tag_attr(t, "id", &vl);
    if(v == NULL || !md_ui_id_valid(v, vl) || vl >= id_size)
        return -1;
    memcpy(id, v, vl);
    id[vl] = '\0';
    *value = -1;
    *star = 0;
    v = md_ui_tag_attr(t, dim, &vl);
    if(v != NULL) {
        int kind, val;
        ui_size_parse(v, vl, &kind, &val);
        if(kind == 2)
            *star = val;        /* the weight of an elastic size */
        else
            *value = val;
    }
    return 0;
}

/*
 * An inline slot: @width cells in the row, drawn by the slot renderer or left
 * blank. The cells are non-breaking, so a row wraps around the slot and never
 * inside it.
 */
static void
ui_slot_inline(MD_ANSI* r, const MD_UI_TAG* t)
{
    static const char nbsp[] = "\xc2\xa0";
    char id[64], arg[96];
    const char* p;
    char* content;
    size_t clen, i, rowlen;
    MD_SIZE clip, e;
    int width, star, w = 0, n;

    if(ui_slot_attrs(t, "width", id, sizeof(id), &width, &star) != 0)
        return;
    if(width < 1)
        width = 1;
    if(width > MD_UI_SLOT_MAX_WIDTH)
        width = MD_UI_SLOT_MAX_WIDTH;
    n = snprintf(arg, sizeof(arg), "%s:%d:1", id, width);
    if(n > 0 && (size_t) n < sizeof(arg))
        ui_marker(r, "slot", arg, (size_t) n, 0);

    content = ui_slot_content(r, id, width, 1, &clen);
    if(content != NULL) {
        p = (const char*) memchr(content, '\n', clen);
        rowlen = p ? (size_t)(p - content) : clen;
        clip = ui_clip_cols(content, (MD_SIZE) rowlen, width);
        w = ansi_disp_width(content, clip);
        for(i = 0; i < clip; ) {
            e = ansi_esc_len(content + i, clip - i);
            if(e > 0) {
                render_verbatim(r, content + i, e);
                i += e;
            } else if(content[i] == ' ') {
                render_verbatim(r, nbsp, 2);
                i++;
            } else {
                render_verbatim(r, content + i, 1);
                i++;
            }
        }
        free(content);
    }
    for(; w < width; w++)
        render_verbatim(r, nbsp, 2);
}

/*
 * A block slot: rows at the width of the page or of the column, drawn by the
 * slot renderer or left blank. A height of "*" is elastic: with a page height
 * the vertical layout gives it rows like a vfill, and it is reported only.
 */
static void
ui_slot_block(MD_ANSI* r, const MD_UI_TAG* t)
{
    char id[64], arg[112];
    char* content;
    const char* nl;
    size_t clen, pos, end;
    MD_SIZE clip;
    int height, star, width, rows = 0, h, k, n, indent;

    if(ui_slot_attrs(t, "height", id, sizeof(id), &height, &star) != 0)
        return;
    if(r->line_open)
        flush_wrapped(r);
    if(r->need_newline) {
        render_separator(r);
        r->need_newline = 0;
    }
    indent = ansi_indent_width(r);
    width = (r->wrap_cols > 0 ? r->wrap_cols : 80) - indent - DOC_MARGIN;
    if(width < 1)
        width = 1;

    if(star) {
        if((r->flags & MD_ANSI_FLAG_UI_ROWS) && !r->ui_col_depth) {
            const char* mv;
            size_t ml;
            int mk, min = 0;
            mv = md_ui_tag_attr(t, "min", &ml);
            if(mv != NULL)
                ui_size_parse(mv, ml, &mk, &min);
            n = snprintf(arg, sizeof(arg), "%s:%d:%d:%d:%d", id, indent, width,
                         star, min);
            if(n > 0 && (size_t) n < sizeof(arg))
                ui_row_marker(r, "vslot", arg, (size_t) n);
            r->need_newline = 1;
            return;
        }
        /* no page to take rows from: its least rows, or one */
        {
            const char* mv;
            size_t ml;
            int mk, min = 0;
            mv = md_ui_tag_attr(t, "min", &ml);
            if(mv != NULL)
                ui_size_parse(mv, ml, &mk, &min);
            height = min > 0 ? min : 1;
        }
    }

    content = ui_slot_content(r, id, width, height > 0 ? height : 0, &clen);
    for(pos = 0; content != NULL && pos < clen; pos = end + 1) {
        nl = (const char*) memchr(content + pos, '\n', clen - pos);
        end = nl ? (size_t)(nl - content) : clen;
        rows++;
    }
    h = height > 0 ? height : (rows > 0 ? rows : 1);
    if(h > MD_UI_SLOT_MAX_HEIGHT)
        h = MD_UI_SLOT_MAX_HEIGHT;

    for(k = 0, pos = 0; k < h; k++) {
        render_indent(r);
        if(k == 0) {
            n = snprintf(arg, sizeof(arg), "%s:%d:%d", id, width, h);
            if(n > 0 && (size_t) n < sizeof(arg))
                ui_marker(r, "slot", arg, (size_t) n, 1);
        } else {
            /* a blank slot row is a row: a column must not trim it */
            ui_marker(r, "keep", NULL, 0, 1);
        }
        if(content != NULL && k < rows) {
            nl = (const char*) memchr(content + pos, '\n', clen - pos);
            end = nl ? (size_t)(nl - content) : clen;
            clip = ui_clip_cols(content + pos, (MD_SIZE)(end - pos), width);
            if(clip > 0) {
                out_direct(r, content + pos, clip);
                if(memchr(content + pos, 0x1b, clip) != NULL &&
                   !(r->flags & MD_ANSI_FLAG_NO_COLOR))
                    out_direct(r, "\x1b[0m", 4);
            }
            pos = end + 1;
        }
        out_direct(r, "\n", 1);
    }
    free(content);
    r->need_newline = 1;
}

/* The fy-* tags of an HTML block: columns, vfill, scroll and slot. */
static void
ui_block_tags(MD_ANSI* r, const char* text, MD_SIZE size)
{
    MD_UI_TAG t;
    const char* v;
    size_t vl;
    MD_SIZE i;
    int avail, gap;

    for(i = 0; i < size; i++) {
        if(text[i] != '<' || md_ui_tag_parse(text + i, size - i, &t) != 0)
            continue;
        if(md_ui_tag_is(&t, "columns") && !t.closing) {
            if(r->ui_cols != NULL || r->ui_col_depth)
                goto next;
            r->ui_cols = (MD_ANSI_COLUMNS*) calloc(1, sizeof(*r->ui_cols));
            if(r->ui_cols == NULL)
                goto next;
            gap = 2;
            v = md_ui_tag_attr(&t, "gap", &vl);
            if(v != NULL)
                for(gap = 0; vl > 0 && *v >= '0' && *v <= '9'; v++, vl--)
                    gap = gap * 10 + (*v - '0');
            r->ui_cols->gap = gap;
            avail = (r->wrap_cols > 0 ? r->wrap_cols : 80) -
                    ansi_indent_width(r) - DOC_MARGIN;
            v = md_ui_tag_attr(&t, "widths", &vl);
            if(v == NULL) {
                v = "*,*";
                vl = 3;
            }
            {
                size_t ml = 0;
                const char* m = md_ui_tag_attr(&t, "min", &ml);
                if(ui_columns_widths(r->ui_cols, v, vl, m, ml, avail) != 0)
                    ui_columns_free(r);
            }
        } else if(md_ui_tag_is(&t, "columns") && t.closing) {
            if(r->ui_cols == NULL)
                goto next;
            if(r->ui_cols->capturing)
                ui_column_end(r);
            ui_columns_emit(r);
            ui_columns_free(r);
        } else if(md_ui_tag_is(&t, "col") && !t.closing) {
            if(r->ui_cols == NULL || r->ui_cols->capturing ||
               r->ui_cols->cur >= r->ui_cols->n)
                goto next;
            ui_column_begin(r);
        } else if(md_ui_tag_is(&t, "col") && t.closing) {
            if(r->ui_cols != NULL && r->ui_cols->capturing)
                ui_column_end(r);
        } else if(md_ui_tag_is(&t, "vfill") && !t.closing) {
            char arg[32];
            int weight = 1, min = 0, n;
            v = md_ui_tag_attr(&t, "weight", &vl);
            if(v != NULL)
                ui_size_parse(v, vl, &n, &weight);
            v = md_ui_tag_attr(&t, "min", &vl);
            if(v != NULL)
                ui_size_parse(v, vl, &n, &min);
            n = snprintf(arg, sizeof(arg), "%d:%d", weight, min);
            ui_row_marker(r, "vfill", arg, (size_t) n);
        } else if(md_ui_tag_is(&t, "scroll")) {
            if(t.closing) {
                ui_row_marker(r, "/scroll", NULL, 0);
            } else {
                v = md_ui_tag_attr(&t, "anchor", &vl);
                ui_row_marker(r, "scroll", v, v ? vl : 0);
            }
        } else if(md_ui_tag_is(&t, "slot") && !t.closing) {
            ui_slot_block(r, &t);
        }
next:
        if(t.len > 0)
            i += t.len - 1;
    }
}

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
                    RENDER_VERBATIM(r, r->style->task_done_glyph);
                    render_ansi(r, r->style->task_done.off);
                } else {
                    RENDER_VERBATIM(r, r->style->task_open_glyph);
                }
                RENDER_VERBATIM(r, " ");
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
            r->heading_level = ((MD_BLOCK_H_DETAIL*) detail)->level;
            render_ansi(r, heading_pair(r)->on);
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
            r->code_diff = 0;
            r->code_diff_lang[0] = '\0';
            r->code_fyts = 0;
            r->code_custom = NULL;
            {
                const MD_BLOCK_CODE_DETAIL* det = (const MD_BLOCK_CODE_DETAIL*) detail;
                if(det->lang.text != NULL && det->lang.size > 0) {
                    MD_SIZE sz = det->lang.size < sizeof(r->code_lang)
                               ? det->lang.size : sizeof(r->code_lang) - 1;
                    memcpy(r->code_lang, det->lang.text, sz);
                    r->code_lang_size = sz;
                }
                if(r->style->diff_enabled)
                    r->code_diff = ansi_diff_info(det->info.text, det->info.size,
                                                  r->code_diff_lang,
                                                  sizeof(r->code_diff_lang));
            }
#ifdef MD4C_WITH_FYTS
            /* Highlight when the info string names a language libfyts supports;
             * the text is then buffered until the block closes (the plain
             * ANSI_DIM styling is skipped while doing so). Checking support up
             * front avoids fyts emitting an "unknown language" diagnostic. */
            if(r->style->code_enabled && r->code_lang_size > 0) {
                r->code_lang[r->code_lang_size] = '\0';
                if(fyts_language_supported(r->code_lang)) {
                    r->code_highlight = 1;
                    r->code_fyts = 1;
                }
            }
#endif
            /* A diff block is rendered by emit_diff_code() (never by the fyts
             * "diff" grammar), so buffer its text regardless of fyts support. */
            if(r->code_diff)
                r->code_highlight = 1;

            /* A renderer registered for the language draws the block when it
             * closes, so its text is buffered like highlighted code. A diff is
             * never handed to one. */
            if(!r->code_diff)
                r->code_custom = md_ansi_style_block_renderer(r->style,
                                        r->code_lang, r->code_lang_size);
            if(r->code_custom != NULL)
                r->code_highlight = 1;

            /* Header rule (with the language label, when present). In reverse
             * mode the header is deferred to leave, where it is drawn on fyts's
             * frame background together with the code. */
            /* A block renderer draws no code chrome; the header waits for its
             * answer. */
            if(r->code_custom == NULL &&
               !(r->code_highlight && !r->code_diff &&
                 r->style->code_reverse && !r->card))
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
                r->code_on_pending = 1;   /* emitted at the first body byte */
            break;

        case MD_BLOCK_HTML:
            if(r->flags & MD_ANSI_FLAG_UI) {
                r->in_html_block = 1;
                r->html_size = 0;
            }
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
            render_ansi(r, heading_pair(r)->off);
            r->heading_level = 0;
            render_newline(r);
            r->need_newline = 1;
            break;

        case MD_BLOCK_CODE: {
            /* done: 0 = not highlighted / fell back, 1 = code emitted (draw the
             * footer here), 2 = whole block (header+code+footer) already emitted. */
            int done = 0;
            if(r->code_custom != NULL) {
                done = emit_custom_block(r);
                r->code_custom = NULL;
                /* Declined: draw the header that waited. In reverse mode the
                 * code path draws its own. */
                if(done == 0 && !(r->style->code_reverse && !r->card))
                    render_code_rule(r, r->code_lang, r->code_lang_size);
            }
            if(done == 0 && r->code_highlight) {
                if(r->code_diff)
                    done = emit_diff_code(r);
#ifdef MD4C_WITH_FYTS
                if(!done && (r->code_fyts || r->code_diff))
                    done = emit_highlighted_code(r, 1);
#endif
                if(done == 0) {
                    /* Fall back to plain. In reverse mode the header was deferred
                     * to here, so draw it (plain) before the body. */
                    if(r->style->code_reverse && !r->card)
                        render_code_rule(r, r->code_lang, r->code_lang_size);
                    emit_plain_code(r);
                }
            } else if(done == 0) {
                if(!r->code_on_pending)
                    render_ansi(r, r->style->code_block.off);
                r->code_on_pending = 0;
            }
            r->code_highlight = 0;
            if((r->flags & MD_ANSI_FLAG_CODE_META) && r->n_code_blocks < r->code_blocks_cap) {
                r->code_blocks[r->n_code_blocks].end = r->output_offset;
                r->n_code_blocks++;
            }
            /* 2: the code path drew the whole block; 3: a block renderer did. */
            if(done != 2 && done != 3) {
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
            if(r->in_html_block) {
                r->in_html_block = 0;
                ui_block_tags(r, r->html_buf, r->html_size);
            }
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
            /* Raw HTML renders as nothing; a UI render acts on its fy-* tags. */
            if(r->flags & MD_ANSI_FLAG_UI) {
                if(r->in_html_block)
                    ui_html_append(r, text, size);
                else
                    ui_inline_tag(r, text, size);
            }
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
                if(r->code_on_pending) {
                    render_ansi(r, r->style->code_block.on);
                    r->code_on_pending = 0;
                }
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
                             parser_flags, renderer_flags, width, NULL, NULL);
}

int
md_ansi_ex_styled(const MD_CHAR* input, MD_SIZE input_size,
                  void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                  void* userdata, unsigned parser_flags, unsigned renderer_flags,
                  int width, const struct MD_ANSI_STYLE* style, size_t *output_rows)
{
    return md_ansi_ex_styled_ctx(input, input_size, process_output, userdata,
                                 parser_flags, renderer_flags, width, style, NULL, output_rows);
}

int
md_ansi_ex_styled_margins_ctx(const MD_CHAR* input, MD_SIZE input_size,
                  void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                  void* userdata, unsigned parser_flags, unsigned renderer_flags,
                  int width, const struct MD_ANSI_STYLE* style,
                  MD_ANSI_MARGIN_FN margin_fn, void* margin_userdata,
                  struct fyts_ctx** fyts_ctx,
		  size_t *output_rows)
{
    return md_ansi_ex_styled_ui(input, input_size, process_output, userdata,
                                parser_flags, renderer_flags, width, style,
                                margin_fn, margin_userdata, fyts_ctx,
                                output_rows, NULL);
}

int
md_ansi_ex_styled_ui(const MD_CHAR* input, MD_SIZE input_size,
                  void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                  void* userdata, unsigned parser_flags, unsigned renderer_flags,
                  int width, const struct MD_ANSI_STYLE* style,
                  MD_ANSI_MARGIN_FN margin_fn, void* margin_userdata,
                  struct fyts_ctx** fyts_ctx,
		  size_t *output_rows, struct MD_ANSI_UI* ui)
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
        ret = md_ansi_ex_styled_ui(hbuf.data, hbuf.size,
                                process_output, userdata,
                                parser_flags, renderer_flags & ~MD_ANSI_FLAG_HEAL,
                                width, style, margin_fn, margin_userdata, fyts_ctx,
				output_rows, ui);
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
    render.margin_fn = margin_fn;
    render.margin_userdata = margin_userdata;
    render.fyts_ctx = fyts_ctx;
    render.ui = ui;
    render.ui_act_seg = -1;
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

        /* Count only now: the trailing flushes above may add a final row. */
        if(output_rows != NULL && ret >= 0)
            *output_rows = render.output_row + (render.row_open ? 1 : 0);

        /* A column left open by the end of the document is dropped. */
        if(render.ui_cols != NULL && render.ui_cols->capturing)
            ui_column_end(&render);
        ui_columns_free(&render);
        free(render.html_buf);
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

int
md_ansi_ex_styled_ctx(const MD_CHAR* input, MD_SIZE input_size,
                  void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                  void* userdata, unsigned parser_flags, unsigned renderer_flags,
                  int width, const struct MD_ANSI_STYLE* style,
                  struct fyts_ctx** fyts_ctx, size_t *output_rows)
{
    return md_ansi_ex_styled_margins_ctx(input, input_size, process_output,
            userdata, parser_flags, renderer_flags, width, style, NULL, NULL,
            fyts_ctx, output_rows);
}

int
md_ansi_ex_styled_margins(const MD_CHAR* input, MD_SIZE input_size,
                  void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                  void* userdata, unsigned parser_flags, unsigned renderer_flags,
                  int width, const struct MD_ANSI_STYLE* style,
                  MD_ANSI_MARGIN_FN margin_fn, void* margin_userdata,
		  size_t *output_rows)
{
    return md_ansi_ex_styled_margins_ctx(input, input_size, process_output,
            userdata, parser_flags, renderer_flags, width, style, margin_fn,
            margin_userdata, NULL, output_rows);
}

/* Emit raw code one physical line at a time. Styled blocks use the same
 * two-column inset, clipping and code_block style as Markdown fences. */
static void
emit_raw_code(MD_ANSI* r, int styled)
{
    MD_SIZE start = 0, end;
    int avail = 0, first = 1;
    char pad[64];
    int prefixw = code_prefix_width(r);

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
            RENDER_VERBATIM(r, code_row_prefix(r, first, pad, sizeof(pad)));
            first = 0;
        }
        if(avail > 0)
            len = ansi_clip_bytes(r->code_buf + start, len, avail);
        if(len > 0)
            render_verbatim(r, r->code_buf + start, len);
        start = end < r->code_size ? end + 1 : end;
        /* close the style BEFORE the final newline: closing after it left
         * an escape-only line -- a blank row -- above the footer rule */
        if(styled && start >= r->code_size)
            render_ansi(r, r->style->code_block.off);
        render_newline(r);
    }
    if(styled && r->code_size == 0)
        render_ansi(r, r->style->code_block.off);
}

int
md_ansi_fenced_styled(const MD_CHAR* input, MD_SIZE input_size,
                       const char* language, fy_generic template_vars,
                       size_t lines, size_t plain_lines, size_t hidden_lines,
                       unsigned fence_flags,
                       void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
                       void* userdata, unsigned renderer_flags, int width,
                       const struct MD_ANSI_STYLE* style,
                       struct fyts_ctx** fyts_ctx)
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
    render.fyts_ctx = fyts_ctx;
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
    /* "diff"/"patch" (with an optional inner language) gets the diff view,
     * whether or not libfyts knows the language -- see emit_diff_code(). */
    if((fence_flags & MD_ANSI_FENCE_HIGHLIGHT) && style->diff_enabled &&
       ansi_diff_info(render.code_lang, render.code_lang_size,
                      render.code_diff_lang, sizeof(render.code_diff_lang))) {
        render.code_diff = 1;
        render.code_highlight = 1;
    }

    if(styled && !(render.code_highlight && !render.code_diff &&
                   style->code_reverse && !render.card))
        render_code_rule(&render, render.code_lang, render.code_lang_size);
    if(render.code_highlight) {
        if(render.code_diff)
            done = emit_diff_code(&render);
#ifdef MD4C_WITH_FYTS
        if(!done)
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
