#!/usr/bin/env python3
"""Check that a style loaded from its image renders as the parsed text does."""

import argparse
import os
import subprocess
import sys


def render(program, markdown, theme, parse):
    env = dict(os.environ)
    env.pop("FYMD_NO_IMAGE", None)
    if parse:
        env["FYMD_NO_IMAGE"] = "1"
    command = [program, "--color=on", "--width=72"]
    if theme:
        command.append("--theme=" + theme)
    command.append(markdown)
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    parser.add_argument("-s", "--styles", required=True)
    opts = parser.parse_args()
    markdown = os.path.join(opts.styles, "showcase.md")
    names = [None]
    names.extend(sorted(f[:-5] for f in os.listdir(opts.styles)
                        if f.endswith(".yaml")))
    failed = 0

    for name in names:
        image = render(opts.program, markdown, name, False)
        parse = render(opts.program, markdown, name, True)
        if (image.returncode != 0 or parse.returncode != 0 or
                not image.stdout or image.stdout != parse.stdout):
            failed += 1
            print("FAIL %s: %r %r" % (name or "default", image.stderr,
                                      parse.stderr))

    print("%d passed, %d failed" % (len(names) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
