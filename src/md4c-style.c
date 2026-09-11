/*
 * MD4C: Markdown parser for C
 * (http://github.com/mity/md4c)
 *
 * ANSI renderer styling configuration (YAML, via libfyaml).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfyaml/libfyaml-generic.h>

#include "md4c-style.h"

#ifdef MD4C_WITH_FYPALETTE
#include <libfypalette.h>
#endif

struct md4c_embedded_theme {
    const char* name;
    const char* borderless_name;
    const unsigned char* yaml;
    size_t yaml_len;
};

#include "md4c_default_style.inc"   /* MD4C_DEFAULT_STYLE[], MD4C_DEFAULT_STYLE_LEN */

/* ---- owned-string registry: every resolved string is strdup'd here so the
 *      parsed fy_generic builder can be torn down immediately ---- */

typedef struct {
    char** v;
    size_t n, cap;
} STRREG;

static const char*
reg_dup(STRREG* reg, const char* s)
{
    char* p = strdup(s != NULL ? s : "");
    if(p == NULL)
        return "";
    if(reg->n == reg->cap) {
        size_t nc = reg->cap ? reg->cap * 2 : 32;
        char** nv = (char**) realloc(reg->v, nc * sizeof(char*));
        if(nv == NULL) { free(p); return ""; }
        reg->v = nv;
        reg->cap = nc;
    }
    reg->v[reg->n++] = p;
    return p;
}

static void
reg_free(STRREG* reg)
{
    size_t i;
    for(i = 0; i < reg->n; i++)
        free(reg->v[i]);
    free(reg->v);
}

/* ---- YAML helpers ---- */

/* Resolve a style reference to its raw escape sequence: look the name up in the
 * `styles:` map (fy_get returns the value, or `ref` itself when absent -- so an
 * unknown name, e.g. an inline "\e[...", is used verbatim). */
static const char*
resolve_style(STRREG* reg, fy_generic styles, const char* ref)
{
    if(ref == NULL || ref[0] == '\0')
        return reg_dup(reg, "");
    return reg_dup(reg, fy_get(styles, ref, ref));
}

/* Load one element's on/off pair, layering a "light:" override over the base
 * on/off (or the built-in defaults). fy_get with a typed default returns the
 * value directly, and a nested fy_get supplies the fallback. */
static void
load_pair(STRREG* reg, fy_generic styles, fy_generic elements, const char* name,
          const char* def_on, const char* def_off, int light, MD_STYLE_PAIR* out)
{
    fy_generic el = fy_get(elements, name, fy_invalid);
    const char* base_on  = fy_get(el, "on",  def_on);
    const char* base_off = fy_get(el, "off", def_off);
    const char* ron  = base_on;
    const char* roff = base_off;

    if(light) {
        fy_generic lt = fy_get(el, "light", fy_invalid);
        ron  = fy_get(lt, "on",  base_on);
        roff = fy_get(lt, "off", base_off);
    }

    out->on  = resolve_style(reg, styles, ron);
    out->off = resolve_style(reg, styles, roff);
}

static const char*
load_str(STRREG* reg, fy_generic map, const char* key, const char* dflt)
{
    return reg_dup(reg, fy_get(map, key, dflt));
}

static void
load_indicators(MD_ANSI_STYLE* s, STRREG* reg, fy_generic indicators)
{
    fy_generic frames;
    fy_generic frame;
    size_t i, count;

    frames = fy_get(indicators, "pending", fy_invalid);
    count = fy_generic_is_sequence(frames) ? fy_len(frames) : 0;
    if(count > 8)
        count = 8;
    for(i = 0; i < count; i++) {
        frame = fy_get_at(frames, i);
        s->indicator_pending_frames[i] =
            reg_dup(reg, fy_castp(&frame, ""));
    }
    if(!count) {
        s->indicator_pending_frames[0] = reg_dup(reg, "●");
        s->indicator_pending_frames[1] = reg_dup(reg, " ");
        count = 2;
    }
    s->indicator_pending_frame_count = count;
    s->indicator_success_glyph =
        load_str(reg, indicators, "success", "●");
    s->indicator_failure_glyph =
        load_str(reg, indicators, "failure", "●");
    s->indicator_interval_ms =
        (unsigned int) fy_get(indicators, "interval_ms", (long) 500);
}

