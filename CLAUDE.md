# CLAUDE.md

Guidance for working in this repository.

## What this fork is

Vanilla **md4c** (an unmodified one-shot CommonMark parser, `src/md4c.c` +
`md4c.h`, MIT) plus a set of thin add-on modules built over its public SAX API,
and a public wrapper library that packages them for consumers:

- **`md4c-ansi`** (`src/md4c-ansi.{c,h}`) — Markdown → ANSI terminal renderer.
- **`md4c-style`** (`src/md4c-style.{c,h}`) — YAML-driven styling (libfyaml),
  light/dark themes, no global state; embeds `stylings/md4c-default.yaml`.
- **`md4c-heal`** (`src/md4c-heal.{c,h}`) — pre-parser text transform that closes
  dangling Markdown markers for mid-stream rendering. Parser-independent.
- **`md4c-stream`** (`src/md4c-stream.{c,h}`) — push/streaming front-end with
  safe-sync-point commit, progressive line-diff updates, active-region cap.
- **`libfymd4c`** (`src/libfymd4c.c`, `include/libfymd4c*`) — the **one shipped,
  public, opaque-typed library** (libfyaml-style API). Everything above (the md4c
  parser + `entity.c`, HTML renderer, heal, ANSI renderer, styling, streaming) is
  compiled into it and hidden; it exports only the `fymd_*` API. There are **no
  standalone `libmd4c` / `libmd4c-html` / `libmd4c-heal` libraries** — a single
  self-contained shared object whose only runtime dependency is libfyaml. The
  renderer lives behind `struct fymd_renderer`; stateless HTML/heal conversions
  are plain `fymd_render_html*` / `fymd_heal*` functions.
- **`fymd4c`** (`fymd4c/`) — the **single CLI**, linking only `libfymd4c`. One
  tool for all formats: `-t ansi` (default), `-t html` (with dialect / `-f`
  full-html / `-x` xhtml flags), `-t heal`. Parses options with standard
  `getopt_long` (long-only flags use value codes ≥ 256). It needs only
  `libfymd4c.h` plus `md4c.h` (for the `MD_FLAG_*` dialect constants). The
  conformance scripts drive it via `fymd4c -t html`.

`entity.c` is compiled into `libfymd4c` a single time, so the parser and HTML
renderer share it without duplicate symbols in static builds.

Fenced code blocks are syntax-highlighted with **libfyts** (tree-sitter).
`MD4C_FYTS_PROVIDER` (`auto`/`system`/`fetch`) selects where it comes from: `auto`
uses the installed libfyts CMake package (`find_package(libfyts CONFIG)`) if
present, else builds it from source via FetchContent (pinned `GIT_TAG`). We always
link libfyts' **static** target — `libfyts::fyts_static` (installed) or
`fyts_static` (in-tree) — via `${MD4C_FYTS_TARGET}`, so tree-sitter and the
grammars are absorbed into the shared libfymd4c. `MD4C_FYTS_CATALOGUE`
(minimal/default/full) selects the grammars, and applies to `fetch` mode only.

## The public API (libfymd4c)

`include/libfymd4c.h` →
`libfymd4c/{libfymd4c-util.h,libfymd4c-renderer.h,libfymd4c-convert.h}`.
Conventions mirror libfyaml (at `~/work/libfyaml`): `fymd_`/`FYMD_` prefixes,
bare opaque `struct` forward-decls, `cfg`+flag-enum constructors, `FYMD_EXPORT`
visibility. One handle, both modes:

```c
struct fymd_renderer *r = fymd_renderer_create(&cfg);   /* cfg or NULL */
char *s = fymd_render_to_string(r, md, len);            /* one-shot */
fymd_render_push(r, chunk, n, &upd);                    /* progressive + heal */
fymd_render_finish(r, &out, &olen);
fymd_renderer_destroy(r);
```

`MD_ANSI_STYLE` and `MD4C_STREAM` are implementation details hidden behind the
handle. When adding a renderer capability, expose it through
`struct fymd_renderer_cfg` (a `FYMD_RF_*` flag or field), not by leaking an
internal type. Stateless conversions (`libfymd4c-convert.h`) don't take a
renderer: `fymd_render_html(md, len, parser_flags, FYMD_HTML_*, &out, &olen)`
and `fymd_heal(md, len, &out, &olen)` (results freed with `fymd_free`).

