# Renderers (fork additions)

This fork adds an ANSI terminal renderer and a Markdown heal utility, plus a
streaming front-end for the renderer. All are thin modules over the md4c parser's
public SAX API (`md4c.h`).

For consumers, everything is compiled into a single public, opaque-typed library
— **`libfymd4c`** — described first below, and driven by the single **`fymd4c`**
CLI (`-t ansi` default, `-t html`, `-t heal`). There are no standalone
`md4c-ansi` / `md4c-heal` / `md4c-stream` libraries; the sections that follow
document the internal modules `libfymd4c` compiles in.

## Public library (`libfymd4c.h`)

`libfymd4c` is the recommended entry point: a single library, modeled on
[libfyaml](https://github.com/pantoniou/libfyaml)'s API conventions (`fymd_` /
`FYMD_` symbols, opaque `struct` handles, `cfg`+flag-enum constructors). One
opaque `struct fymd_renderer`, built from a `struct fymd_renderer_cfg`, drives
both one-shot rendering and progressive, self-healing streaming with
syntax-highlighted fenced code. The styling and stream internals are hidden.

```c
#include <libfymd4c.h>

struct fymd_renderer_cfg cfg;
memset(&cfg, 0, sizeof cfg);
cfg.flags = FYMD_RF_DEFAULT;          /* = FYMD_RF_HEAL */
cfg.width = FYMD_WIDTH_AUTO;          /* AUTO(-1) / INF(0) / columns */
cfg.background = FYMD_BG_AUTO;        /* AUTO / DARK / LIGHT */
/* optional: cfg.style / cfg.style_path (YAML), cfg.parser_flags,
 *           cfg.max_active_lines, cfg.code_theme, cfg.userdata */

struct fymd_renderer *r = fymd_renderer_create(&cfg);   /* NULL cfg => all defaults */
```

### Styling source

The theme comes from a YAML config (same schema as `md4c-style`, below). The cfg
offers three ways to supply it, highest precedence first:

```c
fy_generic   style_generic;  /* an already-parsed libfyaml mapping */
const char  *style_path;     /* a YAML file path */
const char  *style;          /* inline YAML text */
```

If none is set, the built-in default theme is used. The `style_generic` form
makes libfyaml an explicit, first-class dependency: a caller already building
config as a libfyaml `fy_generic` (the same generic API `md4c-style` uses
internally) can hand it straight to the renderer without re-serializing to text.
The generic is only read (all strings are copied), so the caller may free its
builder immediately after `fymd_renderer_create()`.

### Block renderers

A fenced block of a Markdown document can be drawn by the application instead
of as code. Register a renderer for the language of its info string:

```c
static int draw(void *userdata, const char *lang, const char *text, size_t len,
                int width, enum fymd_block_flags flags,
                fymd_block_emit_fn emit, void *emit_ctx)
{
    /* draw text[0..len) in at most `width` columns (0 is unlimited) */
    emit(emit_ctx, rows, rows_len);   /* rows separated by '\n' */
    return 0;                         /* or -1: render the block as code */
}

fymd_renderer_set_block_renderer(r, "mermaid", draw, userdata);
```

The renderer lays out each emitted row under the indent of the block, with no
code rule above or below it. A renderer that returns -1 declines: its output is
discarded and the block renders as code, header and all. `FYMD_BF_NO_COLOR`
says that the render has no colour.

A block renderer is not progressive. It is called once, for a closed fence of
a Markdown document, and never for:

- a diff or patch block, which keeps its own rendering;
- `fymd_render_fenced()`, which has no Markdown document; or
- a fence that a progressive stream has not closed yet. The stream renders it
  as code, commits none of its interior lines, and draws it when it closes, so
  the streamed output stays identical to the one-shot render.

The language matches exactly and has 1 to 63 bytes. A later registration for the
language replaces the renderer; a NULL function removes it.
`fymd_renderer_set_theme()` keeps the renderers. The call is rejected while a
progressive stream exists.

### Palette styling

When libfymd4c is built with libfypalette, a renderer can take its colours
from a palette context in addition to its theme:

```c
struct fypal_ctx *palette = fypal_ctx_create(&caps);

fypal_ctx_load_builtin(palette, "ember");
fymd_renderer_set_palette(r, palette);
```

