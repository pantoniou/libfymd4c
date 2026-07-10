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
    }
    failed = 0
    markdown_path = os.path.join(opts.styles, "table-basic.md")

    for name, needles in cases.items():
        path = os.path.join(opts.styles, name)
        proc = subprocess.run(
            [opts.program, "--color=on", "--width=40", "--style=" + path,
             markdown_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0 or any(value not in proc.stdout for value in needles):
            failed += 1
            print("FAIL %s: %r %r" % (name, proc.stdout, proc.stderr))

    print("%d passed, %d failed" % (len(cases) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
