/*
 * MD4C: Markdown parser for C
 * (http://github.com/mity/md4c)
 *
 * ANSI renderer styling configuration (YAML, via libfyaml).
 */

#ifndef MD4C_STYLE_H
#define MD4C_STYLE_H

#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>
#include <libfymd4c/libfymd4c-util.h>
#include <libfymd4c/libfymd4c-renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A renderer registered for the fenced blocks of one language. */
typedef struct MD_BLOCK_RENDERER {
    char* lang;
    fymd_block_render_fn fn;
    void* userdata;
} MD_BLOCK_RENDERER;

/* An element's "on"/"off" escape sequences. */
typedef struct {
    const char* on;
    const char* off;
} MD_STYLE_PAIR;

typedef enum {
    MD_STYLE_BG_AUTO,
    MD_STYLE_BG_DARK,
    MD_STYLE_BG_LIGHT
} MD_STYLE_BG;

/* Resolved styling for the ANSI renderer. All strings are owned by the struct
 * and freed by md_ansi_style_destroy(). */
typedef struct MD_ANSI_STYLE {
    MD_STYLE_PAIR heading;
    MD_STYLE_PAIR heading_level[6]; /* per level 1-6; a NULL .on uses heading */
    MD_STYLE_PAIR strong;
    MD_STYLE_PAIR emphasis;
    MD_STYLE_PAIR underline;
    MD_STYLE_PAIR strikethrough;
    MD_STYLE_PAIR code;          /* inline code */
    MD_STYLE_PAIR math;
    MD_STYLE_PAIR link;
    MD_STYLE_PAIR action;        /* UI Markdown: the label of a fy-act */
    MD_STYLE_PAIR link_url;
    MD_STYLE_PAIR wikilink;
    MD_STYLE_PAIR blockquote;    /* quote bar styling */
    MD_STYLE_PAIR code_block;    /* plain (unhighlighted) fenced code */
    MD_STYLE_PAIR rule;          /* hr + code header/footer rules */
    MD_STYLE_PAIR table_header;
    MD_STYLE_PAIR table_header_row; /* styling spanning the complete header row */
    MD_STYLE_PAIR table_row_odd;    /* complete body rows, first body row is odd */
    MD_STYLE_PAIR table_row_even;
    MD_STYLE_PAIR diff_added;    /* whole-row styling of a "+" diff line */
    MD_STYLE_PAIR diff_removed;  /* whole-row styling of a "-" diff line */
    MD_STYLE_PAIR diff_context;  /* whole-row styling of an unchanged diff line */
    MD_STYLE_PAIR diff_hunk;     /* "@@ ... @@" hunk header row */
    MD_STYLE_PAIR diff_file;     /* "--- a/x" / "+++ b/x" / "diff --git" rows */
    MD_STYLE_PAIR diff_gutter;   /* the line-number column */
    MD_STYLE_PAIR list_marker;   /* list bullets / ordered numbers */
    MD_STYLE_PAIR task_done;     /* checked task-list marker */
    MD_STYLE_PAIR reverse;       /* whole-document card background (.on = bg set) */
    MD_STYLE_PAIR indicator_pending;
    MD_STYLE_PAIR indicator_success;
    MD_STYLE_PAIR indicator_failure;
    const char* indicator_pending_frames[8];
    size_t indicator_pending_frame_count;
    const char* indicator_success_glyph;
    const char* indicator_failure_glyph;
    unsigned int indicator_interval_ms;

    const char* blockquote_bar;
    const char* list_bullet;
    const char* table_vertical;
    const char* table_horizontal;
    const char* table_cross;
    const char* task_done_glyph; /* checked task-list marker, without the space */
    const char* task_open_glyph; /* open task-list marker, without the space */
    const char* card_bar;        /* quote bar of a card; NULL uses blockquote_bar */
    int         doc_margin;      /* columns before the text and after it */
    int         table_border_none; /* true: no separator or vertical grid glyphs */

    MD_STYLE_BG  background;      /* resolved document background (never AUTO) */

    int          code_enabled;   /* fenced-code syntax highlighting on/off */
    const char*  code_theme;     /* libfyts styling name or path */
    MD_STYLE_BG  code_background;
    int          code_reverse;   /* fyts "reverse" bubble mode for fenced code */
    const char*  code_header;    /* decoration template; "default" => legacy rule */
    const char*  code_footer;    /* decoration template; "default" => legacy rule */
    const char*  code_prefix;    /* prefix placed before every fenced content row */
    const char*  code_bubble_on; /* background SGR; NULL means no bubble */
    const char*  code_bubble_off;
    const char*  code_legend_on; /* foreground SGR for the bubble language */
    const char*  code_legend_off;
    int          code_marker_enabled; /* use code_marker (off by default, so the
                                         config can carry a marker that callers
                                         switch on per render) */
    const char*  code_marker;    /* non-empty: prefix of the FIRST content row
                                    only (e.g. "\u23bf "); the remaining rows use
                                    code_prefix padded to the marker's width, so
                                    the block reads as one hanging-indent item */

    /* GitHub-like rendering of ```diff / ```patch fenced blocks. */
    int          diff_enabled;        /* 0: hand diff blocks to the fyts grammar */
    int          diff_line_numbers;   /* draw the new-side line-number gutter */
    int          diff_inner_highlight;/* highlight payloads as the patched file */
    const char*  diff_gutter_sep;     /* glyph between gutter and content */

    struct fypal_ctx* palette;   /* borrowed palette of the overlay, or NULL */
    int palette_ascii;           /* the overlay took the ASCII glyph forms */
    const MD_BLOCK_RENDERER* block_renderers; /* borrowed from the renderer */
    fymd_slot_render_fn slot_fn;              /* UI Markdown fy-slot, or NULL */
    void* slot_userdata;
    size_t n_block_renderers;
    void* _palette;              /* opaque palette overlay: saved pairs, strings */

    void* _owned;                /* opaque heap-string registry */
} MD_ANSI_STYLE;

