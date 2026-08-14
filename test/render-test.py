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
import os
import subprocess
import sys
import tempfile

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
    cards = {
        "dark": "\x1b[48;2;40;42;46m",
        "light": "\x1b[48;2;232;232;232m",
    }
    fill = "\x1b[K\x1b[0m"
    ok = True
    for background, card in cards.items():
        out = subprocess.run(
            [program, "-t", "ansi", "--color=on", "--reverse",
             "--background=" + background, "--width=30"],
            input=text.encode("utf-8"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if out.returncode != 0:
            raise RuntimeError("fymd4c --reverse failed")
        lines = out.stdout.decode("utf-8").split("\n")[:-1]
        empty = card + card + fill
        valid = (bool(lines) and empty in lines and
                 all(line.startswith(card) and line.endswith(fill)
                     for line in lines))
        if not valid:
            ok = False
            print("FAIL reverse_card/%s" % background)
            for line in lines:
                print("  %r" % line)
    return ok


DIFF_INPUT = ("```diff\n"
              "--- a/x.txt\n"
              "+++ b/x.txt\n"
              "@@ -3,4 +3,4 @@\n"
              " ctx\n"
              "-gone\n"
              "+here\n"
              "```\n")


def check_diff(program):
    """```diff blocks render GitHub-style: a new-side line-number gutter
    (blank on removals, numbering resumed from the @@ header) and, with colour
    on, a background band per added/removed row padded to the right margin."""
    ok = True
    plain = run(program, DIFF_INPUT, ["--width=40"]).split("\n")
    rows = [line for line in plain if "%s " % BAR in line]
    expected = [
        "       %s --- a/x.txt" % BAR,
        "       %s +++ b/x.txt" % BAR,
        "       %s @@ -3,4 +3,4 @@" % BAR,
        "      3%s  ctx" % BAR,
        "       %s -gone" % BAR,
        "      4%s +here" % BAR,
    ]
    if rows != expected:
        ok = False
        print("FAIL diff_gutter")
        print("  expected: %r" % expected)
        print("  got:      %r" % rows)

    # Gutter off: no numbers, no separator column.
    nolines = run(program, DIFF_INPUT, ["--width=40", "--diff-lines=off"])
    if BAR in nolines or "-gone\n" not in nolines:
        ok = False
        print("FAIL diff_lines_off")
        print("  got: %r" % nolines)

    # --diff=off falls back to the plain fenced-code path (no gutter at all).
    off = run(program, DIFF_INPUT, ["--width=40", "--diff=off"])
    if BAR in off or "-gone\n" not in off:
        ok = False
        print("FAIL diff_off")
        print("  got: %r" % off)

    # A "git show" preamble carries no marker column: its first character must
    # survive ("commit", not "ommit"), and it gets no line number.
    show = ("```diff\n"
            "commit deadbeef\n"
            "Author: A U Thor <a@example.com>\n"
            "\n"
            "    subject line\n"
            "\n"
            "--- a/x.txt\n"
            "+++ b/x.txt\n"
            "@@ -3,2 +3,2 @@\n"
            "-gone\n"
            "+here\n"
            "```\n")
    rows = [ln for ln in run(program, show, ["--width=44"]).split("\n")
            if "%s " % BAR in ln]
    expected = [
        "       %s commit deadbeef" % BAR,
        "       %s Author: A U Thor <a@example.com>" % BAR,
        "       %s " % BAR,
        "       %s     subject line" % BAR,
        "       %s " % BAR,
        "       %s --- a/x.txt" % BAR,
        "       %s +++ b/x.txt" % BAR,
        "       %s @@ -3,2 +3,2 @@" % BAR,
        "       %s -gone" % BAR,
        "      3%s +here" % BAR,
    ]
    if rows != expected:
        ok = False
        print("FAIL diff_preamble")
        print("  expected: %r" % expected)
        print("  got:      %r" % rows)

    # An informal snippet with no headers at all still marks up its rows.
    bare = run(program, "```diff\n-old\n+new\n ctx\n```\n", ["--width=40"])
    if ["       %s -old" % BAR, "       %s +new" % BAR, "       %s  ctx" % BAR] != \
            [ln for ln in bare.split("\n") if "%s " % BAR in ln]:
        ok = False
        print("FAIL diff_headerless: %r" % bare)

    # Coloured bands: the added/removed rows carry a background that is reset
    # only at end of row, and the row is padded out to the margin.
    out = subprocess.run([program, "-t", "ansi", "--color=on",
                          "--background=dark", "--width=40"],
                         input=DIFF_INPUT.encode("utf-8"),
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise RuntimeError("fymd4c --color=on failed")
    coloured = out.stdout.decode("utf-8").split("\n")
    add = [ln for ln in coloured if "+here" in ln]
    rem = [ln for ln in coloured if "-gone" in ln]
    if (len(add) != 1 or len(rem) != 1 or
            "\x1b[48;2;18;56;32m+here" not in add[0] or
            "\x1b[48;2;74;24;26m-gone" not in rem[0] or
            not add[0].endswith("\x1b[49m") or
            not rem[0].endswith("\x1b[49m")):
        ok = False
        print("FAIL diff_bands")
        print("  add: %r" % add)
        print("  rem: %r" % rem)
    return ok


NO_DECORATION = 'code:\n  decoration:\n    header: ""\n    footer: ""\n'


def check_marker(program):
    """code.decoration.marker / --code-marker: the first row of a fenced block
    carries the marker, the rest are indented to its display width."""
    ok = True
    src = "```c\nint main(void)\n{\n  return 0;\n}\n```\n"
    expected = ("  \u23bf  int main(void)\n"
                "     {\n"
                "       return 0;\n"
                "     }\n")
    for extra in (["--code-marker=\u23bf  ", "--style=" + _tmp_style(NO_DECORATION)],
                  ["--style=" + _tmp_style(NO_DECORATION +
                                           '    marker: "\u23bf  "\n')]):
        got = run(program, src, extra)
        os.unlink(extra[-1].split("=", 1)[1])
        if got != expected:
            ok = False
            print("FAIL code_marker (%s)" % extra[0])
            print("  expected: %r" % expected)
            print("  got:      %r" % got)
    return ok


def _tmp_style(text):
    fd, path = tempfile.mkstemp(suffix=".yaml")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    return path


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

    for check in (check_reverse, check_diff, check_marker):
        if check(opts.program):
            passed += 1
        else:
            failed += 1

    print("%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
