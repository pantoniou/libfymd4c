#!/usr/bin/env python3
"""CLI tests for --language raw fenced-block rendering."""

import argparse
import os
import subprocess
import sys
import tempfile


SOURCE = b"**literal markdown**\n# not a heading\n```still code```\n"


def run(program, args, data=SOURCE):
    return subprocess.run([program, "--color=off", "--width=40"] + args,
                          input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--program", required=True)
    options = parser.parse_args()
    passed = failed = 0

    base = run(options.program, ["--language=text"])
    if (base.returncode == 0 and b"**literal markdown**" in base.stdout and
            b"# not a heading" in base.stdout and b"```still code```" in base.stdout):
        passed += 1
    else:
        failed += 1
        print("FAIL explicit language: %r %r" % (base.stdout, base.stderr))

    for chunk in (1, 3, 17, 128):
        result = run(options.program,
                     ["--language=text", "--stream-progressive",
                      "--stream-chunk=%d" % chunk])
        if result.returncode == 0 and result.stdout == base.stdout:
            passed += 1
        else:
            failed += 1
            print("FAIL progressive language chunk=%d: %r" % (chunk, result.stdout))

    bare = run(options.program, ["--language=text", "--fence-style=off"])
    if bare.returncode == 0 and bare.stdout == SOURCE:
        passed += 1
    else:
        failed += 1
        print("FAIL undecorated language: %r %r" % (bare.stdout, bare.stderr))
    for chunk in (1, 5, 128):
        result = run(options.program,
                     ["--language=text", "--fence-style=off",
                      "--stream-progressive", "--stream-chunk=%d" % chunk])
        if result.returncode == 0 and result.stdout == SOURCE:
            passed += 1
        else:
            failed += 1
            print("FAIL progressive undecorated chunk=%d: %r" %
                  (chunk, result.stdout))

    limited = run(options.program,
                  ["--language=text", "--stream-progressive", "--stream-chunk=2",
                   "--max-lines=3", "--line-overflow=scroll"])
    if limited.returncode == 0 and len(limited.stdout.splitlines()) == 3:
        passed += 1
    else:
        failed += 1
        print("FAIL progressive language line limit: %r" % limited.stdout)

    path = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".c", delete=False) as source:
            path = source.name
            source.write(b"int main(void) { return 0; }\n")
        auto = subprocess.run([options.program, "--color=off", "--language=auto", path],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if auto.returncode == 0 and b"int main(void)" in auto.stdout:
            passed += 1
        else:
            failed += 1
            print("FAIL automatic language: %r %r" % (auto.stdout, auto.stderr))
    finally:
        if path is not None:
            os.unlink(path)

    invalid = run(options.program, ["--format=html", "--language=text"])
    if invalid.returncode != 0:
        passed += 1
    else:
        failed += 1
        print("FAIL --language accepted for HTML")

    invalid = run(options.program, ["--fence-style=off"])
    if invalid.returncode != 0:
        passed += 1
    else:
        failed += 1
        print("FAIL --fence-style accepted without --language")

    print("%d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
