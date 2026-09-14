#!/usr/bin/env python3
"""CLI tests for --table-size: fit is the default, fit-left is fit, and
fit-center and fit-right place a fitted table in the width."""

import argparse
import subprocess
import sys


SOURCE = b"| name | value |\n|---|---|\n| a | 1 |\n"


def run(program, args):
    return subprocess.run([program, "--color=off", "--width=60"] + args,
                          input=SOURCE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def header(output):
    """The leading blanks of the header row and the columns after them. The
    blanks at the end of the row pad its last cell, so they count."""
    for line in output.decode("utf-8").splitlines():
        if "name" in line:
            lead = len(line) - len(line.lstrip(" "))
            return lead, len(line) - lead
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    options = parser.parse_args()
    passed = failed = 0

    def check(cond, what, detail=""):
        nonlocal passed, failed
        if cond:
            passed += 1
        else:
            failed += 1
            print("FAIL %s %s" % (what, detail))

    runs = {}
    for mode in (None, "fill", "fit", "fit-left", "fit-center", "fit-right"):
        result = run(options.program,
                     [] if mode is None else ["--table-size=%s" % mode])
        check(result.returncode == 0, "--table-size=%s runs" % mode,
              result.stderr)
        runs[mode] = result.stdout

    check(runs[None] == runs["fit"], "fit is the default")
    check(runs["fit-left"] == runs["fit"], "fit-left is fit")

    fill, fit = header(runs["fill"]), header(runs["fit"])
    center, right = header(runs["fit-center"]), header(runs["fit-right"])
    if None in (fill, fit, center, right):
        check(False, "every mode draws the header row")
    else:
        edge = fill[0] + fill[1]
        room = edge - (fit[0] + fit[1])
        check(fit[1] < fill[1], "fit is narrower than fill", (fit, fill))
        check(center == (fit[0] + room // 2, fit[1]),
              "fit-center has half the room on its left", (center, fit, room))
        check(right == (edge - fit[1], fit[1]),
              "fit-right ends at the edge of fill", (right, fill))

    bad = run(options.program, ["--table-size=middle"])
    check(bad.returncode != 0 and b"fit-center" in bad.stderr,
          "a bad mode is refused with the modes it takes", bad.stderr)

    print("%d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
