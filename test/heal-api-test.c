#include <stdio.h>
#include <string.h>

#include <libfymd4c.h>

struct heal_case {
    const char *in;
    const char *want;
};

/* Heal closes a dangling marker only when that marker can actually OPEN
 * emphasis. A run the parser never consumes -- "a ** b" (whitespace on both
 * sides), or a trailing "_" that can only close -- must be left alone: a
 * closer appended there renders as literal marker text and hangs off the end
 * of the last rendered line. */
static const struct heal_case cases[] = {
    /* dangling openers: healed */
    { "use *emph",           "use *emph*"        },
    { "use **strong",        "use **strong**"    },
    { "a ***both",           "a ***both***"      },
    { "plain _ital",         "plain _ital_"      },
    { "x __strong",          "x __strong__"      },
    { "**bold** and *it",    "**bold** and *it*" },
    /* not openers: left alone */
    { "checking a ** b now", "checking a ** b now" },
    { "use more_ later",     "use more_ later"     },
    { "snake_case_word",     "snake_case_word"     },
    { "a * b",               "a * b"               },
    /* already balanced: left alone */
    { "all *done* here",     "all *done* here"     },
};

int
main(void)
{
    size_t i;
    int failed = 0;

    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *out = NULL;
        size_t out_len = 0;

        if(fymd_heal(cases[i].in, strlen(cases[i].in), &out, &out_len) != 0) {
            fprintf(stderr, "heal failed for %s\n", cases[i].in);
            failed = 1;
            continue;
        }
        if(out_len != strlen(cases[i].want) ||
           memcmp(out, cases[i].want, out_len) != 0) {
            fprintf(stderr, "heal %s: got <%.*s>, want <%s>\n", cases[i].in,
                    (int) out_len, out, cases[i].want);
            failed = 1;
        }
        fymd_free(out);
    }

    if(failed)
        fprintf(stderr, "heal public API test failed\n");
    return failed;
}
