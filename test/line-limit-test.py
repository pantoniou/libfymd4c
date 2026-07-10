#!/usr/bin/env python3
"""Rendered-row viewport tests for one-shot and progressive ANSI output."""

import argparse
import subprocess
import sys


TEXT = "\n\n".join("line %d" % n for n in range(1, 9)) + "\n"
CHUNKS = (1, 2, 7, 64)


def run(program, args, text=TEXT, check=True):
    proc = subprocess.run(
        [program, "-t", "ansi", "--color=off", "--width=60"] + args,
        input=text.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and proc.returncode:
        raise RuntimeError("fymd4c failed: %s" % proc.stderr.decode("utf-8", "replace"))
    return proc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    opts = parser.parse_args()
    passed = failed = 0

    full = run(opts.program, []).stdout
    rows = full.splitlines(keepends=True)

    cases = []
    cases.append(("below_limit", ["--max-lines=99"], full))
    cases.append(("exact_limit", ["--max-lines=%d" % len(rows)], full))
    cases.append(("scroll", ["--max-lines=5"], b"".join(rows[-5:])))

    maximum = 6
    head = (maximum - 1) // 2
    tail = maximum - head - 1
    omitted = len(rows) - head - tail
    expected = (b"".join(rows[:head]) +
                ("... %d lines omitted ...\n" % omitted).encode() +
                b"".join(rows[-tail:]))
    cases.append(("balanced", ["--max-lines=6", "--line-overflow=head-tail"], expected))

    head, maximum = 2, 5
    tail = maximum - head - 1
    omitted = len(rows) - head - tail
    expected = (b"".join(rows[:head]) +
                ("[hidden %d / 100%%]\n" % omitted).encode() +
                b"".join(rows[-tail:]))
    cases.append(("explicit_head",
                  ["--max-lines=5", "--line-head=2",
                   "--line-separator=[hidden %d / 100%%]"], expected))

    for name, args, expected in cases:
        got = run(opts.program, args).stdout
        if got == expected:
            passed += 1
        else:
            failed += 1
            print("FAIL %s: expected %r, got %r" % (name, expected, got))
        for chunk in CHUNKS:
            streamed = run(opts.program, args + ["--stream-progressive",
                                                  "--stream-chunk=%d" % chunk]).stdout
            if streamed == expected:
                passed += 1
            else:
                failed += 1
                print("FAIL %s progressive chunk=%d: expected %r, got %r" %
                      (name, chunk, expected, streamed))

    invalid = (
        ["--max-lines=2", "--line-overflow=head-tail"],
        ["--max-lines=5", "--line-head=0"],
        ["--max-lines=5", "--line-head=4"],
        ["--max-lines=5", "--line-separator=no-conversion"],
        ["--max-lines=5", "--line-separator=bad-%s"],
    )
    for args in invalid:
        if run(opts.program, args, check=False).returncode != 0:
            passed += 1
        else:
            failed += 1
            print("FAIL invalid options accepted: %r" % (args,))

    print("%d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
