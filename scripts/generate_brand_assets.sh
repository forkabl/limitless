#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_png="$repo_root/assets/brand/limitless.png"
output_icns="$repo_root/ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/AppIcon.icns"

if [ ! -f "$source_png" ]; then
  echo "Missing brand source: $source_png" >&2
  exit 1
fi

if ! command -v trash >/dev/null 2>&1; then
  echo "The trash command is required for safe temporary-file cleanup." >&2
  exit 1
fi

task_temp=$(mktemp -d "${TMPDIR:-/tmp}/limitless-brand.XXXXXX")
trap 'trash "$task_temp"' EXIT HUP INT TERM
iconset="$task_temp/AppIcon.iconset"
mkdir -p "$iconset"

# The supplied artwork includes generous presentation padding. Crop it to the
# macOS icon safe area before resizing so the mark remains recognizable in
# Finder's 16 px list view without clipping its glow at large sizes.
icon_master="$task_temp/limitless-icon-master.png"
/usr/bin/sips -c 2176 2176 "$source_png" --out "$icon_master" >/dev/null

make_icon() {
  pixels=$1
  filename=$2
  /usr/bin/sips -z "$pixels" "$pixels" "$icon_master" \
    --out "$iconset/$filename" >/dev/null
}

make_icon 16 icon_16x16.png
make_icon 32 icon_16x16@2x.png
make_icon 32 icon_32x32.png
make_icon 64 icon_32x32@2x.png
make_icon 128 icon_128x128.png
make_icon 256 icon_128x128@2x.png
make_icon 256 icon_256x256.png
make_icon 512 icon_256x256@2x.png
make_icon 512 icon_512x512.png
make_icon 1024 icon_512x512@2x.png

/usr/bin/iconutil -c icns "$iconset" -o "$output_icns"
echo "Generated $output_icns"
