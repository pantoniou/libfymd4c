#!/usr/bin/env python3
"""Validate bundled document themes and progressive rendering."""

import argparse
import os
import re
import subprocess
import sys


ANSI = re.compile(br"\033\[[0-9;]*m")


def render(program, theme, markdown, background, progressive=False):
    command = [program, "--color=on", "--width=72",
               "--background=" + background, "--theme=" + theme]
    if progressive:
        command.extend(("--stream-progressive", "--stream-chunk=7"))
    command.append(markdown)
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    parser.add_argument("-s", "--styles", required=True)
    opts = parser.parse_args()
    themes = {
        "catppuccin": b"\033[1;38;2;203;166;247m",
        "kanagawa": b"\033[1;38;2;149;127;184m",
        "solarized": b"\033[1;38;2;38;139;210m",
        "tokyonight": b"\033[1;38;2;187;154;247m",
    }
    markdown = os.path.join(opts.styles, "showcase.md")
    failed = checks = 0

    for name, heading in themes.items():
        for background in ("dark", "light"):
            checks += 1
            normal = render(opts.program, name, markdown, background)
            progressive = render(opts.program, name, markdown, background, True)
            expected = heading if background == "dark" else b"\033["
            leaked = b"default-fg" in normal.stdout or b"reset" in normal.stdout
            same_text = ANSI.sub(b"", normal.stdout) == ANSI.sub(b"", progressive.stdout)
            if (normal.returncode != 0 or progressive.returncode != 0 or
                    expected not in normal.stdout or leaked or not same_text):
                failed += 1
                print("FAIL %s/%s: %r %r" %
                      (name, background, normal.stderr, progressive.stderr))

        checks += 1
        table_markdown = os.path.join(opts.styles, "..", "examples", "table-basic.md")
        borderless = render(opts.program, name + "-borderless", table_markdown, "dark")
        if (borderless.returncode != 0 or
                any(glyph in borderless.stdout for glyph in
                    ("│".encode(), "─".encode(), "┼".encode()))):
            failed += 1
            print("FAIL %s-borderless: %r" % (name, borderless.stderr))

    print("%d passed, %d failed" % (checks - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
