# Native Markdown Instant View

The native Markdown Instant View skeleton is compiled only when Telegram Desktop is configured with `-D TDESKTOP_NATIVE_MARKDOWN_IV=ON`.

Configure and build the gated-on state from the repository root:

```bat
cmake -S . -B out -D TDESKTOP_NATIVE_MARKDOWN_IV=ON
cmake --build out --config Debug --target Telegram
```

The gated state now builds a cmark-gfm parser adapter, a value document model, a deterministic debug dump, and math extraction metadata. It also intercepts already-local `.md` and `.markdown` files and opens a minimal native diagnostic window. Full native Markdown rendering and MicroTeX output are still future work.
