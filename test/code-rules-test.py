#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Palette fence layouts must agree in one-shot and streamed renders."""

import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    opts = parser.parse_args()
    fixtures = (
        "```unknown-language\nplain\n\nmore\n```\n\nafter\n",
        "```c\n/* comment\n * continued */\nint x;\n```\n\nafter\n",
        "```diff\n@@ -1 +1 @@\n-old\n+new\n```\n\nafter\n",
        "> ```c\n> int x;\n> ```\n\nafter\n",
    )
    with tempfile.TemporaryDirectory(prefix="fymd-code-rules-") as directory:
        theme = pathlib.Path(directory) / "rules.yaml"
        for mode in ("none", "top", "both", "top-bottom", "bubble-rule-faint",
                     "bubble-raise-rule", "bubble-raise-faint"):
            theme.write_text(
                "params: {md.code.rules: " + mode + "}\n"
                "colors: {raise: '#234567', rule: '#123456', faint: '#abcdef'}\n",
                encoding="utf-8",
            )
            for color in ("on", "off"):
                args = [opts.program, "--palette=" + str(theme),
                        "--width=40", "--color=" + color, "--background=dark"]
                for fixture in fixtures:
                    source = fixture.encode("utf-8")
                    expected = subprocess.run(args, input=source, capture_output=True,
                                              check=True).stdout
                    for stream in ("--stream", "--stream-progressive"):
                        for chunk in (1, 7, 32):
                            actual = subprocess.run(
                                args + [stream, "--stream-chunk=" + str(chunk)],
                                input=source, capture_output=True, check=True,
                            ).stdout
                            assert actual == expected, (mode, color, stream, chunk,
                                                        fixture, expected, actual)


if __name__ == "__main__":
    main()