/* dark | light | auto -> concrete. auto consults $COLORFGBG (bg field >= 8 or
 * "default" -> light), else assumes dark. */
static MD_STYLE_BG
resolve_background(const char* mode)
{
    const char* fgbg;
    const char* semi;
    if(mode != NULL && strcmp(mode, "light") == 0)
        return MD_STYLE_BG_LIGHT;
    if(mode != NULL && strcmp(mode, "dark") == 0)
        return MD_STYLE_BG_DARK;
    /* auto */
    fgbg = getenv("COLORFGBG");
    if(fgbg != NULL && (semi = strrchr(fgbg, ';')) != NULL) {
        const char* bg = semi + 1;
        if(*bg >= '0' && *bg <= '9') {
            int v = atoi(bg);
            if(v == 7 || v == 15 || v >= 8)   /* light bg colour */
                return MD_STYLE_BG_LIGHT;
        }
    }
    return MD_STYLE_BG_DARK;
}

/* Populate an allocated style from a parsed config root. The root is only read
 * (all strings are copied into reg), never retained. */
static void
build_style(MD_ANSI_STYLE* s, STRREG* reg, fy_generic root, const MD_ANSI_STYLE_OPTS* opts)
{
    fy_generic elements, glyphs, code, decoration, table, styles, diff;
    fy_generic indicators;
    const char* bg;
    int light;

    elements = fy_get(root, "elements", fy_invalid);
    glyphs   = fy_get(root, "glyphs",   fy_invalid);
    code     = fy_get(root, "code",     fy_invalid);
    decoration = fy_get(code, "decoration", fy_invalid);
    diff     = fy_get(code, "diff",     fy_invalid);
    table    = fy_get(root, "table",    fy_invalid);
    styles   = fy_get(root, "styles",   fy_invalid);
    indicators = fy_get(root, "indicators", fy_invalid);

    if(opts != NULL && opts->background != MD_STYLE_BG_AUTO)
        s->background = opts->background;
    else
        s->background = resolve_background(fy_get(root, "background", "auto"));
    light = (s->background == MD_STYLE_BG_LIGHT);

#define LP(name, don, doff, field) \
    load_pair(reg, styles, elements, (name), (don), (doff), light, &s->field)
    LP("heading",       "\033[1;35m", "\033[0m",  heading);
    LP("strong",        "\033[1m",    "\033[22m", strong);
    LP("emphasis",      "\033[3m",    "\033[23m", emphasis);
    LP("underline",     "\033[4m",    "\033[24m", underline);
    LP("strikethrough", "\033[9m",    "\033[29m", strikethrough);
    LP("code",          "\033[36m",   "\033[39m", code);
    LP("math",          "\033[33m",   "\033[39m", math);
    LP("link",          "\033[4;34m", "\033[0m",  link);
    LP("link_url",      "\033[2;34m", "\033[0m",  link_url);
    LP("wikilink",      "\033[4;34m", "\033[0m",  wikilink);
    LP("blockquote",    "\033[2m",    "\033[22m", blockquote);
    LP("code_block",    "\033[2m",    "\033[22m", code_block);
    LP("rule",          "\033[2m",    "\033[22m", rule);
    LP("table_header",  "\033[1m",    "\033[22m", table_header);
    LP("table_header_row", "",         "",         table_header_row);
    LP("table_row_odd",    "",         "",         table_row_odd);
    LP("table_row_even",   "",         "",         table_row_even);
    /* Diff rows: background bands, so the "off" resets the background only.
     * A theme that does not name them still gets bands matching its background. */
    LP("diff_added",    light ? "\033[48;2;209;250;219m" : "\033[48;2;18;56;32m",
                        "\033[49m", diff_added);
    LP("diff_removed",  light ? "\033[48;2;255;215;215m" : "\033[48;2;74;24;26m",
                        "\033[49m", diff_removed);
    LP("diff_context",  "",                     "",         diff_context);
    LP("diff_hunk",     light ? "\033[48;2;219;229;255m\033[34m"
                              : "\033[48;2;28;38;58m\033[36m",
                        "\033[49m\033[39m", diff_hunk);
    LP("diff_file",     "\033[1m",    "\033[22m", diff_file);
    LP("diff_gutter",   "\033[2m",    "\033[22m", diff_gutter);
    LP("list_marker",   "\033[2m",    "\033[22m", list_marker);
    LP("task_done",     "\033[32m",   "\033[39m", task_done);
    /* Whole-document card background (mirrors libfyts' frame background). */
    LP("reverse",       "\033[7m",         "\033[0m", reverse);
    LP("indicator_pending", "\033[33m", "\033[39m", indicator_pending);
    LP("indicator_success", "\033[32m", "\033[39m", indicator_success);
    LP("indicator_failure", "\033[31m", "\033[39m", indicator_failure);
#undef LP
    load_indicators(s, reg, indicators);

    s->blockquote_bar   = load_str(reg, glyphs, "blockquote_bar",   "\xe2\x94\x82");
    s->list_bullet      = load_str(reg, glyphs, "list_bullet",      "*");
    s->table_vertical   = load_str(reg, glyphs, "table_vertical",   "\xe2\x94\x82");
    s->table_horizontal = load_str(reg, glyphs, "table_horizontal", "\xe2\x94\x80");
    s->table_cross      = load_str(reg, glyphs, "table_cross",      "\xe2\x94\xbc");
    s->table_border_none = strcmp(fy_get(table, "border", "grid"), "none") == 0;

    s->code_enabled = fy_get(code, "enabled", (_Bool) true);
    s->code_theme   = load_str(reg, code, "theme", "default");
    s->code_header  = load_str(reg, decoration, "header", "default");
    s->code_footer  = load_str(reg, decoration, "footer", "default");
    s->code_prefix  = load_str(reg, decoration, "prefix", "  ");
    s->code_marker  = load_str(reg, decoration, "marker", "\xe2\x8e\xbf  ");
    s->code_marker_enabled = fy_get(decoration, "marker_enabled", (_Bool) false);
    s->diff_enabled = fy_get(diff, "enabled", (_Bool) true);
    s->diff_line_numbers = fy_get(diff, "line_numbers", (_Bool) true);
    s->diff_inner_highlight = fy_get(diff, "inner_highlight", (_Bool) true);
    s->diff_gutter_sep = load_str(reg, diff, "gutter", "\xe2\x94\x82");
    bg = fy_get(code, "background", "auto");
    if(strcmp(bg, "dark") == 0)        s->code_background = MD_STYLE_BG_DARK;
    else if(strcmp(bg, "light") == 0)  s->code_background = MD_STYLE_BG_LIGHT;
    else                               s->code_background = s->background;  /* auto: follow doc */

    if(opts != NULL && opts->reverse >= 0)
        s->code_reverse = opts->reverse;
    else
        s->code_reverse = fy_get(code, "reverse", (_Bool) false);
}