Each element whose role the palette defines takes the escapes of that role.
Every other element keeps the pair of its theme. The roles are:

| element | role | element | role |
|---|---|---|---|
| heading | `md.heading` | table header | `md.table.header` |
| strong | `md.strong` | table header row | `md.table.header.row` * |
| emphasis | `md.emphasis` | odd / even rows | `md.table.row.odd` / `.even` * |
| underline | `md.underline` | diff added / removed | `diff.add` / `diff.del` * |
| strikethrough | `md.strike` | diff context | `diff.context` * |
| inline code | `md.code` | diff hunk / file | `diff.hunk` / `diff.file` * |
| math | `md.math` | diff line numbers | `diff.lineno` * |
| link / URL | `md.link` / `md.link.url` | list marker | `md.bullet` |
| wiki link | `md.link.wiki` | done task | `md.task.done` |
| blockquote bar | `md.quote.bar` | document card | `md.card` * |
| plain fenced code | `code.plain` | indicators | `tool.pending` / `tool.ok` / `tool.fail` |
| rules | `md.rule` | | |

A heading of level N takes `md.heading.N`, which answers with `md.heading`
when the theme does not define the level. A YAML theme sets a level with the
element `headingN`; a level it does not name uses `heading`.

A role query answers with the nearest defined ancestor, so `md.link.wiki`
takes `md.link` when the theme does not define it. A role marked * must be
defined itself: the ancestor of a row or a card styles another extent.

Fenced code is highlighted through the `code.*` roles when the libfyts in use
supports a palette; see the libfyts documentation for the capture roles.
`fymd_renderer_get_style_pair()` and `fymd_renderer_get_indicator()` return the
palette pairs, so an application that draws its own chrome uses the same
colours as the document.

The palette is borrowed and must stay alive while the renderer uses it. The
escapes are copied when the palette is set, so set it again after the palette
changes its variant or capabilities. `fymd_renderer_set_theme()` keeps the
palette. `NULL` returns to the theme. The call is rejected while a progressive
stream exists and when the library is built without libfypalette.

The build uses libfypalette when it finds the package: `-DMD4C_FYPALETTE=on`
makes it required and `-DMD4C_FYPALETTE=off` disables it. `fymd4c
--palette=ember` renders with a palette theme.

```c

/* One-shot. */
char *out = fymd_render_to_string(r, md, len);          /* free with fymd_free() */
/* or: int fymd_render(r, md, len, &out, &out_len);     */

/* Progressive + healing stream (one live stream per handle). */
struct fymd_update upd;
fymd_render_push(r, chunk, n, &upd);    /* apply: up `backtrack`, clear, print `content` */
fymd_render_finish(r, &final, &flen);   /* renderer-owned, valid until next call */
fymd_render_reset(r);                   /* drop stream state to start another */

fymd_renderer_destroy(r);
```

### Raw fenced blocks

Raw text can use the fenced-code presentation and syntax-highlighting pipeline
without first being encoded as Markdown:

```c
struct fymd_fenced_block_opts block = {
    .language = "c",
    .flags = FYMD_FBF_STYLE | FYMD_FBF_HIGHLIGHT,
    .template_vars = variables, /* optional borrowed fy_generic mapping */
};
char *out;
size_t out_len;
fymd_render_fenced_block(r, source, source_len, &block, &out, &out_len);
fymd_free(out);
```

`FYMD_FBF_STYLE` enables the theme's current header/footer rules, two-column
code inset, plain-code styling and reverse bubble behavior. Without it, only
the raw code rows are emitted. `FYMD_FBF_HIGHLIGHT` independently requests
libfyts highlighting when `language` is supported. Passing `NULL` options uses
both flags with no language. Input escape filtering and rendered-row limits are
the same as for Markdown rendering; the text itself is never parsed as Markdown.
Decoration templates substitute arbitrary `{key}` placeholders from
`template_vars`. The mapping is borrowed for the render call. Caller values
override renderer context values; the default context provides `language`,
`rule`, and width-aware `fill` values.

### Rendered-row limits

A renderer can optionally expose a bounded terminal viewport. Configure it
before one-shot rendering or before the first progressive push:

