#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Golden-output tests for the ANSI renderer's block structure. Unlike
# stream-test.py (which only checks that streamed output matches the one-shot
# render), these pin the actual rendered text for constructs whose layout has
# regressed before: nested lists (marker type and indentation must not leak
# across nesting levels) and blockquotes (the bar is kept on blank lines).
#
# Run with --color=off so the output is plain text; the bar glyph is U+2502.

import argparse
import subprocess
import sys

BAR = "│"

# name -> (markdown input, expected --color=off output)
CASES = {
    # A bullet list nested in an ordered item keeps bullet markers (not the
    # parent's numbering) and each level indents two more columns.
    "nested_ul_under_ol": (
        "1. Ordered item\n"
        "   - Nested bullet\n"
        "   - Another bullet\n"
        "     - Deeper bullet\n",
        "  1. Ordered item\n"
        "    * Nested bullet\n"
        "    * Another bullet\n"
        "      * Deeper bullet\n",
    ),
    # An ordered list nested in a bullet item keeps its own numbering.
    "nested_ol_under_ul": (
        "- top\n"
        "  1. one\n"
        "  2. two\n",
        "  * top\n"
        "    1. one\n"
        "    2. two\n",
    ),
    # Ordered lists honour the start value.
    "ol_start": (
        "3. three\n"
        "4. four\n",
        "  3. three\n"
        "  4. four\n",
    ),
    # A tight list (no blank lines in source) renders compactly.
    "tight_list": (
        "- a\n- b\n- c\n",
        "  * a\n  * b\n  * c\n",
    ),
    # A loose list (blank line between items) keeps a blank line between them.
    "loose_list": (
        "- a\n\n- b\n",
        "  * a\n\n  * b\n",
    ),
    # A blank line inside a blockquote keeps the quote bar.
    "quote_blank_bar": (
        "> para one\n>\n> para two\n",
        "  %s para one\n  %s\n  %s para two\n" % (BAR, BAR, BAR),
    ),
    # Nested blockquotes: the blank line belongs to the outer quote only.
    "nested_quote": (
        "> outer\n>\n> > inner\n",
        "  %s outer\n  %s\n  %s %s inner\n" % (BAR, BAR, BAR, BAR),
    ),
}


def run(program, text, extra=None):
    p = subprocess.run([program, "-t", "ansi", "--color=off", "--width=60"]
                       + (extra or []),
                       input=text.encode("utf-8"),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError("fymd4c failed (%d): %s"
                           % (p.returncode, p.stderr.decode("utf-8", "replace")))
    return p.stdout.decode("utf-8")


def check_reverse(program):
    """Whole-document card (--reverse): every emitted line must be wrapped in
    the theme background and padded to the edge with erase-to-end-of-line."""
    text = "# Title\n\nSome text.\n\n- a\n- b\n"
    out = subprocess.run(
        [program, "-t", "ansi", "--color=on", "--reverse",
         "--background=dark", "--width=30"],
        input=text.encode("utf-8"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise RuntimeError("fymd4c --reverse failed")
    lines = out.stdout.decode("utf-8").split("\n")[:-1]  # drop trailing ""
    BG = "\x1b[7m"           # contrasting card style (from the theme)
    FILL = "\x1b[K\x1b[0m"   # erase-to-EOL + reset
    ok = bool(lines) and all(l.startswith(BG) and l.endswith(FILL) for l in lines)
    if not ok:
        print("FAIL reverse_card")
        for l in lines:
            print("  %r" % l)
    return ok


def main():
    ap = argparse.ArgumentParser(description="ANSI renderer golden-output test")
    ap.add_argument("-p", "--program", required=True, help="path to fymd4c binary")
    opts = ap.parse_args()

    passed = failed = 0
    for name, (text, expected) in CASES.items():
        got = run(opts.program, text)
        if got == expected:
            passed += 1
        else:
            failed += 1
            print("FAIL %s" % name)
            print("  input:    %r" % text)
            print("  expected: %r" % expected)
            print("  got:      %r" % got)

    if check_reverse(opts.program):
        passed += 1
    else:
        failed += 1

    print("%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