/* Allocate a style + its owned-string registry. Returns NULL on alloc failure. */
static MD_ANSI_STYLE*
style_alloc(void)
{
    MD_ANSI_STYLE* s = (MD_ANSI_STYLE*) calloc(1, sizeof(*s));
    STRREG* reg;
    if(s == NULL)
        return NULL;
    reg = (STRREG*) calloc(1, sizeof(*reg));
    if(reg == NULL) { free(s); return NULL; }
    s->_owned = reg;
    return s;
}

MD_ANSI_STYLE*
md_ansi_style_create(const char* yaml, size_t yaml_len, const MD_ANSI_STYLE_OPTS* opts)
{
    MD_ANSI_STYLE* s;
    struct fy_generic_builder* gb;
    fy_generic root;
    fy_generic_sized_string src;

    s = style_alloc();
    if(s == NULL)
        return NULL;

    if(yaml == NULL || yaml_len == 0) {
        src.data = (const char*) MD4C_DEFAULT_STYLE;
        src.size = MD4C_DEFAULT_STYLE_LEN;
    } else {
        src.data = yaml;
        src.size = yaml_len;
    }

    gb = fy_generic_builder_create(NULL);
    if(gb == NULL) { md_ansi_style_destroy(s); return NULL; }
    root = fy_parse(gb, src, FYOPPF_DEFAULT, NULL);
    if(!fy_generic_is_valid(root)) {
        fy_generic_builder_destroy(gb);
        md_ansi_style_destroy(s);
        return NULL;
    }

    build_style(s, (STRREG*) s->_owned, root, opts);

    fy_generic_builder_destroy(gb);
    return s;
}

