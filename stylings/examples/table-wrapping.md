# Longer table content

| Component | Description | Notes |
|-----------|-------------|-------|
| Renderer | Converts Markdown into ANSI terminal output with configurable styling. | Designed for one-shot and progressive rendering. |
| Streaming | Re-renders the mutable tail while preserving committed output. | Supports bounded active regions and fixed-line viewports. |
| Highlighting | Uses libfyts and tree-sitter for fenced source blocks. | Language can be explicit or detected from a filename. |