libfyaml is an explicit public dependency: the cfg's `style_generic` field takes
an already-parsed `fy_generic` mapping (highest precedence over `style_path` /
`style`), so the public header includes `<libfyaml/libfyaml-generic.h>` and the
`.pc` / CMake package declare the libfyaml dependency.

## Build & test

```sh
cmake -S . -B build && cmake --build build     # configure fetches libfyts (~18s)
ctest --test-dir build -j$(nproc)               # stream + CommonMark conformance
```

`libfymd4c` compiles all of its sources (parser, entity table, HTML renderer,
heal, ANSI renderer, styling, stream) directly with `-fvisibility=hidden`
(static-archive libfyts/tree-sitter symbols additionally hidden with
`-Wl,--exclude-libs,ALL`). Nothing else is built as a separate library, so there
are no duplicate-symbol concerns.

**Two flavours are built from the same sources in one configure** (mirroring
libfyaml/libfyts): `fymd4c` (the primary library — shared when
`BUILD_SHARED_LIBS`) and `fymd4c_static` (a plain static archive; `libfymd4c.a`
next to `libfymd4c.so` on ELF). The shared object absorbs the static
libfyts/tree-sitter, so it is self-contained — NEEDED deps are just `libfyaml`
(+ libc). A `.a` cannot absorb another archive, so `fymd4c_static` **declares**
its deps (libfyaml, and libfyts when built against the installed package) through
the CMake package's `find_dependency()` — consumers link those too.

```sh
cmake -S . -B build && cmake --build build -j$(nproc)   # builds both .so and .a
```

Both are installed and exported: `libfymd4c::libfymd4c` (shared) and
`libfymd4c::libfymd4c_static` (the raw export names are `libfymd4c::fymd4c` /
`::fymd4c_static`; the config adds the `libfymd4c::`-prefixed aliases). The static
consumer path (`find_package(libfymd4c)` → `libfymd4c::libfymd4c_static`) links
libfyaml + a static libfyts transitively. `-DBUILD_SHARED_LIBS=OFF` makes the
primary target static too.

(The absorbed libfyts is linked into the shared object via
`$<BUILD_INTERFACE:${MD4C_FYTS_TARGET}>` so it stays out of the installed export
interface.)

Sanitizer build (`-DENABLE_ASAN=ON`, address + UB + signed-overflow, as in
libfyaml): the flags are applied to `libfymd4c` (compile + link) and propagated
to the `fymd4c` executable via the target's INTERFACE link options, so both the
`.so` and the CLI pull in `libasan`.

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan -j$(nproc)
```

**Invariant to preserve:** only `fymd_*` symbols are exported. Verify after any
build-system change:

```sh
nm -D --defined-only build/src/libfymd4c.so | grep ' T '   # must be 15 fymd_*
```

The `stream` ctest enforces the other core invariant: `--stream` and
`--stream-progressive` output is byte-identical to the one-shot render.

The CommonMark (+ extension) conformance suites are folded into ctest: the top
`CMakeLists.txt` registers **every example** in each `test/*.txt` spec file as
its own `conformance/<file>/<n>` ctest (driven through `fymd4c -t html` via
`run-testsuite.py -n N`), so `ctest -j` runs the ~940 examples in parallel
(libfyaml's per-subtest registration pattern). The example count is discovered at
configure time from the spec markers; `ctest -L conformance` selects just these.

## Conventions

- C90-clean (`-Wdeclaration-after-statement` is enforced); declarations at block
  top. Match the surrounding md4c style.
- No hardcoded ANSI sequences in the renderer — everything routes through
  `md4c-style` / the YAML config. No process-global styling state.
- libfyaml generic API idiom (used in `md4c-style.c`): `fy_get(map, key, typed_default)`
  with a typed default (`""`, not `NULL`/`0`).
- Cross-repo libfyts fixes land upstream first, then bump the pinned `GIT_TAG`.
- Soname comes from `.libtool-version` (`current:revision:age`), parsed in the
  top `CMakeLists.txt` like libfyaml.

See [`docs/renderers.md`](docs/renderers.md) for the full module/API reference.
