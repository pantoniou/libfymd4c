# Progressive Tree-sitter highlighting

## Summary

Progressive rendering previously compiled and analyzed the same Tree-sitter
highlight query for every update. Rendering a 3,196-line C file one line at a
time could consequently take several minutes while using a full CPU.

The renderer now retains Tree-sitter state across compatible renders. The same
reproducer completes in approximately 2.9 seconds on the development machine,
while a whole-input render takes approximately 0.03 seconds.

There is no new public libfymd4c function. Existing callers receive the new
behavior automatically when they retain a `struct fymd_renderer` across
renders.

## Caller-facing libfymd4c API

### Progressive Markdown

Create one renderer, push every chunk through it, finish the stream, and then
destroy the renderer:

```c
struct fymd_renderer *renderer;
struct fymd_update update;
const char *final;
size_t final_len;

renderer = fymd_renderer_create(&config);
if (!renderer)
	return -1;

if (fymd_render_push(renderer, chunk, chunk_len, &update))
	goto fail;

/* Apply update.backtrack and update.content to the displayed region. */

if (fymd_render_finish(renderer, &final, &final_len))
	goto fail;

fymd_renderer_destroy(renderer);
```

The renderer owns the Markdown stream and the reusable syntax-highlighting
context. `update.content` and the buffer returned by `fymd_render_finish()`
remain renderer-owned and follow the lifetimes documented in
`libfymd4c-renderer.h`.

Do not create a renderer for every progress notification. Doing so deliberately
selects the old fresh-context behavior and discards the cached Tree-sitter
state.

### Repeated raw fenced blocks

`fymd_render_fenced_block()` also reuses highlighting state when repeated calls
use the same renderer:

```c
struct fymd_fenced_block_opts options;
struct fymd_renderer *renderer;
char *output;
size_t output_len;

memset(&options, 0, sizeof(options));
options.language = "c";
options.flags = FYMD_FBF_HIGHLIGHT;

renderer = fymd_renderer_create(&config);
if (!renderer)
	return -1;

if (fymd_render_fenced_block(renderer, source, source_len, &options,
				     &output, &output_len))
	goto fail;

fymd_free(output);
fymd_renderer_destroy(renderer);
```

The returned fenced-block buffer is still owned by the caller and must be
released with `fymd_free()`.

Reusing a renderer is most effective when successive calls render growing
prefixes of the same source. If the language, query identity, styling identity,
or other incompatible highlighting settings change, libfymd4c discards the
current libfyts context and creates a compatible one.

## Internal libfymd4c changes

`struct fymd_renderer` now owns an optional `struct fyts_ctx`. A pointer to this
slot is passed through `MD4C_STREAM_OPTS`, `MD4C_STREAM`, and the ANSI renderer.
Both progressive Markdown fences and raw fenced-block rendering therefore
reach the same retained context.

The ANSI renderer has two internal paths:

- A retained-context path calls `fyts_ctx_configure()` followed by
  `fyts_ctx_highlight_source()`.
- The reference path calls `fyts_highlight_source()` when no reusable context
  slot is supplied.

The reference path remains present intentionally. The fenced API test renders
each growing C prefix both through a retained renderer and through a newly
created renderer, and requires byte-for-byte identical output.

The retained context is destroyed when its owning `struct fymd_renderer` is
destroyed. Changing the renderer theme also destroys it because that changes
the styling identity.

No process-global cache or locking was added. A renderer and its retained
highlighter remain single-owner objects governed by the existing libfymd4c
threading model.

## New libfyts integration API

libfymd4c uses two new libfyts entry points. These are public libfyts APIs, not
new exported libfymd4c symbols.

### `fyts_ctx_highlight_source()`

```c
int fyts_ctx_highlight_source(struct fyts_ctx *ctx,
			      const char *source, size_t len,
			      char **out, size_t *out_len);
```

This renders a complete source buffer through an existing context. On success,
`*out` is heap allocated and must be released with `free()`.

Unlike `fyts_highlight_source()`, this function does not create or destroy a
context and does not write through `config.write`. It allows a caller such as
libfymd4c to retain the expensive Tree-sitter state while receiving the full
rendered buffer needed for line-diff processing.

### `fyts_ctx_configure()`

```c
int fyts_ctx_configure(struct fyts_ctx *ctx,
		       const struct fyts_config *config);
```

This updates compatible per-render configuration, including framing strings,
width, and output callback fields, without dropping the compiled query or
parsed tree. It returns `-1` when the new configuration changes an identity
that requires a new context, such as:

- language;
- query path;
- styling path, name, or generic styling value;
- color or background mode;
- reverse mode; or
- capture debugging/reporting mode.

libfymd4c handles `-1` by destroying the old context and creating a new one.

## State retained by libfyts

A compatible `fyts_ctx` now retains:

- the selected language and compiled `TSQuery`;
- a `TSParser` and incrementally edited `TSTree`;
- the previous parsed source used to recognize append-only updates;
- compiled regular expressions used by query predicates; and
- highlight spans for the unchanged prefix of a progressive-safe language.

For a growing progressive-safe source, libfyts reparses from the edited tree and
limits query execution to the changed line range. Previously emitted spans are
kept for the unchanged prefix. Languages marked non-progressive-safe continue
to query the complete source so later input can legitimately alter earlier
highlighting.

## Verification

The change is covered by:

- the libfyts stream test, including reuse after removal of a custom query file;
- a fresh-context versus retained-context byte-equivalence test in libfymd4c;
- libfymd4c streamed-versus-one-shot output tests;
- the full libfyts and libfymd4c test suites; and
- the libfyts address/undefined-behavior sanitizer suite.

The reference reproducer is:

```sh
fymd4c --color=on --language=c --fence-style=off \
	~/work/fyai/src/fyai_display.c \
	--stream-progressive --stream-mode=line --output=/dev/null
```
