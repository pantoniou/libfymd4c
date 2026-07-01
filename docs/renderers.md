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

### Config flags (`FYMD_RF_*`)

| Flag                   | Description                                       |
| ---------------------- | ------------------------------------------------- |
| `FYMD_RF_NO_COLOR`     | Emit no SGR color sequences                       |
| `FYMD_RF_SHOW_URLS`    | Show link targets inline                          |
| `FYMD_RF_TABLE_FIT`    | Size tables to content instead of filling width   |
| `FYMD_RF_HEAL`         | Close dangling markers in the active/in-progress tail |
| `FYMD_RF_REVERSE`      | Render the whole document as a card (theme background filled to width) |
| `FYMD_RF_NO_CODE_HL`   | Disable fenced-code syntax highlighting           |

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
glyphs:
  blockquote_bar: "│"   list_bullet: "*"
  table_vertical: "│"   table_horizontal: "─"   table_cross: "┼"
background: auto     # dark | light | auto (auto consults $COLORFGBG, else dark)
code:                # fenced-code highlighting (libfyts)
  enabled: true
  theme: default     # embedded "default", or a path to a libfyts styling YAML
  background: auto    # auto follows the document background
```

The `light:` sub-map of an element overrides its `on`/`off` on a light
background; `background:` (or CLI `--background=dark|light|auto`) selects which
variant is used. Specific "off" codes (e.g. `bold-off` = `\e[22m`) rather than a
blanket reset keep nested styling intact. The `code:` block configures the
libfyts highlighter; the language catalogue is the build-time
`MD4C_FYTS_CATALOGUE` choice.

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
