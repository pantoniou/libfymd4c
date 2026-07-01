#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Streaming (push) mode test for the md4c ANSI renderer.
#
# Property under test: feeding a document through `--stream` (in arbitrary
# chunks) and concatenating the committed output + final remainder must be
# byte-identical to the one-shot ANSI render of the same document. This is
# checked across several chunk sizes and widths, with and without healing.

import argparse
import subprocess
import sys

# Fixtures exercising constructs whose block interpretation depends on
# lookahead (tables, setext headings, fenced code, lists) plus prose wrapping,
# blockquotes, nesting, CJK/emoji widths and inline styling.
FIXTURES = {
    "headings_paras": "# Title\n\nFirst paragraph with some words.\n\n## Sub\n\nSecond paragraph here.\n",
    "wrapping": "This is a fairly long paragraph of prose that should be word-wrapped to the configured width across several lines just like glow does.\n",
    "setext": "A setext heading\n================\n\nFollowed by a normal paragraph of text.\n",
    "list": "- one item\n- two item with a good amount of text that may wrap around\n- three\n\nafter the list\n",
    "blockquote": "> a quoted line that is long enough to wrap onto another line within the quote\n>\n> second quoted paragraph\n\nout.\n",
    "table": "| Name | Role | Notes |\n|:-----|:----:|------:|\n| Alice | Admin | active |\n| Bob | User | a longer cell value |\n\ndone.\n",
    "table_in_quote": "> | A | B |\n> |---|---|\n> | 1 | 2 |\n\nafter.\n",
    "code": "```js\nconst x = 1;\n\nconst y = 2;\n```\n\nafter code.\n",
    "inline": "Some **bold** and *italic* and `code` and ~~del~~ text here.\n",
    "cjk_emoji": "| Lang | Word |\n|------|------|\n| cjk | 日本語 |\n| emoji | \U0001f680\U0001f525 |\n\nend.\n",
    "nested": "1. outer\n   - inner a\n   - inner b\n\n2. second\n",
    "mixed": "# Doc\n\nIntro paragraph.\n\n| x | y |\n|---|---|\n| 1 | 2 |\n\n> note\n\n- a\n- b\n\nThe end.\n",
    # Multi-line HTML blocks (CommonMark types 1-5) may contain blank lines and
    # only close on their end marker (</pre>, -->, ?>, >, ]]>), not on a blank
    # line. A blank line inside them must NOT be taken as a stream sync point, or
    # the block gets split and its Markdown-looking tail (e.g. *not em*) is
    # re-parsed as emphasis, diverging from the one-shot render.
    "html_comment": "Before\n\n<!-- a comment\n\nstill *not em*\n-->\n\nAfter\n",
    "html_pre": "Before\n\n<pre>\nline one\n\n*not em*\n</pre>\n\nAfter\n",
    "html_script": "A\n\n<script>\nvar x=1;\n\n*not em*\n</script>\n\nB\n",
    "html_cdata": "A\n\n<![CDATA[\nfoo\n\n*not em*\n]]>\n\nB\n",
    "html_decl": "A\n\n<!SOMEDECL\n\n*not em*\n>\n\nB\n",
}

CHUNKS = [1, 2, 3, 7, 16, 64]
WIDTHS = [40, 60, 80, 120]


def run(program, text, args):
    p = subprocess.run([program, "-t", "ansi", "--color=off"] + args,
                       input=text.encode("utf-8"),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError("md4c failed (%d): %s" % (p.returncode, p.stderr.decode("utf-8", "replace")))
    return p.stdout


def main():
    ap = argparse.ArgumentParser(description="md4c streaming mode test")
    ap.add_argument("-p", "--program", required=True, help="path to md4c binary")
    opts = ap.parse_args()

    passed = 0
    failed = 0

    # Core invariant: the committed stream is the true (unhealed) render, so
    # streamed output must be byte-identical to the one-shot render. Checked for
    # both the append-only push mode (--stream) and the progressive line-diff
    # mode (--stream-progressive, whose updates are applied to a virtual screen
    # by the CLI and the final screen printed).
    for name, text in FIXTURES.items():
        for w in WIDTHS:
            base = run(opts.program, text, ["--width=%d" % w])
            for chunk in CHUNKS:
                for mode in ("--stream", "--stream-progressive"):
                    got = run(opts.program, text,
                              ["--width=%d" % w, mode, "--stream-chunk=%d" % chunk])
                    if got == base:
                        passed += 1
                    else:
                        failed += 1
                        print("FAIL %s %s width=%d chunk=%d" % (name, mode, w, chunk))
                        print("  one-shot: %r" % base[:200])
                        print("  stream:   %r" % got[:200])

    # Heal smoke test: a stream cut off mid-emphasis. With --heal the trailing
    # marker is closed (bold applied, no literal "**" left); without it the
    # marker stays literal. (Healing is a display aid for truncated streams; it
    # is intentionally NOT required to match a one-shot render byte-for-byte,
    # since md_heal is a whole-document transform.)
    truncated = "a paragraph then some **bold that never closes"
    for mode in ("--stream", "--stream-progressive"):
        healed = run(opts.program, truncated, ["--width=80", mode, "--stream-chunk=5", "--heal"])
        plain = run(opts.program, truncated, ["--width=80", mode, "--stream-chunk=5"])
        if b"**" in healed:
            failed += 1
            print("FAIL heal smoke (%s): trailing ** not healed: %r" % (mode, healed))
        else:
            passed += 1
        if b"**" not in plain:
            failed += 1
            print("FAIL heal smoke (%s): ** unexpectedly removed without --heal: %r" % (mode, plain))
        else:
            passed += 1

    print("%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
