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

#ifdef __cplusplus
extern "C" {
#endif

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
    MD_STYLE_PAIR strong;
    MD_STYLE_PAIR emphasis;
    MD_STYLE_PAIR underline;
    MD_STYLE_PAIR strikethrough;
    MD_STYLE_PAIR code;          /* inline code */
    MD_STYLE_PAIR math;
    MD_STYLE_PAIR link;
    MD_STYLE_PAIR link_url;
    MD_STYLE_PAIR wikilink;
    MD_STYLE_PAIR blockquote;    /* quote bar styling */
    MD_STYLE_PAIR code_block;    /* plain (unhighlighted) fenced code */
    MD_STYLE_PAIR rule;          /* hr + code header/footer rules */
    MD_STYLE_PAIR table_header;
    MD_STYLE_PAIR table_header_row; /* styling spanning the complete header row */
    MD_STYLE_PAIR table_row_odd;    /* complete body rows, first body row is odd */
    MD_STYLE_PAIR table_row_even;
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
    int         table_border_none; /* true: no separator or vertical grid glyphs */

    MD_STYLE_BG  background;      /* resolved document background (never AUTO) */

    int          code_enabled;   /* fenced-code syntax highlighting on/off */
    const char*  code_theme;     /* libfyts styling name or path */
    MD_STYLE_BG  code_background;
    int          code_reverse;   /* fyts "reverse" bubble mode for fenced code */
    const char*  code_header;    /* decoration template; "default" => legacy rule */
    const char*  code_footer;    /* decoration template; "default" => legacy rule */
    const char*  code_prefix;    /* prefix placed before every fenced content row */

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

#ifdef __cplusplus
}
#endif

#endif /* MD4C_STYLE_H */
