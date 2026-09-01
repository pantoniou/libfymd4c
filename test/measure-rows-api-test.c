/* fymd_measure_rows() must agree with the rows the real render emits, for
 * every block kind (the row counter used to miss newlines written straight to
 * the output: code blocks, tables, rules) and for margined renders. */
#include <stdio.h>
#include <string.h>

#include <libfymd4c.h>

static const char *docs[] = {
    "",
    "a",
    "hello world\n",
    "# Heading\n\ntext\n",
    "```c\nint main(void){return 0;}\nreturn 0;\n```\n",
    "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n",
    "---\n\npara\n\n---\n",
    "> quoted\n> lines\n\nafter\n",
    "- one\n- two\n  - nested\n\n1. first\n2. second\n",
    "a very long paragraph that must wrap across several physical rows "
        "because the renderer width is far narrower than this sentence is\n",
    "> ```sh\n> echo hi\n> ```\n\n| x |\n|---|\n| `y` |\n",
};

static size_t
count_rows(const char *s, size_t n)
{
    size_t i, count = 0;
    for(i = 0; i < n; i++)
        if(s[i] == '\n')
            count++;
    return count + (n > 0 && s[n - 1] != '\n');
}

static const char *
margin(void *userdata, size_t row)
{
    (void)userdata;
    return row ? ">>>>" : "@";
}

/* Measure and render the same document; report a mismatch. */
static int
check(struct fymd_renderer *r, const char *md, fymd_margin_fn fn, const char *what)
{
    size_t len = strlen(md), measured = 0, actual;
    char *out = NULL;
    size_t out_len = 0;
    int rc;

    if(fn != NULL)
        rc = fymd_measure_rows_with_margins(r, md, len, fn, NULL, &measured);
    else
        rc = fymd_measure_rows(r, md, len, &measured);
    if(rc != 0) {
        fprintf(stderr, "%s: measure failed\n", what);
        return 1;
    }
    if(fn != NULL)
        rc = fymd_render_with_margins(r, md, len, fn, NULL, &out, &out_len);
    else
        rc = fymd_render(r, md, len, &out, &out_len);
    if(rc != 0) {
        fprintf(stderr, "%s: render failed\n", what);
        return 1;
    }
    actual = count_rows(out, out_len);
    fymd_free(out);
    if(measured != actual) {
        fprintf(stderr, "%s: measured %zu, rendered %zu\n", what, measured, actual);
        return 1;
    }
    return 0;
}

/* Run the corpus through one renderer, plain and margined. */
static int
check_all(struct fymd_renderer *r, const char *what)
{
    size_t i;
    int failed = 0;
    char label[128];

    for(i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        snprintf(label, sizeof(label), "%s doc %zu", what, i);
        failed |= check(r, docs[i], NULL, label);
        snprintf(label, sizeof(label), "%s doc %zu (margins)", what, i);
        failed |= check(r, docs[i], margin, label);
    }
    return failed;
}

int
main(void)
{
    static const int widths[] = { 20, 40, 80 };
    struct fymd_renderer_cfg cfg;
    struct fymd_line_limit_opts opts;
    struct fymd_renderer *r;
    size_t i, measured = 0;
    int failed = 0;

    for(i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        char what[64];
        memset(&cfg, 0, sizeof(cfg));
        cfg.width = widths[i];
        r = fymd_renderer_create(&cfg);
        if(r == NULL)
            return 1;
        snprintf(what, sizeof(what), "width %d", widths[i]);
        failed |= check_all(r, what);

        /* A viewport clamps the render; the measurement must follow it. */
        memset(&opts, 0, sizeof(opts));
        opts.mode = FYMD_LLM_SCROLL;
        opts.max_lines = 3;
        if(fymd_renderer_set_line_limit(r, &opts) != 0)
            failed = 1;
        snprintf(what, sizeof(what), "width %d, limited", widths[i]);
        failed |= check_all(r, what);
        fymd_renderer_destroy(r);
    }

    /* Argument validation, and NULL margin_fn == the plain entry point. */
    r = fymd_renderer_create(NULL);
    if(r == NULL)
        return 1;
    if(fymd_measure_rows_with_margins(r, "a", 1, NULL, NULL, NULL) == 0 ||
       fymd_measure_rows_with_margins(NULL, "a", 1, NULL, NULL, &measured) == 0)
        failed = 1;
    failed |= check(r, "# hi\n\nthere\n", NULL, "default renderer");
    fymd_renderer_destroy(r);

    if(failed)
        fprintf(stderr, "measure-rows-api-test FAILED\n");
    return failed ? 1 : 0;
}
