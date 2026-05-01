# Native Markdown IV Report

Status: production-direction PoC hardening completed with the expanded prepare/render test target kept in place.

## Verification snapshot

Local verification was run in Debug trees with `TDESKTOP_NATIVE_MARKDOWN_IV=ON`:

- build: `out`, target `Telegram`, result pass
- executable: `out/Debug/test_markdown_iv.exe`, result pass
- throwaway `out_phase5/Debug/test_markdown_iv.exe`, result pass before cleanup
- MicroTeX backend: linked

## Explicit limits

- source bytes: 4 MiB
- cmark nodes: 100000
- nesting depth: 128
- formula bytes: 64 KiB
- formula count: 10000
- prepared blocks: 4096
- rendered table rows: 128
- rendered table columns: 16
- rendered table cells: 1024
- display-math logical render cap: 1600 x 1200
- formula physical image cap: 128 MiB
- formula cache budget: 32 MiB per preview renderer

## Failure behavior

- Pre-open validation or parse rejection returns control to the normal file-open path.
- Unsupported or empty Markdown documents also fall back before native preview open.
- Post-open terminal prepare failures, including the prepared-block budget, switch the preview surface to `Can't preview this Markdown file` and expose `Open file`.
- Oversized tables flatten into fallback blocks and increment prepare warnings instead of failing the preview.
- Formula overflow or render failure stays local to the formula slot and uses fallback text / overflow styling instead of aborting the document.

## Selection and copy scope

- drag selection works across multiple prepared segments in document order
- inline formulas copy their original `$...$` source through `copySource`
- display math copies as `$$...$$`
- code blocks copy the raw prepared block text, not visually wrapped lines
- tables support per-cell text selection and whole-table copy serialization
- context menus expose `Copy Text` / `Copy Selected Text`, `Copy Link`, and `Open Link`
- rejected relative links and details toggles intentionally do not expose open actions

## Measured debug counters

From the local `test_markdown_iv.exe` run:

- `markdown-example.md`: `prepare_ms=18`, `formula_ms=14`, `prepare_warnings=0`, `formula_warnings=0`, `prepared_formulas=2`
- `latex-markdown-test.md`: `prepare_ms=342`, `formula_ms=340`, `prepare_warnings=0`, `formula_warnings=0`, `prepared_formulas=124`
- cache smoke, first pass: `hits=6`, `misses=120`
- cache smoke, second pass through the same renderer: `hits=126`, `misses=0`
- cache usage after second pass: `1651956` bytes

The regression target also exercises a synthetic display-math render-cap failure by forcing `displayMathMaxRenderWidth = 1` and verifying that prepare completes with formula warnings and a nonterminal fallback result. It separately builds a parsed document that exceeds the prepared-block budget and verifies the real terminal `prepared-block-limit` failure path.

## Regression target coverage

`test_markdown_iv` now covers:

- parser validation and both shipped fixtures
- known inline/display formula counts
- currency, escaped-dollar, fenced-code, and inline-code exclusions
- inline-formula `copySource`
- display-math `formulaTex`
- prepared table structure and oversize-table flatten diagnostics
- prepared-block terminal failure behavior
- details-block preservation
- footnote references, backlinks, and bottom-list preservation
- safe local-Markdown-link classification versus rejected relative links
- MicroTeX render/cache reuse across repeated prepare passes

## Known unsupported or intentionally deferred cases

- native preview entry is still limited to local files; message-bubble embedding is not implemented
- details-body reparsing degrades to plain paragraph text when nested formulas or footnotes would need a second prepared subdocument
- table selection is not a spreadsheet-style rectangular model; whole-table copy is the supported fallback
- formula cache reuse is scoped to a preview renderer lifetime, not shared globally across preview windows

## Next steps for message-bubble embedding

- split the local-file controller surface from the reusable document-view surface more explicitly
- define a bubble-friendly width policy and table overflow policy for chat layout
- add preview-root creation from message media data instead of the local-file resolver only
- decide whether preview renderers should share a broader formula cache across bubbles or windows