MD_ANSI_STYLE*
md_ansi_style_create_named(const char* name, const MD_ANSI_STYLE_OPTS* opts)
{
    static const char suffix[] = "-borderless";
    const unsigned char* yaml;
    size_t yaml_len;
    size_t name_len, suffix_len;
    char base[32];
    MD_ANSI_STYLE* style;
    size_t i;

    if(name == NULL || name[0] == '\0' || strcmp(name, "default") == 0)
        return md_ansi_style_create(NULL, 0, opts);
    name_len = strlen(name);
    suffix_len = sizeof(suffix) - 1;
    if(name_len > suffix_len && name_len - suffix_len < sizeof(base) &&
       strcmp(name + name_len - suffix_len, suffix) == 0) {
        memcpy(base, name, name_len - suffix_len);
        base[name_len - suffix_len] = '\0';
        style = md_ansi_style_create_named(base, opts);
        if(style != NULL && strcmp(base, "default") != 0)
            style->table_border_none = 1;
        else if(style != NULL) {
            md_ansi_style_destroy(style);
            style = NULL;
        }
        return style;
    }
    for(i = 0; i < MD4C_THEME_COUNT; i++) {
        if(strcmp(name, MD4C_THEMES[i].name) == 0)
            break;
    }
    if(i == MD4C_THEME_COUNT)
        return NULL;
    yaml = MD4C_THEMES[i].yaml;
    yaml_len = MD4C_THEMES[i].yaml_len;
    return md_ansi_style_create((const char*)yaml, yaml_len, opts);
}

size_t
md_ansi_theme_count(void)
{
    return MD4C_THEME_COUNT;
}

const char*
md_ansi_theme_name(size_t index, int borderless)
{
    if(index >= MD4C_THEME_COUNT)
        return NULL;
    return borderless ? MD4C_THEMES[index].borderless_name
                      : MD4C_THEMES[index].name;
}

MD_ANSI_STYLE*
md_ansi_style_create_from_generic(fy_generic root, const MD_ANSI_STYLE_OPTS* opts)
{
    MD_ANSI_STYLE* s;

    if(!fy_generic_is_valid(root))
        return NULL;

    s = style_alloc();
    if(s == NULL)
        return NULL;

    build_style(s, (STRREG*) s->_owned, root, opts);
    return s;
}