```c
struct fymd_line_limit_opts limit = {
    .mode = FYMD_LLM_HEAD_TAIL,       /* or FYMD_LLM_SCROLL */
    .max_lines = 20,
    .split = FYMD_LLS_BALANCED,       /* extra retained row goes to the tail */
    .separator_format = "... %d lines omitted ...",
};
fymd_renderer_set_line_limit(r, &limit);
```

Rows are counted after wrapping and layout. Short output is unchanged and is
not padded. Scroll mode retains the newest rows. Head-tail mode reserves one
row for the separator; `FYMD_LLS_HEAD_COUNT` uses `head_lines` and gives the
remaining rows to the tail. The separator accepts exactly one `%d` (the number
of omitted rendered rows) and `%%` for a literal percent sign. Pass `NULL`, use
`FYMD_LLM_NONE`, or set `max_lines` to zero to disable the viewport.

With progressive updates, the bounded viewport remains mutable (`freeze` is
zero) so rows can scroll out. Apply updates normally; the final flush replaces
the active viewport. Call `fymd_render_reset()` before changing the limit of a
renderer that has started a stream.

The CLI equivalents are `--max-lines`, `--line-overflow=scroll|head-tail`,
`--line-head=N|balanced`, and `--line-separator=FORMAT`.

The CLI's `--language=LANG` renders its entire input through the raw fenced-block
API; `--language=auto` detects the language from the input filename through the
same libfyts catalogue. It supports one-shot and progressive streaming modes,
including rendered-row limits. With stdin or an unknown extension, automatic
detection produces an unlabeled, unhighlighted fence.
`--fence-style=off` suppresses the header, footer, content inset and plain-code
theme styling while leaving syntax highlighting enabled; it applies equally to
one-shot and progressive language rendering.

### Config flags (`FYMD_RF_*`)

| Flag                   | Description                                       |
| ---------------------- | ------------------------------------------------- |
| `FYMD_RF_NO_COLOR`     | Emit no SGR color sequences                       |
| `FYMD_RF_SHOW_URLS`    | Show link targets inline                          |
| `FYMD_RF_TABLE_FIT`    | Size tables to content instead of filling width   |
| `FYMD_RF_HEAL`         | Close dangling markers in the active/in-progress tail |
| `FYMD_RF_REVERSE`      | Render the whole document as a card (theme background filled to width) |
| `FYMD_RF_NO_CODE_HL`   | Disable fenced-code syntax highlighting           |
| `FYMD_RF_NO_DIFF`      | Render ```` ```diff ```` blocks as ordinary code instead of a GitHub-like diff |
| `FYMD_RF_NO_DIFF_LINES`| No line-number gutter in diff blocks              |
| `FYMD_RF_NO_DIFF_HL`   | Do not highlight diff content as the patched file's language |
| `FYMD_RF_CODE_MARKER`  | Switch the styling's fenced first-row marker on for this render |
| `FYMD_RF_NO_CODE_MARKER` | ... or off                                      |

`FYMD_RF_DEFAULT` is `FYMD_RF_HEAL`. `fymd_renderer_get_cfg()` returns the
renderer's owned copy of the cfg; `fymd_detect_width()` resolves the auto width;
`fymd_library_version()` returns the version string.

### ABI / packaging

The parser, entity table, HTML renderer, heal, ANSI renderer, styling and stream
code are all compiled directly into `libfymd4c` with hidden visibility (and
bundled-static `libfyts`/tree-sitter symbols hidden), so the **only exported
symbols are the `fymd_*` API**. Nothing is built as a separate library, so a
static build has no duplicate symbols. `libfyaml` is the sole runtime dependency
and a public one (the header includes `<libfyaml/libfyaml-generic.h>` for the
`style_generic` field).

Built in **two flavours from the same sources** in a single configure (mirroring
libfyaml / libfyts):

- `libfymd4c::libfymd4c` — the primary library, shared by default. The shared
  object absorbs the static libfyts + tree-sitter, so it is self-contained: its
  only NEEDED dependency is `libfyaml` (+ libc).
- `libfymd4c::libfymd4c_static` — a plain static archive (`libfymd4c.a`). An
  archive can't absorb another archive, so it **declares** its dependencies
  (libfyaml, and libfyts when built against the installed package) through the
  CMake package's `find_dependency()`; a consumer linking it pulls those in too.

Ships a soname (`libfymd4c.so.N` from `.libtool-version`), a `libfymd4c.pc`
(`Requires: libfyaml`), and a CMake package: `find_package(libfymd4c)` exposes
both targets (raw export names `libfymd4c::fymd4c` / `::fymd4c_static`, plus the
`libfymd4c::libfymd4c` / `::libfymd4c_static` aliases). `-DBUILD_SHARED_LIBS=OFF`
makes the primary target static too. libfyts is selected by `MD4C_FYTS_PROVIDER`
(`auto`/`system`/`fetch`), always linking its static target.

```sh
cc app.c $(pkg-config --cflags --libs libfymd4c) -o app       # shared
# or, via CMake: target_link_libraries(app PRIVATE libfymd4c::libfymd4c_static)
```

### Stateless conversions (`libfymd4c-convert.h`)

Besides the renderer handle, the library exposes two stateless conversions that
don't need a `fymd_renderer`. Results are heap-allocated and freed with
`fymd_free()`.

```c
/* Markdown -> HTML (md4c's HTML renderer; parser_flags are md4c MD_FLAG_*,
 * 0 => CommonMark; flags are FYMD_HTML_XHTML/VERBATIM_ENTITIES/SKIP_UTF8_BOM). */
