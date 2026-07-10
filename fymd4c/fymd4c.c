/*
 * libfymd4c command-line tool: render Markdown to ANSI terminal output (default),
 * HTML, or heal incomplete/streaming Markdown.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>

#include "md4c.h"          /* MD_FLAG_* dialect constants, MD_CHAR/MD_SIZE */
#include <libfymd4c.h>

#ifdef _WIN32
    #include <io.h>
    #define fymd_isatty(fd) _isatty(fd)
    #define fymd_fileno(f) _fileno(f)
    #define fymd_read(fd, buf, n) _read((fd), (buf), (unsigned)(n))
#else
    #include <unistd.h>
    #define fymd_isatty(fd) isatty(fd)
    #define fymd_fileno(f) fileno(f)
    #define fymd_read(fd, buf, n) read((fd), (buf), (n))
#endif


/* Default parser flags for the ANSI renderer: the standard md4c extensions the
 * renderer knows how to display. (Used only when no explicit dialect flags are
 * given; HTML output defaults to plain CommonMark instead.) */
#define FYMD_ANSI_DEFAULT_PARSER_FLAGS                                     \
    (MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | \
     MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS |       \
     MD_FLAG_UNDERLINE)

/* Output format. */
typedef enum {
    FORMAT_ANSI,
    FORMAT_HTML,
    FORMAT_HEAL
} OutputFormat;

/* General options. */
static OutputFormat output_format = FORMAT_ANSI;
static unsigned parser_flags = 0;   /* MD_FLAG_*; 0 => format-specific default */
static int want_heal = 0;
static int want_stat = 0;
static int want_replay_fuzz = 0;

/* ANSI output: color mode and table width. */
typedef enum { COLOR_AUTO, COLOR_ON, COLOR_OFF } ColorMode;
static ColorMode color_mode = COLOR_AUTO;
static int ansi_width = FYMD_WIDTH_AUTO; /* >0 fixed, 0 inf, <0 auto */

/* ANSI streaming (push) mode. */
enum stream_mode { STREAM_MODE_WHOLE, STREAM_MODE_LINE, STREAM_MODE_BYTE,
                   STREAM_MODE_SIZE };
static int want_stream = 0;
static int stream_chunk = 0;        /* push chunk size; 0 => default (undocumented) */
static int stream_mode = STREAM_MODE_SIZE;  /* how the input is chopped into pushes */
static int stream_delay_ms = 0;     /* pause between pushes (progressive viewing) */
static int want_stream_progressive = 0;
static int max_active_lines = 0;
static size_t max_lines = 0;
static enum fymd_line_limit_mode line_limit_mode = FYMD_LLM_SCROLL;
static enum fymd_line_split line_split = FYMD_LLS_BALANCED;
static size_t line_head = 0;
static const char* line_separator = NULL;
static int table_fit_content = 0;
static const char* style_path = NULL;
static enum fymd_background forced_bg = FYMD_BG_AUTO;
static enum fymd_sgr_input sgr_input = FYMD_SGR_STRIP;
static int forced_reverse = 0;

/* HTML output. */
/* Skip a leading UTF-8 BOM by default, matching the historical md2html. */
static unsigned html_flags = FYMD_HTML_SKIP_UTF8_BOM;     /* FYMD_HTML_* */
static int want_fullhtml = 0;
static int want_xhtml = 0;
static const char* html_title = NULL;
static const char* css_path = NULL;


/*********************************
 ***  Simple grow-able buffer  ***
 *********************************/

struct membuffer {
    char* data;
    size_t asize;
    size_t size;
};

static void
membuf_init(struct membuffer* buf, size_t new_asize)
{
    buf->size = 0;
    buf->asize = new_asize;
    buf->data = malloc(buf->asize);
    if(buf->data == NULL) {
        fprintf(stderr, "membuf_init: malloc() failed.\n");
        exit(1);
    }
}

static void
membuf_fini(struct membuffer* buf)
{
    if(buf->data)
        free(buf->data);
}

static void
membuf_grow(struct membuffer* buf, size_t new_asize)
{
    buf->data = realloc(buf->data, new_asize);
    if(buf->data == NULL) {
        fprintf(stderr, "membuf_grow: realloc() failed.\n");
        exit(1);
    }
    buf->asize = new_asize;
}

static void
membuf_append(struct membuffer* buf, const char* data, size_t size)
{
    if(buf->asize < buf->size + size)
        membuf_grow(buf, buf->size + buf->size / 2 + size);
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
}


/**********************
 ***  Main program  ***
 **********************/

static size_t
count_nl(const char* s, size_t n)
{
    size_t i, c = 0;
    for(i = 0; i < n; i++)
        if(s[i] == '\n') c++;
    return c;
}

