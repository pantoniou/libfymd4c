#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>

static size_t
row_count(const char *s, size_t n)
{
    size_t i, rows = 0;
    for(i = 0; i < n; i++)
        if(s[i] == '\n')
            rows++;
    return rows + (n > 0 && s[n - 1] != '\n');
}

int
main(void)
{
    static const char input[] = "**not bold**\n```inside```\n";
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    struct fymd_fenced_block_opts opts;
    struct fymd_line_limit_opts limit;
    char *bare = NULL, *styled = NULL, *limited = NULL, *safe = NULL;
    size_t bare_len = 0, styled_len = 0, limited_len = 0, safe_len = 0;
    int failed = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_NO_COLOR;
    cfg.width = 40;
    r = fymd_renderer_create(&cfg);
    if(r == NULL)
        return 1;

    memset(&opts, 0, sizeof(opts));
    if(fymd_render_fenced_block(r, input, sizeof(input) - 1, &opts,
                                &bare, &bare_len) != 0 ||
       bare_len != sizeof(input) - 1 || memcmp(bare, input, bare_len) != 0)
        failed = 1;

    opts.language = "text";
    opts.flags = FYMD_FBF_STYLE;
    if(fymd_render_fenced_block(r, input, sizeof(input) - 1, &opts,
                                &styled, &styled_len) != 0 ||
       styled_len <= bare_len || strstr(styled, "**not bold**") == NULL ||
       strstr(styled, "```inside```") == NULL ||
       row_count(styled, styled_len) != row_count(bare, bare_len) + 2)
        failed = 1;

    memset(&limit, 0, sizeof(limit));
    limit.mode = FYMD_LLM_SCROLL;
    limit.max_lines = 1;
    if(fymd_renderer_set_line_limit(r, &limit) != 0 ||
       fymd_render_fenced_block(r, input, sizeof(input) - 1, &opts,
                                &limited, &limited_len) != 0 ||
       row_count(limited, limited_len) != 1)
        failed = 1;

    fymd_renderer_set_line_limit(r, NULL);
    opts.flags = 0;
    if(fymd_render_fenced_block(r, "ok\033[2J\n", sizeof("ok\033[2J\n") - 1, &opts,
                                &safe, &safe_len) != 0 ||
       safe_len != 3 || memcmp(safe, "ok\n", 3) != 0)
        failed = 1;

    fymd_free(bare);
    fymd_free(styled);
    fymd_free(limited);
    fymd_free(safe);
    fymd_renderer_destroy(r);
    if(failed)
        fprintf(stderr, "raw fenced-block API test failed\n");
    return failed;
}