int   fymd_render_html(md, len, parser_flags, flags, &out, &out_len);
char *fymd_render_html_to_string(md, len, parser_flags, flags);

/* Heal incomplete / mid-stream Markdown into well-formed Markdown. */
int   fymd_heal(md, len, &out, &out_len);
char *fymd_heal_to_string(md, len);
```

These back the `fymd4c -t html` and `-t heal` CLI formats.

## Inline reader (`libfymd4c-inline.h`)

`fymd_inline_parse()` reads a short text as CommonMark and reports its inline
content as a sequence of runs, each carrying the attributes that apply to it:

```c
struct fymd_inline *inl = fymd_inline_parse(label, FYMD_NT,
                                            FYMD_IF_STRIKETHROUGH);
size_t i;

for (i = 0; i < fymd_inline_count(inl); i++) {
        const struct fymd_inline_run *run = fymd_inline_get(inl, i);

        /* run->text, run->attrs (FYMD_IA_STRONG, ...), run->href */
}
fymd_inline_destroy(inl);
```

This is the piece a caller wants when it draws the text itself — a label in a
diagram, a cell in a table — and needs to know which part of it is bold,
rather than a rendered document. `libfymermaid` uses it for mermaid's markdown
strings.

Block structure is consumed and not reported: a heading or a list marker is
dropped and its content reported as runs, and an indented line is not taken
for a code block. A soft line break becomes a space; a hard one is a run
carrying `FYMD_IA_BREAK`. `fymd_inline_plain()` joins the runs, which is the
text with its markup removed.

## ANSI Renderer (`md4c-ansi.h`)

Renders Markdown into ANSI terminal output with escape codes for styling.

```c
int md_ansi(const MD_CHAR* input, MD_SIZE input_size,
            void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
            void* userdata, unsigned parser_flags, unsigned renderer_flags);

/* Extended: explicit table/wrap width. */
int md_ansi_ex(const MD_CHAR* input, MD_SIZE input_size,
               void (*process_output)(const MD_CHAR*, MD_SIZE, void*),
               void* userdata, unsigned parser_flags, unsigned renderer_flags,
               int width);

