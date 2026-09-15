#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A fenced-code bubble opens on a blank row, under its label when it has a
language, keeps the quote bars on its blank rows, and ends on a blank row, in
one-shot and streamed renders alike."""

import argparse
import re
import subprocess

BAR = "   ▏"
ESCAPES = re.compile(rb"\x1b\[[0-9;:]*[mK]")
LINES = ["line %d" % i for i in range(8)]
LONG = "".join(line + "\n" for line in LINES)

# (source, expected rows)
CASES = (
    # No language: the bubble opens on its blank row alone.
    ("> Before.\n>\n> ```\n> if (x)\n> ```\n>\n> After.\n",
     [BAR + " Before.", BAR, BAR, BAR + "   if (x)", BAR, BAR, BAR + " After."]),
    # A label: the row under it stays in the quote.
    ("> Before.\n>\n> ```c\n> if (x)\n> ```\n>\n> After.\n",
     [BAR + " Before.", BAR, BAR + " ── C ──", BAR,
      BAR + "   if (x)", BAR, BAR, BAR + " After."]),
    # A bubble that ends the output ends on its blank row.
    ("> ```c\n> if (x)\n> ```\n",
     [BAR + " ── C ──", BAR, BAR + "   if (x)", BAR]),
    # Code that ends on a blank row has its row already.
    ("> ```c\n> if (x)\n>\n> ```\n",
     [BAR + " ── C ──", BAR, BAR + "   if (x)", BAR]),
    # Outside a quote the blank rows are plain.
    ("```\nif (x)\n```\n\nAfter.\n",
     ["", "     if (x)", "", "", "   After."]),
)

# Long fences: a small active region commits inside them, and the stream
# continues the open fence without losing a code row. It strips only the rows
# the header drew, and a fence without a language draws no label row.
CONTINUED = (
    "```\n" + LONG + "```\n\nafter\n",
    "```c\n" + LONG + "```\n\nafter\n",
)


def text(data):
    return ESCAPES.sub(b"", data).decode("utf-8")


def rows(data):
    return [row.rstrip() for row in text(data).split("\n")][:-1]


def render(args, data):
    return subprocess.run(args, input=data, capture_output=True,
                          check=True).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    opts = parser.parse_args()
    args = [opts.program, "--palette=ember", "--width=50", "--color=on",
            "--background=dark"]
    streams = [[stream, "--stream-chunk=" + str(chunk)]
               for stream in ("--stream", "--stream-progressive")
               for chunk in (1, 7, 32)]
    for source, expected in CASES:
        data = source.encode("utf-8")
        oneshot = render(args, data)
        actual = rows(oneshot)
        assert actual == expected, (source, expected, actual)
        for extra in streams:
            streamed = render(args + extra, data)
            assert streamed == oneshot, (extra, source, oneshot, streamed)
    for source in CONTINUED:
        data = source.encode("utf-8")
        for extra in streams:
            streamed = text(render(args + extra + ["--max-active-lines=2"],
                                   data))
            for line in LINES:
                count = len(re.findall(r"\b" + line + r"\b", streamed))
                assert count >= 1, (extra, source, line, count, streamed)


if __name__ == "__main__":
    main()