/* Read the whole stream into buf. */
static void
read_all(FILE* in, struct membuffer* buf)
{
    size_t n;
    membuf_init(buf, 32 * 1024);
    while(1) {
        if(buf->size >= buf->asize)
            membuf_grow(buf, buf->asize + buf->asize / 2);
        n = fread(buf->data + buf->size, 1, buf->asize - buf->size, in);
        if(n == 0)
            break;
        buf->size += n;
    }
}

/* Streaming ANSI rendering via the progressive line-diff API (same logic as the former md2ansi
 * lineage). When `live` (a terminal in progressive mode), updates are applied
 * with cursor control; otherwise a virtual screen is reconstructed and written
 * at the end (deterministic; matches the one-shot render). */
/* Size of the next push chunk starting at data[off], per the selected mode:
 *   whole - the entire remaining input in one push,
 *   line  - up to and including the next newline (one source line),
 *   byte  - a single byte,
 *   size  - stream_chunk bytes (default: a large block).
 */
static size_t
next_chunk(const char* data, size_t size, size_t off)
{
    size_t rem = size - off;

    switch(stream_mode) {
    case STREAM_MODE_WHOLE:
        return rem;
    case STREAM_MODE_LINE: {
        const char* nl = (const char*) memchr(data + off, '\n', rem);
        return nl ? (size_t)(nl - (data + off)) + 1 : rem;
    }
    case STREAM_MODE_BYTE:
        return 1;
    default: {
        size_t want = stream_chunk > 0 ? (size_t) stream_chunk : 8192;
        return rem < want ? rem : want;
    }
    }
}