int md_ansi_detect_width(void);   /* $COLUMNS / terminal / 80 */
```

`MD4C_ANSI_PARSER_FLAGS` is the default parser flag set (tables, strikethrough,
task lists, LaTeX math, wiki links, underline, permissive autolinks) — the md4c
extensions the renderer knows how to display. The `width` argument: `> 0` fixed,
`MD_ANSI_WIDTH_INF` (`0`) unlimited (no wrapping), `MD_ANSI_WIDTH_AUTO` (`-1`)
auto-detect. `md_ansi()` is `md_ansi_ex()` with `AUTO`.

### Renderer flags (`MD_ANSI_FLAG_*`)

| Flag                         | Value    | Description                                   |
| ---------------------------- | -------- | --------------------------------------------- |
| `MD_ANSI_FLAG_DEBUG`         | `0x0001` | Send debug output from md_parse() to stderr   |
| `MD_ANSI_FLAG_SKIP_UTF8_BOM` | `0x0002` | Skip a UTF-8 BOM at input start               |
| `MD_ANSI_FLAG_NO_COLOR`      | `0x0004` | Suppress color escapes (plain styled text)    |
| `MD_ANSI_FLAG_CODE_META`     | `0x0008` | Append code-block metadata after a NUL byte   |
| `MD_ANSI_FLAG_SHOW_URLS`     | `0x0010` | Show link URLs after link text (default OSC 8)|
| `MD_ANSI_FLAG_TABLE_FIT_CONTENT` | `0x0020` | Size tables to content (grow to width), don't fill |
| `MD_ANSI_FLAG_HEAL`          | `0x0100` | Heal the input before rendering (see below)   |

### Rendering details

- Headings bold magenta; bold/italic/underline/strikethrough as expected; inline
  code cyan; links underline-blue with OSC 8 hyperlinks; blockquotes a dim `│`
  bar; lists with bullet/number prefixes; task lists `[x]`/`[ ]`; horizontal
  rules and code blocks dim; entities resolved to UTF-8; raw HTML stripped.
- **Tables** are laid out glow-style: content-sized columns with Unicode box
  separators (`│ ┼ ─`), a header separator, per-column alignment, 1-space cell
  padding, shrink with cell word-wrap when too wide. By default narrow tables
  expand to fill the width; `MD_ANSI_FLAG_TABLE_FIT_CONTENT` (CLI
  `--table-size=fit`) instead sizes columns to their content, growing only up to
  the width and then shrinking/wrapping to fit.
- **Word-wrapping**: all text is wrapped to the width with a symmetric 2-column
  document margin; code blocks are left preformatted; `--width=inf` disables it.
- **Display widths** use a Markus-Kuhn-style `wcwidth` table (zero-width
  combining/format marks; East Asian Wide/Fullwidth and most emoji = 2 cols).

md4c block/span types the renderer does not display (footnotes, admonitions,
highlight, sub/superscript, spoilers — not enabled by `MD4C_ANSI_PARSER_FLAGS`)
fall through harmlessly.

## Streaming / Push API (`md4c-stream.h`)

A push-mode front-end over `md_ansi_ex()`, for live terminal output. The parser
is one-shot, so the context accumulates input, re-renders only the **active
region** since the last "safe sync point" (a blank line where all block
containers are closed), and emits the stable prefix.

```c
MD4C_STREAM* md4c_stream_create(const MD4C_STREAM_OPTS* opts);   /* NULL = defaults */
void         md4c_stream_destroy(MD4C_STREAM* s);

int md4c_stream_push(MD4C_STREAM* s, const char* chunk, size_t len,
                     const char** out, size_t* out_len);   /* committed output */
int md4c_stream_preview(MD4C_STREAM* s, const char** out, size_t* out_len); /* healed active region */
int md4c_stream_finish(MD4C_STREAM* s, const char** out, size_t* out_len);  /* final remainder */
```

With healing off, the concatenation of all `push` outputs plus `finish` is
byte-identical to a one-shot `md_ansi` render. (The one theoretical exception is
a CommonMark link reference definition appearing later in the stream.)

### Progressive updates (`md4c_stream_render`)

For a terminal that updates the active region in place, returns a line-diff
instead of append-only output:

```c
typedef struct MD4C_STREAM_UPDATE {
    size_t backtrack;     /* trailing active-region lines to erase upward */
    const char* content;  /* replacement text (newline-terminated lines) */
    size_t content_len;
    size_t freeze;        /* lines at the top now permanent */
} MD4C_STREAM_UPDATE;