/* Overrides applied at style-creation time (no global state). A field left at
 * its "inherit" sentinel takes the value from the YAML config. */
typedef struct MD_ANSI_STYLE_OPTS {
    MD_STYLE_BG background;   /* MD_STYLE_BG_AUTO => use the config's background: */
    int         reverse;      /* < 0 => use the config's code.reverse */
} MD_ANSI_STYLE_OPTS;

/* Build a style from a YAML document. Pass NULL/len 0 for the built-in default
 * config, NULL opts for no overrides. Returns NULL on parse/alloc failure. */
MD_ANSI_STYLE* md_ansi_style_create(const char* yaml, size_t yaml_len,
                                    const MD_ANSI_STYLE_OPTS* opts);

/* Build one of the embedded named themes. NULL/empty selects the default;
 * unknown names return NULL. */
MD_ANSI_STYLE* md_ansi_style_create_named(const char* name,
                                          const MD_ANSI_STYLE_OPTS* opts);
size_t md_ansi_theme_count(void);
const char* md_ansi_theme_name(size_t index, int borderless);

/* Build a style from a YAML file (NULL path => built-in default config). */
MD_ANSI_STYLE* md_ansi_style_create_from_file(const char* path,
                                              const MD_ANSI_STYLE_OPTS* opts);

/* Build a style from an already-parsed libfyaml generic (a mapping with the
 * same schema as the YAML config). The generic is only read, never retained --
 * all strings are copied -- so the caller may free its builder afterwards.
 * Returns NULL on alloc failure or if `root` is not a valid generic. */
MD_ANSI_STYLE* md_ansi_style_create_from_generic(fy_generic root,
                                                 const MD_ANSI_STYLE_OPTS* opts);

void md_ansi_style_destroy(MD_ANSI_STYLE* s);

struct fypal_ctx;

/* Overlay the element pairs with the roles of a libfypalette context. An
 * element whose role the palette defines takes the escapes of the role; every
 * other element keeps the pair of its theme. A later call replaces the overlay
 * and a NULL palette removes it. The palette is borrowed and the escapes are
 * copied, so call again after the palette changes variant or capabilities.
 * Returns 0, or -1 on allocation failure or without palette support. */
int md_ansi_style_set_palette(MD_ANSI_STYLE* s, struct fypal_ctx* palette);

/* md_ansi_style_set_palette() that also takes the glyphs and the document
 * margin of the palette. A glyph the palette defines replaces the glyph of the
 * theme: the ASCII form when @ascii is non-zero, else the UTF-8 form. The
 * gutter.cols parameter sets the document margin. */
int md_ansi_style_set_palette_glyphs(MD_ANSI_STYLE* s, struct fypal_ctx* palette,
                                     int ascii);

/* The block renderer registered for the language @lang of @len bytes, or NULL. */
const MD_BLOCK_RENDERER* md_ansi_style_block_renderer(const MD_ANSI_STYLE* s,
                                                      const char* lang, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MD4C_STYLE_H */