static void
stream_pause(void)
{
    struct timespec ts;

    if(stream_delay_ms <= 0)
        return;
    ts.tv_sec = stream_delay_ms / 1000;
    ts.tv_nsec = (long)(stream_delay_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int
process_ansi_stream(FILE* in, FILE* out, struct fymd_renderer* r, int live)
{
    struct membuffer input = {0};
    struct membuffer scr = {0};
    size_t active_rows = 0;
    size_t off;
    int ret = 0;

    /* Buffer the whole input, then replay it in mode-sized chunks. Buffering is
     * fine here: this is a rendering/debug tool, not an unbounded pipe. */
    read_all(in, &input);
    if(!live)
        membuf_init(&scr, 8192);

    for(off = 0; off < input.size; ) {
        struct fymd_update upd;
        size_t n = next_chunk(input.data, input.size, off);
        size_t b;

        if(fymd_render_push(r, input.data + off, n, &upd) != 0) { ret = -1; break; }
        off += n;

        b = (upd.backtrack > active_rows) ? active_rows : upd.backtrack;
        if(live) {
            if(b > 0) {
                char esc[32];
                int m = snprintf(esc, sizeof(esc), "\033[%uA\r\033[J", (unsigned) b);
                fwrite(esc, 1, (size_t) m, out);
            }
            if(upd.content_len > 0)
                fwrite(upd.content, 1, upd.content_len, out);
            fflush(out);
        } else {
            size_t pos = scr.size, k;
            for(k = 0; k < b && pos > 0; k++) {
                pos--;
                while(pos > 0 && scr.data[pos - 1] != '\n') pos--;
            }
            scr.size = pos;
            if(upd.content_len > 0)
                membuf_append(&scr, upd.content, upd.content_len);
        }
        active_rows -= b;
        active_rows += count_nl(upd.content, upd.content_len);
        active_rows = (upd.freeze >= active_rows) ? 0 : active_rows - upd.freeze;
        stream_pause();
    }

    /* Final flush: the healed end-state of the still-active region. Rewind over
     * the rows the last push drew (the active region shown on screen) so the
     * finish output replaces them rather than appending a duplicate below. */
    if(ret == 0) {
        const char* fin = NULL;
        size_t fin_len = 0;
        if(fymd_render_finish(r, &fin, &fin_len) != 0) {
            ret = -1;
        } else if(live) {
            if(active_rows > 0) {
                char esc[32];
                int m = snprintf(esc, sizeof(esc), "\033[%uA\r\033[J",
                                 (unsigned) active_rows);
                fwrite(esc, 1, (size_t) m, out);
            }
            if(fin_len > 0)
                fwrite(fin, 1, fin_len, out);
            fflush(out);
        } else {
            size_t pos = scr.size, k;
            for(k = 0; k < active_rows && pos > 0; k++) {
                pos--;
                while(pos > 0 && scr.data[pos - 1] != '\n') pos--;
            }
            scr.size = pos;
            if(fin_len > 0)
                membuf_append(&scr, fin, fin_len);
        }
    }

    if(!live && ret == 0)
        fwrite(scr.data, 1, scr.size, out);

    membuf_fini(&scr);
    membuf_fini(&input);
    return ret;
}

static void
write_fullhtml_head(FILE* out)
{
    if(want_xhtml) {
        fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(out, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
                        "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n");
        fprintf(out, "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n");
    } else {
        fprintf(out, "<!DOCTYPE html>\n");
        fprintf(out, "<html>\n");
    }
    fprintf(out, "<head>\n");
    fprintf(out, "<title>%s</title>\n", html_title ? html_title : "");
    fprintf(out, "<meta name=\"generator\" content=\"fymd4c\"%s>\n", want_xhtml ? " /" : "");
#if !defined MD4C_USE_ASCII && !defined MD4C_USE_UTF16
    fprintf(out, "<meta charset=\"UTF-8\"%s>\n", want_xhtml ? " /" : "");
#endif
    if(css_path != NULL)
        fprintf(out, "<link rel=\"stylesheet\" href=\"%s\"%s>\n", css_path, want_xhtml ? " /" : "");
    fprintf(out, "</head>\n");
    fprintf(out, "<body>\n");
}

static int
process_file(const char* in_path, FILE* in, FILE* out, struct fymd_renderer* r)
{
    struct membuffer buf_in = {0};
    int ret = -1;
    clock_t t0, t1;
    unsigned p_flags = parser_flags;
    char* o = NULL;
    size_t olen = 0;

    /* Streaming ANSI mode reads input incrementally; handle before buffering. */
    if(output_format == FORMAT_ANSI && want_stream) {
        int live = want_stream_progressive && fymd_isatty(fymd_fileno(out));
        return process_ansi_stream(in, out, r, live);
    }

    read_all(in, &buf_in);

    /* Special mode for replaying fuzzer test cases: the first machine word is an
     * override for the parser flags, prepended to the document. */
    if(want_replay_fuzz) {
        if(buf_in.size < sizeof(unsigned)) {
            fprintf(stderr, "File %s isn't a valid fuzz test case.\n", in_path);
            goto out;
        }
        memcpy(&p_flags, buf_in.data, sizeof(unsigned));
        memmove(buf_in.data, buf_in.data + sizeof(unsigned),
                buf_in.size - sizeof(unsigned));
        buf_in.size -= sizeof(unsigned);
    }

    t0 = clock();
    switch(output_format) {
        case FORMAT_ANSI:
            ret = fymd_render(r, buf_in.data, buf_in.size, &o, &olen);
            break;
        case FORMAT_HTML:
            ret = fymd_render_html(buf_in.data, buf_in.size, p_flags, html_flags, &o, &olen);
            break;
        case FORMAT_HEAL:
            ret = fymd_heal(buf_in.data, buf_in.size, &o, &olen);
            break;
    }
    t1 = clock();

    if(ret != 0) {
        fprintf(stderr, "Rendering failed.\n");
        goto out;
    }

    if(output_format == FORMAT_HTML && want_fullhtml)
        write_fullhtml_head(out);
    fwrite(o, 1, olen, out);
    if(output_format == FORMAT_HTML && want_fullhtml)
        fprintf(out, "</body>\n</html>\n");

    if(want_stat && t0 != (clock_t)-1 && t1 != (clock_t)-1) {
        double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;
        if(elapsed < 1)
            fprintf(stderr, "Time spent on parsing: %7.2f ms.\n", elapsed*1e3);
        else
            fprintf(stderr, "Time spent on parsing: %6.3f s.\n", elapsed);
    }

    ret = 0;

out:
    fymd_free(o);
    membuf_fini(&buf_in);
    return ret;
}


/* Long-only options get value codes past the byte range so they never collide
 * with the short-option characters. */
enum {
    OPT_HEAL = 256,
    OPT_COLOR,
    OPT_WIDTH,
    OPT_TABLE_SIZE,
    OPT_STYLE,
    OPT_BACKGROUND,
    OPT_SGR,
    OPT_REVERSE,
    OPT_STREAM,
    OPT_STREAM_PROGRESSIVE,
    OPT_MAX_ACTIVE_LINES,
    OPT_MAX_LINES,
    OPT_LINE_OVERFLOW,
    OPT_LINE_HEAD,
    OPT_LINE_SEPARATOR,
    OPT_STREAM_CHUNK,
    OPT_STREAM_MODE,
    OPT_STREAM_DELAY,
    OPT_REPLAY_FUZZ,

    /* HTML / dialect */
    OPT_HTML_TITLE,
    OPT_HTML_CSS,
    OPT_COMMONMARK,
    OPT_GFM,
    OPT_FADMONITIONS,
    OPT_FCOLLAPSE_WHITESPACE,
    OPT_FFOOTNOTES,
    OPT_FHARD_SOFT_BREAKS,
    OPT_FHIGHLIGHT,
    OPT_FLATEX_MATH,
    OPT_FPERMISSIVE_ATX_HEADERS,
    OPT_FPERMISSIVE_AUTOLINKS,
    OPT_FPERMISSIVE_EMAIL_AUTOLINKS,
    OPT_FPERMISSIVE_URL_AUTOLINKS,
    OPT_FPERMISSIVE_WWW_AUTOLINKS,
    OPT_FSPOILERS,
    OPT_FSTRIKETHROUGH,
    OPT_FSUBSCRIPTS,
    OPT_FSUPERSCRIPTS,
    OPT_FTABLES,
    OPT_FTASKLISTS,
    OPT_FUNDERLINE,
    OPT_FVERBATIM_ENTITIES,
    OPT_FWIKI_LINKS,
    OPT_FNO_HTML_BLOCKS,
    OPT_FNO_HTML_SPANS,
    OPT_FNO_HTML,
    OPT_FNO_INDENTED_CODE
};

static const struct option long_options[] = {
    { "output",             required_argument, NULL, 'o' },
    { "format",             required_argument, NULL, 't' },
    { "stat",               no_argument,       NULL, 's' },
    { "help",               no_argument,       NULL, 'h' },
    { "version",            no_argument,       NULL, 'v' },
    { "heal",               no_argument,       NULL, OPT_HEAL },

    /* ANSI */
    { "color",              required_argument, NULL, OPT_COLOR },
    { "width",              required_argument, NULL, OPT_WIDTH },
    { "table-size",         required_argument, NULL, OPT_TABLE_SIZE },
    { "style",              required_argument, NULL, OPT_STYLE },
    { "background",         required_argument, NULL, OPT_BACKGROUND },
    { "sgr",                required_argument, NULL, OPT_SGR },
    { "reverse",            no_argument,       NULL, OPT_REVERSE },
    { "stream",             no_argument,       NULL, OPT_STREAM },
    { "stream-progressive", no_argument,       NULL, OPT_STREAM_PROGRESSIVE },
    { "max-active-lines",   required_argument, NULL, OPT_MAX_ACTIVE_LINES },
    { "max-lines",          required_argument, NULL, OPT_MAX_LINES },
    { "line-overflow",      required_argument, NULL, OPT_LINE_OVERFLOW },
    { "line-head",          required_argument, NULL, OPT_LINE_HEAD },
    { "line-separator",     required_argument, NULL, OPT_LINE_SEPARATOR },
    { "stream-chunk",       required_argument, NULL, OPT_STREAM_CHUNK },
    { "stream-mode",        required_argument, NULL, OPT_STREAM_MODE },
    { "stream-delay",       required_argument, NULL, OPT_STREAM_DELAY },

    /* HTML */
    { "full-html",          no_argument,       NULL, 'f' },
    { "xhtml",              no_argument,       NULL, 'x' },
    { "html-title",         required_argument, NULL, OPT_HTML_TITLE },
    { "html-css",           required_argument, NULL, OPT_HTML_CSS },

    /* Markdown dialect / parser flags (apply to ansi and html). */
    { "commonmark",                  no_argument, NULL, OPT_COMMONMARK },
    { "github",                      no_argument, NULL, OPT_GFM },
    { "gfm",                         no_argument, NULL, OPT_GFM },
    { "fadmonitions",                no_argument, NULL, OPT_FADMONITIONS },
    { "fcollapse-whitespace",        no_argument, NULL, OPT_FCOLLAPSE_WHITESPACE },
    { "ffootnotes",                  no_argument, NULL, OPT_FFOOTNOTES },
    { "fhard-soft-breaks",           no_argument, NULL, OPT_FHARD_SOFT_BREAKS },
    { "fhighlight",                  no_argument, NULL, OPT_FHIGHLIGHT },
    { "flatex-math",                 no_argument, NULL, OPT_FLATEX_MATH },
    { "fpermissive-atx-headers",     no_argument, NULL, OPT_FPERMISSIVE_ATX_HEADERS },
    { "fpermissive-autolinks",       no_argument, NULL, OPT_FPERMISSIVE_AUTOLINKS },
    { "fpermissive-email-autolinks", no_argument, NULL, OPT_FPERMISSIVE_EMAIL_AUTOLINKS },
    { "fpermissive-url-autolinks",   no_argument, NULL, OPT_FPERMISSIVE_URL_AUTOLINKS },
    { "fpermissive-www-autolinks",   no_argument, NULL, OPT_FPERMISSIVE_WWW_AUTOLINKS },
    { "fspoilers",                   no_argument, NULL, OPT_FSPOILERS },
    { "fstrikethrough",              no_argument, NULL, OPT_FSTRIKETHROUGH },
    { "fsubscripts",                 no_argument, NULL, OPT_FSUBSCRIPTS },
    { "fsuperscripts",               no_argument, NULL, OPT_FSUPERSCRIPTS },
    { "ftables",                     no_argument, NULL, OPT_FTABLES },
    { "ftasklists",                  no_argument, NULL, OPT_FTASKLISTS },
    { "funderline",                  no_argument, NULL, OPT_FUNDERLINE },
    { "fverbatim-entities",          no_argument, NULL, OPT_FVERBATIM_ENTITIES },
    { "fwiki-links",                 no_argument, NULL, OPT_FWIKI_LINKS },
    { "fno-html-blocks",             no_argument, NULL, OPT_FNO_HTML_BLOCKS },
    { "fno-html-spans",              no_argument, NULL, OPT_FNO_HTML_SPANS },
    { "fno-html",                    no_argument, NULL, OPT_FNO_HTML },
    { "fno-indented-code",           no_argument, NULL, OPT_FNO_INDENTED_CODE },

    /* Undocumented: streaming push chunk size / fuzz replay (for tests). */
    { "replay-fuzz",        no_argument,       NULL, OPT_REPLAY_FUZZ },

    { NULL, 0, NULL, 0 }
};

static void
usage(void)
{
    printf(
        "Usage: fymd4c [OPTION]... [FILE]\n"
        "Render input FILE (or standard input) in Markdown format.\n"
        "\n"
        "General options:\n"
        "  -o, --output=FILE    Output file (default is standard output)\n"
        "  -t, --format=FORMAT  Output format: ansi (default), html, heal\n"
        "      --heal           Heal incomplete markdown before ANSI rendering\n"
        "  -s, --stat           Measure time of input parsing\n"
        "  -h, --help           Display this help and exit\n"
        "  -v, --version        Display version and exit\n"
        "\n"
        "ANSI output options (--format=ansi, the default):\n"
        "      --color=MODE     Color output: auto (default), on, off\n"
        "      --width=WIDTH    Table width: auto (default), inf, or a column count\n"
        "      --table-size=MODE  Table sizing: fill width (default) or fit to content\n"
        "      --style=FILE     YAML styling config (overrides the built-in default)\n"
        "      --background=MODE  Background for light/dark styles: auto (default), dark, light\n"
        "      --sgr=MODE       Input ANSI escapes: off (default, strip), on (pass), safe (SGR only)\n"
        "      --reverse        Render the whole document as a card (background filled to width)\n"
        "      --stream         Render incrementally (push mode)\n"
        "      --stream-progressive  Live progressive render; updates the active region in place\n"
        "      --max-active-lines=N  Cap the streaming active region to N input lines (0 = unlimited)\n"
        "      --max-lines=N    Cap output to N rendered rows (0 = unlimited)\n"
        "      --line-overflow=MODE  Overflow policy: scroll (default) or head-tail\n"
        "      --line-head=N|balanced  Head/tail allocation for head-tail mode\n"
        "      --line-separator=FORMAT  Omission row with one %%d conversion\n"
        "      --stream-mode=MODE  How input is chopped into pushes: whole, line, byte (implies --stream)\n"
        "      --stream-chunk=N    Push N bytes at a time (implies --stream)\n"
        "      --stream-delay=MS   Pause MS milliseconds between pushes (watch the reconstruction)\n"
        "\n"
        "HTML output options (--format=html):\n"
        "  -f, --full-html      Generate a full HTML document, including header\n"
        "  -x, --xhtml          Generate XHTML instead of HTML\n"
        "      --html-title=TITLE  Set the document title (full-html)\n"
        "      --html-css=URL   Add a stylesheet link (full-html)\n"
        "      --fverbatim-entities  Do not translate entities\n"
        "\n"
        "Markdown dialect options (apply to ansi and html):\n"
        "      --commonmark     CommonMark (default for html)\n"
        "      --gfm, --github  GitHub Flavored Markdown\n"
        "      --ftables --ftasklists --fstrikethrough --funderline --fwiki-links\n"
        "      --flatex-math --ffootnotes --fhighlight --fspoilers --fsubscripts\n"
        "      --fsuperscripts --fadmonitions --fcollapse-whitespace --fhard-soft-breaks\n"
        "      --fpermissive-autolinks (and -email/-url/-www variants)\n"
        "      --fpermissive-atx-headers\n"
        "      --fno-html-blocks --fno-html-spans --fno-html --fno-indented-code\n"
        "\n"
    );
}

static void
version(void)
{
    printf("%d.%d.%d (libfymd4c %s)\n", MD_VERSION_MAJOR, MD_VERSION_MINOR,
           MD_VERSION_RELEASE, fymd_library_version());
}

static const char* input_path = NULL;
static const char* output_path = NULL;

static void
parse_args(int argc, char** argv)
{
    int c;

    while((c = getopt_long(argc, argv, "o:t:sfxhv", long_options, NULL)) != -1) {
        switch(c) {
            case 'o':   output_path = optarg; break;
            case 's':   want_stat = 1; break;
            case 'h':   usage(); exit(0); break;
            case 'v':   version(); exit(0); break;
            case 'f':   want_fullhtml = 1; break;
            case 'x':   want_xhtml = 1; html_flags |= FYMD_HTML_XHTML; break;
            case OPT_HEAL:        want_heal = 1; break;
            case OPT_REPLAY_FUZZ: want_replay_fuzz = 1; break;
            case OPT_REVERSE:     forced_reverse = 1; break;
            case OPT_STREAM:      want_stream = 1; break;
            case OPT_STREAM_PROGRESSIVE:
                want_stream = 1; want_stream_progressive = 1; break;
            case OPT_STYLE:       style_path = optarg; break;
            case OPT_HTML_TITLE:  html_title = optarg; break;
            case OPT_HTML_CSS:    css_path = optarg; break;

            case 't':
                if(strcmp(optarg, "ansi") == 0)
                    output_format = FORMAT_ANSI;
                else if(strcmp(optarg, "html") == 0)
                    output_format = FORMAT_HTML;
                else if(strcmp(optarg, "heal") == 0)
                    output_format = FORMAT_HEAL;
                else {
                    fprintf(stderr, "Unknown format: %s\n", optarg);
                    fprintf(stderr, "Supported formats: ansi, html, heal\n");
                    exit(1);
                }
                break;

            case OPT_COLOR:
                if(strcmp(optarg, "auto") == 0)
                    color_mode = COLOR_AUTO;
                else if(strcmp(optarg, "on") == 0 || strcmp(optarg, "always") == 0)
                    color_mode = COLOR_ON;
                else if(strcmp(optarg, "off") == 0 || strcmp(optarg, "never") == 0)
                    color_mode = COLOR_OFF;
                else {
                    fprintf(stderr, "Invalid --color value: %s (use auto, on, or off)\n", optarg);
                    exit(1);
                }
                break;

            case OPT_TABLE_SIZE:
                if(strcmp(optarg, "fit") == 0)
                    table_fit_content = 1;
                else if(strcmp(optarg, "fill") == 0)
                    table_fit_content = 0;
                else {
                    fprintf(stderr, "Invalid --table-size value: %s (use fit or fill)\n", optarg);
                    exit(1);
                }
                break;

            case OPT_BACKGROUND:
                if(strcmp(optarg, "dark") == 0)        forced_bg = FYMD_BG_DARK;
                else if(strcmp(optarg, "light") == 0)  forced_bg = FYMD_BG_LIGHT;
                else if(strcmp(optarg, "auto") == 0)   forced_bg = FYMD_BG_AUTO;
                else {
                    fprintf(stderr, "Invalid --background value: %s (use auto, dark, light)\n", optarg);
                    exit(1);
                }
                break;

            case OPT_SGR:
                if(strcmp(optarg, "off") == 0)       sgr_input = FYMD_SGR_STRIP;
                else if(strcmp(optarg, "on") == 0)   sgr_input = FYMD_SGR_KEEP;
                else if(strcmp(optarg, "safe") == 0) sgr_input = FYMD_SGR_SAFE;
                else {
                    fprintf(stderr, "Invalid --sgr value: %s (use off, on, safe)\n", optarg);
                    exit(1);
                }
                break;

            case OPT_MAX_ACTIVE_LINES: {
                long n = atol(optarg);
                if(n < 0) {
                    fprintf(stderr, "Invalid --max-active-lines value: %s\n", optarg);
                    exit(1);
                }
                max_active_lines = (int) n;
                break;
            }

            case OPT_MAX_LINES: {
                char* end = NULL;
                unsigned long n = strtoul(optarg, &end, 10);
                if(optarg[0] == '-' || end == optarg || *end != '\0' ||
                   (unsigned long)(size_t)n != n) {
                    fprintf(stderr, "Invalid --max-lines value: %s\n", optarg);
                    exit(1);
                }
                max_lines = (size_t) n;
                break;
            }

            case OPT_LINE_OVERFLOW:
                if(strcmp(optarg, "scroll") == 0)
                    line_limit_mode = FYMD_LLM_SCROLL;
                else if(strcmp(optarg, "head-tail") == 0)
                    line_limit_mode = FYMD_LLM_HEAD_TAIL;
                else {
                    fprintf(stderr, "Invalid --line-overflow value: %s "
                            "(use scroll or head-tail)\n", optarg);
                    exit(1);
                }
                break;

            case OPT_LINE_HEAD:
                line_limit_mode = FYMD_LLM_HEAD_TAIL;
                if(strcmp(optarg, "balanced") == 0) {
                    line_split = FYMD_LLS_BALANCED;
                    line_head = 0;
                } else {
                    char* end = NULL;
                    unsigned long n = strtoul(optarg, &end, 10);
                    if(optarg[0] == '-' || end == optarg || *end != '\0' ||
                       (unsigned long)(size_t)n != n) {
                        fprintf(stderr, "Invalid --line-head value: %s\n", optarg);
                        exit(1);
                    }
                    line_split = FYMD_LLS_HEAD_COUNT;
                    line_head = (size_t) n;
                }
                break;

            case OPT_LINE_SEPARATOR:
                line_limit_mode = FYMD_LLM_HEAD_TAIL;
                line_separator = optarg;
                break;

            case OPT_STREAM_CHUNK:
                stream_chunk = atoi(optarg);
                if(stream_chunk < 1) {
                    fprintf(stderr, "Invalid --stream-chunk value: %s\n", optarg);
                    exit(1);
                }
                stream_mode = STREAM_MODE_SIZE;
                want_stream = 1;
                break;

            case OPT_STREAM_MODE:
                if(strcmp(optarg, "whole") == 0)      stream_mode = STREAM_MODE_WHOLE;
                else if(strcmp(optarg, "line") == 0)  stream_mode = STREAM_MODE_LINE;
                else if(strcmp(optarg, "byte") == 0)  stream_mode = STREAM_MODE_BYTE;
                else {
                    fprintf(stderr, "Invalid --stream-mode value: %s "
                            "(use whole, line, or byte)\n", optarg);
                    exit(1);
                }
                want_stream = 1;
                break;

            case OPT_STREAM_DELAY: {
                long n = atol(optarg);
                if(n < 0) {
                    fprintf(stderr, "Invalid --stream-delay value: %s\n", optarg);
                    exit(1);
                }
                stream_delay_ms = (int) n;
                break;
            }

            case OPT_WIDTH:
                if(strcmp(optarg, "auto") == 0) {
                    ansi_width = FYMD_WIDTH_AUTO;
                } else if(strcmp(optarg, "inf") == 0) {
                    ansi_width = FYMD_WIDTH_INF;
                } else {
                    char* end = NULL;
                    long w = strtol(optarg, &end, 10);
                    if(end == optarg || *end != '\0' || w < 0 || w > 100000) {
                        fprintf(stderr, "Invalid --width value: %s (use auto, inf, 0, or a column count)\n", optarg);
                        exit(1);
                    }
                    ansi_width = (int) w;
                }
                break;

            case OPT_COMMONMARK:    parser_flags |= MD_DIALECT_COMMONMARK; break;
            case OPT_GFM:           parser_flags |= MD_DIALECT_GITHUB; break;
            case OPT_FADMONITIONS:              parser_flags |= MD_FLAG_ADMONITIONS; break;
            case OPT_FCOLLAPSE_WHITESPACE:      parser_flags |= MD_FLAG_COLLAPSEWHITESPACE; break;
            case OPT_FFOOTNOTES:                parser_flags |= MD_FLAG_FOOTNOTES; break;
            case OPT_FHARD_SOFT_BREAKS:         parser_flags |= MD_FLAG_HARD_SOFT_BREAKS; break;
            case OPT_FHIGHLIGHT:                parser_flags |= MD_FLAG_HIGHLIGHT; break;
            case OPT_FLATEX_MATH:               parser_flags |= MD_FLAG_LATEXMATHSPANS; break;
            case OPT_FPERMISSIVE_ATX_HEADERS:   parser_flags |= MD_FLAG_PERMISSIVEATXHEADERS; break;
            case OPT_FPERMISSIVE_AUTOLINKS:     parser_flags |= MD_FLAG_PERMISSIVEAUTOLINKS; break;
            case OPT_FPERMISSIVE_EMAIL_AUTOLINKS: parser_flags |= MD_FLAG_PERMISSIVEEMAILAUTOLINKS; break;
            case OPT_FPERMISSIVE_URL_AUTOLINKS: parser_flags |= MD_FLAG_PERMISSIVEURLAUTOLINKS; break;
            case OPT_FPERMISSIVE_WWW_AUTOLINKS: parser_flags |= MD_FLAG_PERMISSIVEWWWAUTOLINKS; break;
            case OPT_FSPOILERS:                 parser_flags |= MD_FLAG_SPOILERS; break;
            case OPT_FSTRIKETHROUGH:            parser_flags |= MD_FLAG_STRIKETHROUGH; break;
            case OPT_FSUBSCRIPTS:               parser_flags |= MD_FLAG_SUBSCRIPTS; break;
            case OPT_FSUPERSCRIPTS:             parser_flags |= MD_FLAG_SUPERSCRIPTS; break;
            case OPT_FTABLES:                   parser_flags |= MD_FLAG_TABLES; break;
            case OPT_FTASKLISTS:                parser_flags |= MD_FLAG_TASKLISTS; break;
            case OPT_FUNDERLINE:                parser_flags |= MD_FLAG_UNDERLINE; break;
            case OPT_FWIKI_LINKS:               parser_flags |= MD_FLAG_WIKILINKS; break;
            case OPT_FVERBATIM_ENTITIES:        html_flags |= FYMD_HTML_VERBATIM_ENTITIES; break;
            case OPT_FNO_HTML_BLOCKS:           parser_flags |= MD_FLAG_NOHTMLBLOCKS; break;
            case OPT_FNO_HTML_SPANS:            parser_flags |= MD_FLAG_NOHTMLSPANS; break;
            case OPT_FNO_HTML:                  parser_flags |= MD_FLAG_NOHTML; break;
            case OPT_FNO_INDENTED_CODE:         parser_flags |= MD_FLAG_NOINDENTEDCODEBLOCKS; break;

            case '?':   /* getopt_long already printed the diagnostic */
            default:
                fprintf(stderr, "Use --help for more info.\n");
                exit(1);
        }
    }

    if(optind < argc) {
        input_path = argv[optind++];
        if(optind < argc) {
            fprintf(stderr, "Too many arguments. Only one input file can be specified.\n");
            fprintf(stderr, "Use --help for more info.\n");
            exit(1);
        }
    }
}

int
main(int argc, char** argv)
{
    FILE* in = stdin;
    FILE* out = stdout;
    struct fymd_renderer* r = NULL;
    struct fymd_renderer_cfg cfg;
    int ret = 0;
    int use_color;
    struct fymd_line_limit_opts limit_opts;

    parse_args(argc, argv);

    if(input_path != NULL && strcmp(input_path, "-") != 0) {
        in = fopen(input_path, "rb");
        if(in == NULL) {
            fprintf(stderr, "Cannot open %s.\n", input_path);
            exit(1);
        }
    }
    if(output_path != NULL && strcmp(output_path, "-") != 0) {
        out = fopen(output_path, "wt");
        if(out == NULL) {
            fprintf(stderr, "Cannot open %s.\n", output_path);
            exit(1);
        }
    }

    /* Resolve color: auto = enabled only when output is a terminal. */
    if(color_mode == COLOR_ON)
        use_color = 1;
    else if(color_mode == COLOR_OFF)
        use_color = 0;
    else
        use_color = fymd_isatty(fymd_fileno(out));

    /* Build the ANSI renderer once. Its parser flags default to the renderer's
     * rich set only when no explicit dialect flag was given. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.style_path = style_path;
    cfg.width = ansi_width;
    cfg.max_active_lines = max_active_lines;
    cfg.parser_flags = parser_flags ? parser_flags : FYMD_ANSI_DEFAULT_PARSER_FLAGS;
    cfg.background = forced_bg;
    cfg.sgr_input = sgr_input;
    if(!use_color)          cfg.flags |= FYMD_RF_NO_COLOR;
    if(table_fit_content)   cfg.flags |= FYMD_RF_TABLE_FIT;
    if(want_heal)           cfg.flags |= FYMD_RF_HEAL;
    if(forced_reverse)      cfg.flags |= FYMD_RF_REVERSE;

    r = fymd_renderer_create(&cfg);
    if(r == NULL) {
        fprintf(stderr, "Cannot create renderer (styling%s%s).\n",
                style_path ? " from " : "", style_path ? style_path : "");
        exit(1);
    }

    memset(&limit_opts, 0, sizeof(limit_opts));
    limit_opts.mode = max_lines ? line_limit_mode : FYMD_LLM_NONE;
    limit_opts.max_lines = max_lines;
    limit_opts.split = line_split;
    limit_opts.head_lines = line_head;
    limit_opts.separator_format = line_separator;
    if(fymd_renderer_set_line_limit(r, &limit_opts) != 0) {
        fprintf(stderr, "Invalid rendered-line limit configuration.\n");
        fymd_renderer_destroy(r);
        exit(1);
    }

    ret = process_file((input_path != NULL) ? input_path : "<stdin>", in, out, r);
    if(in != stdin)
        fclose(in);
    if(out != stdout)
        fclose(out);
    fymd_renderer_destroy(r);

    return ret;
}
