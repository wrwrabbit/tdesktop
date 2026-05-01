# Native Markdown Instant View

This directory contains the native Markdown Instant View proof of concept behind `TDESKTOP_NATIVE_MARKDOWN_IV`.

Current scope:

- local `.md` / `.markdown` interception from the saved-document open flow
- UTF-8 validation plus explicit parser limits
- cmark-gfm parsing with tables, task lists, strikethrough, autolinks, tagfilter, and footnotes
- native prepare/layout/paint for paragraphs, lists, quotes, code blocks, tables, details blocks, and math
- MicroTeX-backed formula rendering with a preview-lifetime cache
- selection, copy, `Copy Link`, and `Open Link` actions inside the native preview

## Build

The feature is compiled only when `TDESKTOP_NATIVE_MARKDOWN_IV=ON`.

For the app build, use a Debug tree:

```bat
cmake --build out --config Debug --target Telegram
```

The regression probe is emitted only when the build tree is also configured with `DESKTOP_APP_TEST_APPS=ON`. In that case:

```bat
cmake --build out --config Debug --target test_markdown_iv
out\Debug\test_markdown_iv.exe
```

## Manual smoke test

1. Build a Debug app with `TDESKTOP_NATIVE_MARKDOWN_IV=ON`.
2. Launch Telegram Desktop from that build.
3. Save or download a local Markdown file.
4. Click the local `.md` or `.markdown` document from chat history.

Expected behavior:

- supported local Markdown opens in the native preview
- validation, parse, or unsupported-document rejection falls back to the normal file open path
- terminal post-open prepare failures show `Can't preview this Markdown file` with an `Open file` action

Useful manual checks:

- drag selection across multiple blocks
- copy inline math, display math, code blocks, and tables
- verify `Copy Link` / `Open Link` on external, anchor, footnote, and local-Markdown links
- verify rejected relative links do not expose an open action

## Regression target coverage

`test_markdown_iv` now exercises both parser and prepare/render layers:

- parses `markdown-example.md` and `latex-markdown-test.md`
- checks known inline/display formula counts
- keeps the currency, escaped-dollar, fenced-code, and inline-code exclusions
- asserts inline-formula `copySource`, display-math `formulaTex`, details/footnote preservation, and link classification
- verifies oversized-table flattening diagnostics
- runs a headless MicroTeX render/cache smoke pass twice through the same renderer and checks second-pass cache hits

## Limits and failure policy

Current hard limits:

- source bytes: 4 MiB
- cmark nodes: 100000
- nesting depth: 128
- formula bytes: 64 KiB
- formula count: 10000
- prepared blocks: 4096
- rendered table rows / columns / cells: 128 / 16 / 1024
- display-math logical render cap: 1600 x 1200
- formula image cap: 128 MiB physical image budget
- formula cache budget: 32 MiB per preview renderer

Policy:

- oversized or invalid sources reject before native open and fall back to normal file open
- oversized tables flatten into fallback blocks with diagnostics instead of failing the whole preview
- formula overflow or render failure falls back per formula and keeps the preview alive
- terminal prepare failures, including the prepared-block budget, switch the preview surface to the user-facing failure state

## Known gaps

- preview entry is local-file-only; message-bubble embedding is still future work
- details-body reparsing still falls back to plain paragraph text when nested formulas or footnotes would be introduced
- table interaction supports per-cell text selection and whole-table copy, but not arbitrary rectangular multi-cell selection
- relative links are accepted only when they resolve to safe local Markdown targets under the source directory
