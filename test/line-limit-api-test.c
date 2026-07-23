#include <stdio.h>
#include <string.h>

#include <libfymd4c.h>

static size_t
rows(const char *s, size_t n)
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
    return row ? "--" : "@@";
}

int
main(void)
{
    struct fymd_renderer *r = fymd_renderer_create(NULL);
    struct fymd_renderer_cfg chrome_cfg;
    struct fymd_renderer *chrome;
    struct fymd_line_limit_opts opts;
    struct fymd_update upd;
    char *out = NULL;
    size_t out_len = 0;
    int failed = 0;

    if(r == NULL)
        return 1;
    memset(&opts, 0, sizeof(opts));
    opts.mode = FYMD_LLM_SCROLL;
    opts.max_lines = 2;
    if(fymd_renderer_set_line_limit(r, &opts) != 0 ||
       fymd_render(r, "a\n\nb\n\nc\n", 8, &out, &out_len) != 0 ||
       rows(out, out_len) != 2)
        failed = 1;
    fymd_free(out);

    memset(&chrome_cfg, 0, sizeof(chrome_cfg));
    chrome_cfg.width = 12;
    chrome = fymd_renderer_create(&chrome_cfg);
    out = NULL;
    out_len = 0;
    if(chrome == NULL ||
       fymd_render_with_margins(chrome, "alpha beta gamma", 16, margin, NULL,
                                &out, &out_len) != 0 ||
       memcmp(out, "@@alpha", 7) != 0 ||
       strstr(out, "\n--beta") == NULL)
        failed = 1;
    fymd_free(out);
    fymd_renderer_destroy(chrome);

    if(fymd_render_push(r, "a\n", 2, &upd) != 0 ||
       fymd_renderer_set_line_limit(r, NULL) == 0)
        failed = 1;
    fymd_render_reset(r);
    if(fymd_renderer_set_line_limit(r, NULL) != 0)
        failed = 1;

    memset(&opts, 0, sizeof(opts));
    opts.mode = FYMD_LLM_HEAD_TAIL;
    opts.max_lines = 2;
    opts.split = FYMD_LLS_BALANCED;
    if(fymd_renderer_set_line_limit(r, &opts) == 0)
        failed = 1;
    opts.max_lines = 5;
    opts.separator_format = "missing conversion";
    if(fymd_renderer_set_line_limit(r, &opts) == 0)
        failed = 1;

    fymd_renderer_destroy(r);
    if(failed)
        fprintf(stderr, "line-limit public API test failed\n");
    return failed;
}
