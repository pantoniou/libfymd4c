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
    char *bare = NULL, *styled = NULL, *highlighted = NULL;
    char *limited = NULL, *safe = NULL;
    size_t bare_len = 0, styled_len = 0, highlighted_len = 0;
    size_t limited_len = 0, safe_len = 0;
    int failed = 0;
    struct fy_generic_builder *gb = NULL;
    fy_generic vars = fy_invalid;
    fy_generic_sized_string vars_yaml;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = FYMD_RF_NO_COLOR;
    cfg.width = 40;
    r = fymd_renderer_create(&cfg);
    if(r == NULL)
        return 1;
    if(fymd_theme_count() != 9 ||
       strcmp(fymd_theme_name(0), "default") != 0 ||
       strcmp(fymd_theme_name(8), "tokyonight-borderless") != 0 ||
       fymd_theme_name(9) != NULL ||
       fymd_renderer_set_theme(r, "catppuccin") != 0 ||
       strcmp(fymd_renderer_get_cfg(r)->theme, "catppuccin") != 0 ||
       fymd_renderer_set_theme(r, "not-a-theme") == 0)
        failed = 1;

    memset(&opts, 0, sizeof(opts));
    if(fymd_render_fenced_block(r, input, sizeof(input) - 1, &opts,
                                &bare, &bare_len) != 0 ||
       bare_len != sizeof(input) - 1 || memcmp(bare, input, bare_len) != 0)
        failed = 1;

    opts.language = "c";
    opts.flags = FYMD_FBF_DEFAULT;
    if(fymd_render_fenced_block(r, "/*\n * line\n", sizeof("/*\n * line\n") - 1,
                                &opts, &highlighted, &highlighted_len) != 0 ||
       strstr(highlighted, "    /*\n     * line\n") == NULL)
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

    /* Arbitrary {key} values are borrowed from the caller's generic map. */
    gb = fy_generic_builder_create(NULL);
    vars_yaml.data = "who: world\nmark: '!'\nlanguage: override\n";
    vars_yaml.size = strlen(vars_yaml.data);
    if(gb != NULL)
        vars = fy_parse(gb, vars_yaml, FYOPPF_DEFAULT, NULL);
    if(gb == NULL || !fy_generic_is_valid(vars)) {
        failed = 1;
    } else {
        static const char theme[] =
            "code:\n  decoration:\n"
            "    header: 'Hello {who}{mark} {language}'\n"
            "    footer: ''\n    prefix: ''\n";
        struct fymd_renderer_cfg themed_cfg;
        struct fymd_renderer *themed;
        char *templated = NULL;
        size_t templated_len = 0;
        memset(&themed_cfg, 0, sizeof(themed_cfg));
        themed_cfg.flags = FYMD_RF_NO_COLOR;
        themed_cfg.width = 40;
        themed_cfg.style = theme;
        themed = fymd_renderer_create(&themed_cfg);
        memset(&opts, 0, sizeof(opts));
        opts.language = "c";
        opts.flags = FYMD_FBF_STYLE;
        opts.template_vars = vars;
        if(themed == NULL ||
           fymd_render_fenced_block(themed, "x\n", 2, &opts,
                                    &templated, &templated_len) != 0 ||
           strcmp(templated, "  Hello world! c\n  x\n") != 0)
            failed = 1;
        fymd_free(templated);
        fymd_renderer_destroy(themed);
    }

    /* Renderer-owned template counters include decoration and row limiting. */
    {
        static const char theme[] =
            "code:\n  decoration:\n"
            "    header: ''\n"
            "    footer: '{lines}/{plain-lines}/{hidden-lines}'\n"
            "    prefix: ''\n";
        struct fymd_renderer_cfg themed_cfg;
        struct fymd_renderer *themed;
        char *counted = NULL;
        size_t counted_len = 0;

        memset(&themed_cfg, 0, sizeof(themed_cfg));
        themed_cfg.flags = FYMD_RF_NO_COLOR;
        themed_cfg.width = 40;
        themed_cfg.style = theme;
        themed = fymd_renderer_create(&themed_cfg);
        memset(&limit, 0, sizeof(limit));
        limit.mode = FYMD_LLM_SCROLL;
        limit.max_lines = 1;
        memset(&opts, 0, sizeof(opts));
        opts.flags = FYMD_FBF_STYLE;
        if(themed == NULL || fymd_renderer_set_line_limit(themed, &limit) != 0 ||
           fymd_render_fenced_block(themed, "one\ntwo\n", 8, &opts,
                                    &counted, &counted_len) != 0 ||
           strcmp(counted, "  3/2/2\n") != 0) {
            fprintf(stderr, "counter template output: %s\n",
                    counted != NULL ? counted : "<null>");
            failed = 1;
        }
        fymd_free(counted);
        fymd_renderer_destroy(themed);
    }

    fymd_free(bare);
    fymd_free(styled);
    fymd_free(highlighted);
    fymd_free(limited);
    fymd_free(safe);
    fymd_renderer_destroy(r);
    if(gb != NULL)
        fy_generic_builder_destroy(gb);
    if(failed)
        fprintf(stderr, "raw fenced-block API test failed\n");
    return failed;
}
