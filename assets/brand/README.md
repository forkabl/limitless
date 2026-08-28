# Limitless Brand Assets

`limitless.png` is the authoritative 2560 x 2560 full-color artwork supplied
for the Limitless rebrand. Do not overwrite or recompress it.

The tintable in-product mark is maintained as the reviewable vector source at:

```text
ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/glyphs/glyph-limitless.svg
```

Run `scripts/generate_brand_assets.sh` on macOS to regenerate `AppIcon.icns`
from the full-color source. The script creates every macOS icon rendition in a
temporary iconset before compiling the final resource with `iconutil`.

The initial monochrome concept was produced with the built-in image editor
using the full-color artwork as the shape reference. Because the editor did not
return a real alpha channel, the shipped glyph is a deterministic SVG
interpretation instead of a generated bitmap.