int md4c_stream_render(MD4C_STREAM* s, const char* chunk, size_t len, MD4C_STREAM_UPDATE* upd);
```

Apply it as: move the cursor up `backtrack` lines, clear to end of screen, print
`content`. Usually only the last line or two change; a table reflow may change
the whole active region. `freeze` advances the permanent boundary (future
`backtrack` never exceeds the still-mutable line count). The active region is
rendered per `opts.heal`; committed lines always use the unhealed truth.

The CLI drives this via `--format=ansi --stream-progressive` (cursor control on a
terminal; final reconstructed screen otherwise). See `test/stream-demo.sh`.

### Bounding the active region (`max_active_lines`)

Each push re-renders the whole active region (the input since the last safe sync
point), and the heal scan covers the same span. A single block that never reaches
a sync point — a table or list taller than the screen, a long unbroken paragraph
— makes the active region grow without bound, so per-push cost (and heal cost)
grows linearly and the total goes quadratic.

`MD4C_STREAM_OPTS.max_active_lines` (CLI `--max-active-lines=N`, `0` = unlimited)
caps it: once the active region exceeds `N` input lines the oldest excess lines
are force-committed at a line boundary, so the active region — and thus every
re-render and heal scan — stays `O(N)`. Set it to the terminal row count: content
scrolled above the viewport can't reflow anyway, so freezing it costs nothing
visible. A single newline-free line over roughly `N × width` bytes is split on a
whitespace boundary as a byte-budget backstop. The force-cut is not a safe sync
point, so a construct straddling it (a table taller than `N`) freezes mid-way —
the accepted trade for a bounded cost. With `max_active_lines = 0` (default)
behavior is unchanged and the stream stays byte-identical to one-shot.

## Styling config (`md4c-style.h`)

All of the renderer's ANSI sequences and glyphs are defined in a YAML document
(parsed with libfyaml), mirroring the libfyts styling pattern. The built-in
default (`stylings/md4c-default.yaml`, embedded at build time) reproduces the
historical hardcoded look; `--style=FILE` (CLI) or `md_ansi_style_*` (library)
overrides any subset.

Focused partial themes for ASCII, heavy Unicode, colored-header, and visually
borderless tables are available under `stylings/examples/`.

```c
typedef struct { MD_STYLE_BG background; int reverse; } MD_ANSI_STYLE_OPTS;
MD_ANSI_STYLE* md_ansi_style_create(const char* yaml, size_t len,
                                    const MD_ANSI_STYLE_OPTS* opts);   /* NULL,0 => default config */
MD_ANSI_STYLE* md_ansi_style_create_from_file(const char* path,
                                              const MD_ANSI_STYLE_OPTS* opts);
void           md_ansi_style_destroy(MD_ANSI_STYLE* s);
```

There is no global style state: build one `MD_ANSI_STYLE` and pass it to the
renderer (`md_ansi_ex_styled(..., style)`) or the stream context
(`MD4C_STREAM_OPTS.style`). `md_ansi_ex()` with no style builds and frees the
built-in default for that one call. `MD_ANSI_STYLE_OPTS` overrides the config's
`background:`/`code.reverse` (`MD_STYLE_BG_AUTO` / `reverse < 0` = inherit).

Schema (mirrors libfyts: named `styles:` hold the raw escapes, `elements:`
reference them by name). Every key is optional; missing keys keep the built-in
default. An on/off value that is not a known style name is used verbatim, so an
inline `"\e[..."` still works.

```yaml
styles:              # the only place raw escapes live (\e and \uXXXX honoured)
  bold: "\e[1m"   bold-off: "\e[22m"   cyan: "\e[36m"   blue: "\e[34m" ...
elements:            # each: { on: <style>, off: <style>, light: { on, off } }
  heading: { on: heading, off: reset }
  strong: ...  emphasis: ...  underline: ...  strikethrough: ...
  code: { on: cyan, off: default-fg, light: { on: blue, off: default-fg } }
  math: ...  link: ...  link_url: ...  wikilink: ...  blockquote: ...
  code_block: ...  rule: ...  table_header: ...  list_marker: ...  task_done: ...
  table_header_row: ...  table_row_odd: ...  table_row_even: ...
glyphs:
  blockquote_bar: "│"   list_bullet: "*"
  table_vertical: "│"   table_horizontal: "─"   table_cross: "┼"
background: auto     # dark | light | auto (auto consults $COLORFGBG, else dark)
table:
  border: grid       # grid | none
code:                # fenced-code highlighting (libfyts)
  enabled: true
  theme: default     # embedded "default", or a path to a libfyts styling YAML
  background: auto    # auto follows the document background
  decoration:
    header: default  # empty suppresses; templates substitute arbitrary {key} values
    footer: default
    prefix: "  "     # prefix before every fenced content row
    marker: "⎿  "    # prefix of the FIRST row only, the rest indented to it
    marker_enabled: false  # switched on here, or per render
  diff:              # GitHub-like ```diff / ```patch rendering
    enabled: true
    line_numbers: true      # new-side gutter (blank on removed lines)
    inner_highlight: true   # highlight rows as the patched file's language
    gutter: "│"
