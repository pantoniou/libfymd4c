/*
 * fymd-ui.c - render UI Markdown and report its clickable regions
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libfymd4c.h>
#ifdef MD4C_WITH_FYPALETTE
#include <libfypalette.h>
#endif

static const char demo[] =
    "## Work <fy-slot id=\"spin\" width=\"1\"/>"
    "<fy-fill/><fy-role name=\"chrome\">3 files</fy-role>\n"
    "\n"
    "<fy-scroll anchor=\"bottom\">\n"
    "\n"
    "* <fy-act id=\"open:foo.c\">foo.c</fy-act><fy-fill/>+10, -2\n"
    "* <fy-act id=\"open:bar.c\">bar.c</fy-act><fy-fill/>+1, -40\n"
    "* <fy-act id=\"open:baz.c\">baz.c</fy-act><fy-fill/>new\n"
    "\n"
    "</fy-scroll>\n"
    "\n"
    "<fy-columns widths=\"*,*\" gap=\"3\">\n"
    "<fy-col>\n"
    "\n"
    "**Plan.** Cache the last token, not the last line, and keep the scan "
    "state in one owner.\n"
    "\n"
    "</fy-col>\n"
    "<fy-col>\n"
    "\n"
    "| test | result |\n"
    "|---|---|\n"
    "| parse | <fy-act id=\"test:parse\">pass</fy-act> |\n"
    "| scan | <fy-act id=\"test:scan\">fail</fy-act> |\n"
    "\n"
    "</fy-col>\n"
    "</fy-columns>\n"
    "\n"
    "<fy-slot id=\"preview\"/>\n"
    "\n"
    "<fy-vfill/>\n"
    "\n"
    "<fy-glyph name=\"gutter.tool\" fallback=\"->\"/> "
    "<fy-act id=\"cmd:run\">run</fy-act><fy-fill/>"
    "<fy-act id=\"cmd:quit\">quit</fy-act>\n";

/* The Markdown that the preview slot renders with a renderer of its own. */
static const char preview[] =
    "> **foo.c** <fy-fill/><fy-act id=\"diff:foo.c\">diff</fy-act>\n";

/* Regions of a nested render, in the coordinates of that render. */
struct nested_region {
    char slot[64];
    char id[64];
    size_t row;
    int col;
    int width;
};

struct slot_ctx {
    int color;
    enum fymd_palette_flags palette_flags;
    void *palette;
    struct nested_region nested[16];
    size_t nnested;
};

static const char *
no_margin(void *userdata, size_t row)
{
    (void) userdata;
    (void) row;
    return "";
}

/* Draw a slot: a spinner frame, or a preview rendered recursively. */
static int
slot_render(void *userdata, const char *id, int width, int height,
            enum fymd_block_flags flags, fymd_block_emit_fn emit, void *emit_ctx)
{
    struct slot_ctx *sc = userdata;
    const struct fymd_region *rg;
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *inner;
    char *out = NULL;
    size_t len = 0, count, i;

    (void) height;
    if(!strcmp(id, "spin")) {
        emit(emit_ctx, "*", 1);
        return 0;
    }
    if(strcmp(id, "preview") != 0)
        return -1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_DEFAULT | FYMD_RF_UI |
                ((flags & FYMD_BF_NO_COLOR) ? FYMD_RF_NO_COLOR : 0);
    cfg.width = width + 2;          /* the inner right margin */
    cfg.background = FYMD_BG_DARK;
    inner = fymd_renderer_create(&cfg);
    if(inner == NULL)
        return -1;
#ifdef MD4C_WITH_FYPALETTE
    if(sc->palette != NULL)
        (void) fymd_renderer_set_palette_flags(inner, sc->palette,
                                               sc->palette_flags);
#endif
    if(fymd_render_with_margins(inner, preview, sizeof(preview) - 1,
                                no_margin, NULL, &out, &len) != 0) {
        fymd_renderer_destroy(inner);
        return -1;
    }
    /* keep the inner regions to place them once the slot has a position */
    if(fymd_renderer_get_regions(inner, &rg, &count) == 0)
        for(i = 0; i < count && sc->nnested < 16; i++) {
            struct nested_region *nr = &sc->nested[sc->nnested++];
            snprintf(nr->slot, sizeof(nr->slot), "%s", id);
            snprintf(nr->id, sizeof(nr->id), "%s", rg[i].id);
            nr->row = rg[i].row;
            nr->col = rg[i].col;
            nr->width = rg[i].width;
        }
    while(len > 0 && out[len - 1] == '\n')
        len--;
    emit(emit_ctx, out, len);
    fymd_free(out);
    fymd_renderer_destroy(inner);
    return 0;
}

