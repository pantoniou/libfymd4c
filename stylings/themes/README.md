# Bundled themes

These overlays coordinate Markdown elements with the identically named libfyts
syntax theme. They support dark and light backgrounds and add subtle complete-row
table styling.

```sh
./fymd4c/fymd4c --color=on \
  --theme=catppuccin \
  ../stylings/themes/showcase.md
```

Available themes are `catppuccin`, `kanagawa`, `solarized`, and `tokyonight`.
Append `-borderless` to any name for the same palette with grid-free tables.
Select a variant explicitly with `--background=dark` or `--background=light`;
the default `auto` mode follows the normal background detection.
