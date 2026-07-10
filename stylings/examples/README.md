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
```

`table-borderless.yaml` suppresses the current grid glyphs but retains table
layout and padding. It can become a true borderless policy when configurable
table border policies are added.