MD_ANSI_STYLE*
md_ansi_style_create_from_file(const char* path, const MD_ANSI_STYLE_OPTS* opts)
{
    FILE* f;
    char* buf;
    long n;
    MD_ANSI_STYLE* s;

    if(path == NULL)
        return md_ansi_style_create(NULL, 0, opts);

    f = fopen(path, "rb");
    if(f == NULL)
        return NULL;
    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if(n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char*) malloc((size_t) n + 1);
    if(buf == NULL) { fclose(f); return NULL; }
    if(fread(buf, 1, (size_t) n, f) != (size_t) n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = '\0';

    s = md_ansi_style_create(buf, (size_t) n, opts);
    free(buf);
    return s;
}

/* ---- palette overlay ---- */

/* The palette role of each element pair. A role query answers with the
 * nearest defined ancestor. An exact entry takes only the role itself: the
 * ancestor of a row or a card styles another extent than the pair does. */
static const struct {
    size_t offset;
    const char* role;
    int exact;
} palette_pairs[] = {
    { offsetof(MD_ANSI_STYLE, heading),           "md.heading",          0 },
    { offsetof(MD_ANSI_STYLE, strong),            "md.strong",           0 },
    { offsetof(MD_ANSI_STYLE, emphasis),          "md.emphasis",         0 },
    { offsetof(MD_ANSI_STYLE, underline),         "md.underline",        0 },
    { offsetof(MD_ANSI_STYLE, strikethrough),     "md.strike",           0 },
    { offsetof(MD_ANSI_STYLE, code),              "md.code",             0 },
    { offsetof(MD_ANSI_STYLE, math),              "md.math",             0 },
    { offsetof(MD_ANSI_STYLE, link),              "md.link",             0 },
    { offsetof(MD_ANSI_STYLE, link_url),          "md.link.url",         0 },
    { offsetof(MD_ANSI_STYLE, wikilink),          "md.link.wiki",        0 },
    { offsetof(MD_ANSI_STYLE, blockquote),        "md.quote.bar",        0 },
    { offsetof(MD_ANSI_STYLE, code_block),        "code.plain",          0 },
    { offsetof(MD_ANSI_STYLE, rule),              "md.rule",             0 },
    { offsetof(MD_ANSI_STYLE, table_header),      "md.table.header",     0 },
    { offsetof(MD_ANSI_STYLE, table_header_row),  "md.table.header.row", 1 },
    { offsetof(MD_ANSI_STYLE, table_row_odd),     "md.table.row.odd",    1 },
    { offsetof(MD_ANSI_STYLE, table_row_even),    "md.table.row.even",   1 },
    { offsetof(MD_ANSI_STYLE, diff_added),        "diff.add",            1 },
    { offsetof(MD_ANSI_STYLE, diff_removed),      "diff.del",            1 },
    { offsetof(MD_ANSI_STYLE, diff_context),      "diff.context",        1 },
    { offsetof(MD_ANSI_STYLE, diff_hunk),         "diff.hunk",           1 },
    { offsetof(MD_ANSI_STYLE, diff_file),         "diff.file",           1 },
    { offsetof(MD_ANSI_STYLE, diff_gutter),       "diff.lineno",         1 },
    { offsetof(MD_ANSI_STYLE, list_marker),       "md.bullet",           0 },
    { offsetof(MD_ANSI_STYLE, task_done),         "md.task.done",        0 },
    { offsetof(MD_ANSI_STYLE, reverse),           "md.card",             1 },
    { offsetof(MD_ANSI_STYLE, indicator_pending), "tool.pending",        0 },
    { offsetof(MD_ANSI_STYLE, indicator_success), "tool.ok",             0 },
    { offsetof(MD_ANSI_STYLE, indicator_failure), "tool.fail",           0 },
};

#define PALETTE_PAIR_COUNT (sizeof(palette_pairs) / sizeof(palette_pairs[0]))

/* The theme pairs the overlay replaced, and the strings it owns. */
typedef struct {
    MD_STYLE_PAIR saved[PALETTE_PAIR_COUNT];
    STRREG reg;
} PALETTE_OVERLAY;

static MD_STYLE_PAIR*
palette_pair(MD_ANSI_STYLE* s, size_t i)
{
    return (MD_STYLE_PAIR*) ((char*) s + palette_pairs[i].offset);
}

static void
palette_overlay_remove(MD_ANSI_STYLE* s)
{
    PALETTE_OVERLAY* ov = (PALETTE_OVERLAY*) s->_palette;
    size_t i;

    if(ov == NULL)
        return;
    for(i = 0; i < PALETTE_PAIR_COUNT; i++)
        *palette_pair(s, i) = ov->saved[i];
    reg_free(&ov->reg);
    free(ov);
    s->_palette = NULL;
    s->palette = NULL;
}

int
md_ansi_style_set_palette(MD_ANSI_STYLE* s, struct fypal_ctx* palette)
{
#ifdef MD4C_WITH_FYPALETTE
    const struct fypal_role* role;
    PALETTE_OVERLAY* ov;
    MD_STYLE_PAIR* pair;
    size_t i;

    if(s == NULL)
        return -1;
    palette_overlay_remove(s);
    if(palette == NULL)
        return 0;
    ov = (PALETTE_OVERLAY*) calloc(1, sizeof(*ov));
    if(ov == NULL)
        return -1;
    for(i = 0; i < PALETTE_PAIR_COUNT; i++) {
        pair = palette_pair(s, i);
        ov->saved[i] = *pair;
        role = fypal_ctx_role(palette, palette_pairs[i].role);
        if(role == NULL)
            continue;
        if(palette_pairs[i].exact &&
           strcmp(fypal_role_name(role), palette_pairs[i].role) != 0)
            continue;
        pair->on = reg_dup(&ov->reg, fypal_role_on(palette, role));
        pair->off = reg_dup(&ov->reg, fypal_role_off(palette, role));
    }
    s->_palette = ov;
    s->palette = palette;
    return 0;
#else
    if(s == NULL)
        return -1;
    palette_overlay_remove(s);
    return palette == NULL ? 0 : -1;
#endif
}

void
md_ansi_style_destroy(MD_ANSI_STYLE* s)
{
    if(s == NULL)
        return;
    palette_overlay_remove(s);
    if(s->_owned != NULL) {
        reg_free((STRREG*) s->_owned);
        free(s->_owned);
    }
    free(s);
}
