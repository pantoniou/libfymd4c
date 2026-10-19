/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <libfymd4c.h>

int main(void)
{
    static const struct {
        const char *off;
        const char *wire;
        int restore;
    } cases[] = {
        { "\\e[49m", "\x1b[49m", 1 },
        { "\\e[39;49m", "\x1b[39;49m", 1 },
        { "\\e[0;1m", "\x1b[0;1m", 1 },
        { "\\e[m", "\x1b[m", 1 },
        { "\\e[39m", "\x1b[39m", 0 },
        { "\\e[38;2;49;0;0m", "\x1b[38;2;49;0;0m", 0 },
        { "\\e[48;2;49;0;0m", "\x1b[48;2;49;0;0m", 0 },
        { "\\e[0;48;2;1;2;3m", "\x1b[0;48;2;1;2;3m", 0 },
    };
    static const char bg[] = "\x1b[48;2;40;42;46m";
    static const char md[] = "> **monitor** `load2` ended\n";
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    char style[512], expected[128], *out;
    size_t i;
    int failed = 0;

    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(style, sizeof(style),
            "elements:\n"
            "  reverse: {on: \"\\e[48;2;40;42;46m\", off: \"\\e[0m\"}\n"
            "  code: {on: \"\\e[48;2;10;20;30m\", off: \"%s\"}\n",
            cases[i].off);
        memset(&cfg, 0, sizeof(cfg));
        cfg.flags = FYMD_RF_REVERSE;
        cfg.background = FYMD_BG_DARK;
        cfg.width = 80;
        cfg.style = style;
        r = fymd_renderer_create(&cfg);
        if(r == NULL)
            return 1;
        out = fymd_render_to_string(r, md, sizeof(md) - 1);
        snprintf(expected, sizeof(expected), "load2%s%s ended",
                 cases[i].wire, cases[i].restore ? bg : "");
        if(out == NULL || strstr(out, expected) == NULL) {
            fprintf(stderr, "card background after %s\n", cases[i].off);
            failed = 1;
        }
        fymd_free(out);
        fymd_renderer_destroy(r);
    }
    return failed;
}
