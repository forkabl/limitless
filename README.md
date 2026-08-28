# Mori

Mori is an experimental native macOS browser chrome built into
[ungoogled-chromium](https://github.com/ungoogled-software/ungoogled-chromium).
SwiftUI and AppKit provide the interface, an Objective-C++ bridge connects it
to Chromium, and Chromium owns the browser engine and primitives.

Do not use Mori for sensitive browsing. Use a dedicated test profile and treat
site automation, injected CSS or JavaScript, and AI interactions as untrusted.

First-party source lives in:

```text
ungoogled-chromium-macos/build/src/chrome/browser/ui/mori/
```

The remaining Chromium checkout and build outputs are local dependencies and
are intentionally ignored by Git.

- [Build, run, and debug](BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Source layout](docs/SOURCE_LAYOUT.md)
