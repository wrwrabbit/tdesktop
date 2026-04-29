# Native Markdown Instant View

The native Markdown Instant View skeleton is compiled only when Telegram Desktop is configured with `-D TDESKTOP_NATIVE_MARKDOWN_IV=ON`.

Configure and build the gated-on state from the repository root:

```bat
cmake -S . -B out -D TDESKTOP_NATIVE_MARKDOWN_IV=ON
cmake --build out --config Debug --target Telegram
```

The current skeleton is inert. It adds API and source structure plus dependency linkage only, with no file-opening hook, UI entry point, WebView Instant View change, localization, settings, or real Markdown/MicroTeX rendering.