static void
usage(FILE *fp, const char *prog)
{
    fprintf(fp,
        "usage: %s [options] [FILE]\n"
        "Render UI Markdown (the built-in demo without FILE; - reads stdin).\n"
        "  -w, --width=COLS       columns (default 60)\n"
        "  -H, --height=ROWS      rows of the vertical layout (default none)\n"
        "  -c, --color=on|off     colour (default: on for a terminal)\n"
        "  -p, --palette=THEME    colours, roles and glyphs of a palette theme\n"
        "  -l, --light            the light palette variant\n"
        "  -a, --ascii            the ASCII glyph forms\n"
        "  -n, --no-ui            render without FYMD_RF_UI, for comparison\n"
        "  -r, --regions          print the regions after the output\n"
        "  -k, --click=ROW,COL    print the region at ROW,COL (0-based)\n"
        "  -h, --help             this text\n", prog);
}

static char *
read_all(FILE *fp, size_t *len)
{
    char *buf = NULL, *nb;
    size_t size = 0, alloc = 0, n;

    for(;;) {
        if(alloc - size < 4096) {
            alloc = alloc ? alloc * 2 : 8192;
            nb = realloc(buf, alloc);
            if(nb == NULL) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        n = fread(buf + size, 1, alloc - size, fp);
        size += n;
        if(n == 0)
            break;
    }
    *len = size;
    return buf;
}

int
main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "width",   required_argument, NULL, 'w' },
        { "height",  required_argument, NULL, 'H' },
        { "color",   required_argument, NULL, 'c' },
        { "palette", required_argument, NULL, 'p' },
        { "light",   no_argument,       NULL, 'l' },
        { "ascii",   no_argument,       NULL, 'a' },
        { "no-ui",   no_argument,       NULL, 'n' },
        { "regions", no_argument,       NULL, 'r' },
        { "click",   required_argument, NULL, 'k' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct fymd_renderer_cfg cfg;
    const struct fymd_region *regions;
    struct fymd_renderer *r;
    struct slot_ctx slots;
    const char *palette_name = NULL, *id;
    char *src = NULL, *out = NULL;
    size_t len, out_len, count = 0, i, j, click_row = 0;
    const char *nested_hit = NULL;
    int width = 60, height = 0, color = -1, light = 0, ascii = 0, no_ui = 0;
    int show_regions = 0, click = 0, click_col = 0, opt, rc = 1;
    FILE *fp;
#ifdef MD4C_WITH_FYPALETTE
    struct fypal_ctx *palette = NULL;
    struct fypal_caps caps;
#endif

    while((opt = getopt_long(argc, argv, "w:H:c:p:lanrk:h", opts, NULL)) != -1) {
        switch(opt) {
        case 'w': width = atoi(optarg); break;
        case 'H': height = atoi(optarg); break;
        case 'c': color = strcmp(optarg, "off") != 0; break;
        case 'p': palette_name = optarg; break;
        case 'l': light = 1; break;
        case 'a': ascii = 1; break;
        case 'n': no_ui = 1; break;
        case 'r': show_regions = 1; break;
        case 'k':
            if(sscanf(optarg, "%zu,%d", &click_row, &click_col) != 2) {
                fprintf(stderr, "--click takes ROW,COL\n");
                return 2;
            }
            click = 1;
            break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: usage(stderr, argv[0]); return 2;
        }
    }
    if(color < 0)
        color = isatty(STDOUT_FILENO);

    if(optind < argc) {
        fp = strcmp(argv[optind], "-") ? fopen(argv[optind], "rb") : stdin;
        if(fp == NULL) {
            perror(argv[optind]);
            return 1;
        }
        src = read_all(fp, &len);
        if(fp != stdin)
            fclose(fp);
    } else {
        len = sizeof(demo) - 1;
        src = malloc(len);
        if(src != NULL)
            memcpy(src, demo, len);
    }
    if(src == NULL) {
        fprintf(stderr, "cannot read the document\n");
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_DEFAULT | (no_ui ? 0 : FYMD_RF_UI) |
                (color ? 0 : FYMD_RF_NO_COLOR);
    cfg.width = width;
    cfg.background = light ? FYMD_BG_LIGHT : FYMD_BG_DARK;
    r = fymd_renderer_create(&cfg);
    if(r == NULL) {
        fprintf(stderr, "cannot create a renderer\n");
        goto out;
    }
    memset(&slots, 0, sizeof(slots));
    slots.color = color;
    slots.palette_flags = ascii ? FYMD_PF_ASCII : 0;
    if(fymd_renderer_set_slot_renderer(r, slot_render, &slots) != 0) {
        fprintf(stderr, "cannot set the slot renderer\n");
        goto out;
    }
    if(palette_name != NULL) {
#ifdef MD4C_WITH_FYPALETTE
        fypal_caps_detect(STDOUT_FILENO, &caps);
        if(!color) {
            caps.depth = FYPAL_DEPTH_NONE;
            caps.attrs = 0;
        } else if(caps.depth == FYPAL_DEPTH_NONE) {
            caps.depth = FYPAL_DEPTH_TRUECOLOR;
            caps.attrs = FYPAL_ATTR_ALL & ~FYPAL_ATTR_UNDERCURL;
        }
        palette = fypal_ctx_create(&caps);
        if(palette != NULL)
            fypal_ctx_set_variant(palette, light ? FYPAL_VARIANT_LIGHT :
                                                   FYPAL_VARIANT_DARK);
        if(palette == NULL ||
           fypal_ctx_load_builtin(palette, palette_name) != 0 ||
           fymd_renderer_set_palette_flags(r, palette,
                                           ascii ? FYMD_PF_ASCII : 0) != 0) {
            slots.palette = NULL;
            fprintf(stderr, "cannot apply palette %s: %s\n", palette_name,
                    palette ? fypal_ctx_error(palette) : "no context");
            goto out;
        }
        slots.palette = palette;
#else
        (void) ascii;
        fprintf(stderr, "--palette: built without libfypalette\n");
        goto out;
#endif
    }
    if(fymd_renderer_set_height(r, height) != 0 ||
       fymd_render(r, src, len, &out, &out_len) != 0) {
        fprintf(stderr, "cannot render the document\n");
        goto out;
    }
    fwrite(out, 1, out_len, stdout);

    if((show_regions || click) &&
       fymd_renderer_get_regions(r, &regions, &count) != 0) {
        fprintf(stderr, "cannot read the regions\n");
        goto out;
    }
    if(show_regions)
        for(i = 0; i < count; i++)
            printf("region %s row=%zu col=%d width=%d height=%d kind=%s\n",
                   regions[i].id, regions[i].row, regions[i].col,
                   regions[i].width, regions[i].height,
                   regions[i].kind == FYMD_REGION_SLOT ? "slot" : "act");
    /* a nested region is placed at the position of its slot */
    for(j = 0; j < slots.nnested; j++) {
        const struct nested_region *nr = &slots.nested[j];
        for(i = 0; i < count; i++) {
            if(regions[i].kind != FYMD_REGION_SLOT || strcmp(regions[i].id, nr->slot))
                continue;
            if(show_regions)
                printf("region %s row=%zu col=%d width=%d height=1 kind=act slot=%s\n",
                       nr->id, regions[i].row + nr->row, regions[i].col + nr->col,
                       nr->width, nr->slot);
            if(click && click_row == regions[i].row + nr->row &&
               click_col >= regions[i].col + nr->col &&
               click_col < regions[i].col + nr->col + nr->width)
                nested_hit = nr->id;
        }
    }
    if(click) {
        id = nested_hit ? nested_hit : fymd_renderer_region_at(r, click_row, click_col);
        printf("click %zu,%d: %s\n", click_row, click_col, id ? id : "none");
    }
    rc = 0;
out:
    fymd_free(out);
    fymd_renderer_destroy(r);
#ifdef MD4C_WITH_FYPALETTE
    fypal_ctx_destroy(palette);
#endif
    free(src);
    return rc;
}
