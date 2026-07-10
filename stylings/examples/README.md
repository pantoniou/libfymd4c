# Table styling examples

These are partial themes: unspecified settings use libfymd4c defaults.

```sh
build/fymd4c/fymd4c --style=stylings/examples/table-ascii.yaml \
  stylings/examples/table-basic.md

build/fymd4c/fymd4c --style=stylings/examples/table-heavy.yaml \
  stylings/examples/table-alignment.md

build/fymd4c/fymd4c --style=stylings/examples/table-colored-header.yaml \
  --color=on stylings/examples/table-basic.md

build/fymd4c/fymd4c --style=stylings/examples/table-borderless.yaml \
  --width=60 stylings/examples/table-wrapping.md

build/fymd4c/fymd4c --style=stylings/examples/table-striped.yaml \
  --color=on stylings/examples/table-basic.md
```

`table-borderless.yaml` uses the true `table.border: none` policy: vertical
grid glyphs and the header separator are omitted while spacing and row styling
are retained.

`table-striped.yaml` demonstrates complete-row styling: a reverse header and
alternating body-row backgrounds. Row backgrounds cover padding and separators.
