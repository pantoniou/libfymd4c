/* SPDX-License-Identifier: MIT */
/*
 * A blockquote with no blank row of its own (quoted paragraphs separated by
 * `> `) must freeze rows while it streams, not only when it ends.
 */
#include <stdio.h>
#include <string.h>

#include <libfymd4c.h>

int
main(void)
{
    struct fymd_renderer *r = fymd_renderer_create(NULL);
    struct fymd_update upd;
    size_t frozen = 0;
    char buf[256];
    int i;

    if(!r)
        return 1;
    for(i = 0; i < 40; i++) {
        snprintf(buf, sizeof(buf), "> **Title %d**\n> \n> Words about %d.\n> \n",
                 i, i);
        if(fymd_render_push(r, buf, strlen(buf), &upd))
            return 1;
        frozen += upd.freeze;
    }
    fymd_renderer_destroy(r);
    if(!frozen) {
        fprintf(stderr, "no row froze while the quote streamed\n");
        return 1;
    }
    return 0;
}
