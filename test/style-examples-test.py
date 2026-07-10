#!/usr/bin/env python3
"""Smoke-test the shipped partial styling examples."""

import argparse
import os
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    parser.add_argument("-s", "--styles", required=True)
    opts = parser.parse_args()
    cases = {
        "table-ascii.yaml": (b"|", b"-", b"+"),
        "table-heavy.yaml": ("┃".encode(), "━".encode(), "╋".encode()),
        "table-colored-header.yaml": (b"\033[1;37;44m",),
        "table-borderless.yaml": (b"Alice", b"Bob"),
        "table-striped.yaml": (b"\033[7m", b"\033[48;5;236m",
                               b"\033[48;5;238m", b"\033[0m\033[48;5;236m"),
    }
    failed = checks = 0
    markdown_path = os.path.join(opts.styles, "table-basic.md")

    for name, needles in cases.items():
        checks += 1
        path = os.path.join(opts.styles, name)
        proc = subprocess.run(
            [opts.program, "--color=on", "--width=40", "--style=" + path,
             markdown_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0 or any(value not in proc.stdout for value in needles):
            failed += 1
            print("FAIL %s: %r %r" % (name, proc.stdout, proc.stderr))
        if name == "table-striped.yaml" and proc.returncode == 0:
            separator = next((line for line in proc.stdout.splitlines()
                              if "─".encode() in line), b"")
            checks += 1
            if b"\033[7m" not in separator or not separator.endswith(b"\033[27m"):
                failed += 1
                print("FAIL striped header separator style: %r" % separator)
        if name == "table-borderless.yaml" and proc.returncode == 0:
            checks += 1
            if any(glyph in proc.stdout for glyph in
                   ("│".encode(), "─".encode(), "┼".encode())):
                failed += 1
                print("FAIL borderless table contains grid glyphs: %r" % proc.stdout)
        if name in ("table-striped.yaml", "table-borderless.yaml") and proc.returncode == 0:
            for chunk in (1, 3, 11, 64):
                checks += 1
                streamed = subprocess.run(
                    [opts.program, "--color=on", "--width=40", "--style=" + path,
                     "--stream-progressive", "--stream-chunk=%d" % chunk,
                     markdown_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if streamed.returncode != 0 or streamed.stdout != proc.stdout:
                    failed += 1
                    print("FAIL %s progressive chunk=%d: %r %r" %
                          (name, chunk, streamed.stdout, streamed.stderr))

    print("%d passed, %d failed" % (checks - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
