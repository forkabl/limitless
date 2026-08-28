# Limitless

Limitless is an experimental native macOS browser shell built into Chromium.
SwiftUI and AppKit provide the interface, an Objective-C++ bridge connects it
to Chromium, and Chromium owns the browser engine and primitives.

Do not use Limitless for sensitive browsing. Use a dedicated test profile and
treat site automation, injected CSS or JavaScript, and AI interactions as
untrusted.

First-party source lives in:

```text
ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/
```

The remaining Chromium checkout and build outputs are local dependencies and
are intentionally ignored by Git.

- [Build, run, and debug](BUILDING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Source layout](docs/SOURCE_LAYOUT.md)
- [Product identity contract](config/limitless-product.json)
- [Brand asset sources](assets/brand/README.md)
