#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from subprocess import Popen, PIPE
import platform
import os

def pipe_through_prog(argv, text):
    p1 = Popen(argv, stdout=PIPE, stdin=PIPE, stderr=PIPE)
    [result, err] = p1.communicate(input=text.encode('utf-8'))
    return [p1.returncode, result.decode('utf-8'), err]

class Prog:
    def __init__(self, cmdline="fymd4c -t html", default_options=[]):
        self.cmdline = cmdline.split()
        # The per-test dialect options are additive requirements (e.g. --ftables),
        # so always append them. The base cmdline may carry a fixed prefix such as
        # "fymd4c -t html" (formerly a single md2html token).
        if isinstance(default_options, str):
            self.cmdline += default_options.split()
        else:
            self.cmdline += default_options
        self.to_html = lambda x: pipe_through_prog(self.cmdline, x)
