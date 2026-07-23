#!/usr/bin/env python3
"""Validate bundled document themes and progressive rendering."""

import argparse
import os
import re
import subprocess
import sys


ANSI = re.compile(br"\033\[[0-9;]*m")


def render(program, theme, markdown, background, progressive=False,
           reverse=False):
    command = [program, "--color=on", "--width=72",
               "--background=" + background, "--theme=" + theme]
    if progressive:
        command.extend(("--stream-progressive", "--stream-chunk=7"))
    if reverse:
        command.append("--reverse")
    command.append(markdown)
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    parser.add_argument("-s", "--styles", required=True)
    opts = parser.parse_args()
    themes = {
        "catppuccin": (b"\033[1;38;2;203;166;247m",
                       b"\033[48;2;49;50;68m",
                       b"\033[48;2;220;224;232m"),
        "kanagawa": (b"\033[1;38;2;149;127;184m",
                     b"\033[48;2;42;42;55m",
                     b"\033[48;2;226;224;213m"),
        "solarized": (b"\033[1;38;2;38;139;210m",
                      b"\033[48;2;7;54;66m",
                      b"\033[48;2;238;232;213m"),
        "tokyonight": (b"\033[1;38;2;187;154;247m",
                       b"\033[48;2;41;46;66m",
                       b"\033[48;2;207;214;245m"),
    }
    markdown = os.path.join(opts.styles, "showcase.md")
    failed = checks = 0

    for name, (heading, card_dark, card_light) in themes.items():
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
            card = render(opts.program, name, markdown, background,
                          reverse=True)
            expected_card = card_dark if background == "dark" else card_light
            checks += 1
            if card.returncode != 0 or expected_card not in card.stdout:
                failed += 1
                print("FAIL %s/%s reverse: %r" %
                      (name, background, card.stderr))

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