```

The `light:` sub-map of an element overrides its `on`/`off` on a light
background; `background:` (or CLI `--background=dark|light|auto`) selects which
variant is used. Specific "off" codes (e.g. `bold-off` = `\e[22m`) rather than a
blanket reset keep nested styling intact. The `code:` block configures the
libfyts highlighter; the language catalogue is the build-time
`MD4C_FYTS_CATALOGUE` choice.

`code.decoration.marker` gives a fenced block a hanging-indent shape instead of
header/footer rules: the marker prefixes the **first** content row and the
remaining rows are indented to its display width. With the rules suppressed
(`header: ""`, `footer: ""`):

```
  ⎿  int main(int argc, char **argv)
     {
       return 0;
     }
```

The string lives in the styling (or `cfg.code_marker` / `--code-marker=STR`,
which override it), and it is **off** unless switched on: `marker_enabled: true`
in the styling, `FYMD_RF_CODE_MARKER` / `--marker=on` per render.
`FYMD_RF_NO_CODE_MARKER` / `--marker=off` switches a configured marker back off,
so one styling serves both shapes.

It applies to every fenced path -- plain, highlighted, the diff view and the raw
`fymd_render_fenced_block()` -- but not to the `code.reverse` bubble, which
frames its own background.

A ```` ```diff ```` / ```` ```patch ```` fence is not handed to the tree-sitter
`diff` grammar: it is rendered as a GitHub-like diff view -- a new-side
line-number gutter derived from the `@@ -a,b +c,d @@` hunk headers (blank on
removed lines), a full-width background band per added/removed/hunk row
(`diff_added` / `diff_removed` / `diff_hunk` / `diff_file` / `diff_context` /
`diff_gutter` elements), and row content highlighted as the language of the file
being patched. That language comes from the `+++`/`---` header paths, or from an
explicit info-string override: ```` ```diff c ````, ```` ```diff:c ````. CLI:
`--diff=on|off`, `--diff-lines=on|off`, `--diff-highlight=on|off`.

A bare patch file (no Markdown around it) gets the same view through the raw
fenced-block path: `fymd4c --language=diff x.patch`, or `--language=auto`, which
picks `diff` from the `.diff`/`.patch` extension.

## Heal Utility (`md4c-heal.h`)

Fixes incomplete/streaming Markdown so it renders correctly mid-stream. A
**pre-parser text transform** — it does not use the parser and has no
dependency on it.

```c
int md_heal(const char* input, unsigned input_size,
            void (*process_output)(const char*, unsigned, void*), void* userdata);
```

Closes unclosed bold/italic/strikethrough/inline-code/`$$` math markers and
fenced code blocks, completes incomplete links, removes incomplete image markup
and trailing incomplete HTML tags, prevents setext-heading misinterpretation,
and escapes comparison operators in list items. Inspired by
[remend](https://github.com/vercel/streamdown/tree/main/packages/remend).
Formatting inside complete code spans / fenced blocks and math is not healed.

The shared header `md4c-heal-wrap.h` provides the heal-before-render path used by
`MD_ANSI_FLAG_HEAL` (heal the input, then render).

## Display width (`libfymd4c-width.h`)

The columns a text occupies in a terminal, from Unicode 15.0.0 tables generated
by `scripts/gen-wcwidth.py`.

```c
int    fymd_cp_width(unsigned int cp);
size_t fymd_str_width(const char *s, size_t len);   /* FYMD_NT to the NUL */
size_t fymd_utf8_decode(const char *s, size_t len, unsigned int *cp);
```

`fymd_cp_width()` answers 0 for a combining mark, a zero width format character
and a control, 2 for East Asian Wide and Fullwidth and for the emoji that
present wide, and 1 otherwise. A malformed byte measures as one column and
`fymd_utf8_decode()` always advances, so a caller cannot loop on bad input.

This is a per-codepoint measure: it does not segment grapheme clusters, so a
ZWJ emoji sequence measures as the sum of its parts and is wider than a
terminal that composes it will draw.

The ANSI renderer uses these for its own layout, so a consumer that draws cells
itself measures text exactly as the renderer does.
