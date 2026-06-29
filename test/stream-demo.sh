#!/bin/sh
# Demo of md4c's live progressive ANSI rendering (--stream-progressive).
#
# Feeds a Markdown document into `md4c` one line at a time (with a small delay)
# so you can watch the active region update in place on the terminal: a bare
# table row reflows into a laid-out table, rows append, and a wide cell reflows
# the whole table.
#
# Usage: test/stream-demo.sh [path-to-md4c] [width]

set -e

MD4C="${1:-build/fymd4c/fymd4c}"
WIDTH="${2:-50}"
DELAY="${STREAM_DEMO_DELAY:-0.25}"

doc='# Streaming demo

A paragraph that arrives a line at a time and word-wraps to the
configured width as the words come in.

| Name | Role | Score |
| :--- | :--: | ----: |
| Alice | Admin | 5 |
| Bob | Superuser | 1234 |
| Cy | Guest | a rather wide note that forces a reflow |

> A closing blockquote, **streamed** to the end.'

# Emit the document line by line with a delay, into the live renderer.
printf '%s\n' "$doc" | while IFS= read -r line; do
    printf '%s\n' "$line"
    sleep "$DELAY"
done | "$MD4C" -t ansi --stream-progressive --width="$WIDTH"
