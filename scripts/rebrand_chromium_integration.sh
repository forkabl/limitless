#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -ne 1 ]; then
  echo "Usage: $0 /path/to/chromium/src" >&2
  exit 2
fi

checkout=$1
overlay_source="$repo_root/ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless"
overlay_target="$checkout/chrome/browser/ui/limitless"
integration_patch="$repo_root/patches/chromium/limitless-integration.patch"

if [ ! -d "$checkout/.git" ] || [ ! -d "$checkout/chrome" ]; then
  echo "Chromium checkout is not populated: $checkout" >&2
  exit 1
fi
if [ ! -d "$overlay_source" ]; then
  echo "Tracked Limitless overlay is missing: $overlay_source" >&2
  exit 1
fi
if [ ! -f "$integration_patch" ]; then
  echo "Chromium integration patch is missing: $integration_patch" >&2
  exit 1
fi

expected_revision=$(tr -d '[:space:]' <"$repo_root/chromium.version")
actual_revision=$(git -C "$checkout" rev-parse HEAD)
if [ "$actual_revision" != "$expected_revision" ]; then
  echo "Chromium revision mismatch." >&2
  echo "Expected: $expected_revision" >&2
  echo "Actual:   $actual_revision" >&2
  exit 1
fi

if git -C "$checkout" apply --reverse --check "$integration_patch" 2>/dev/null; then
  echo "Chromium integration patch is already applied."
elif git -C "$checkout" apply --check "$integration_patch"; then
  git -C "$checkout" apply "$integration_patch"
else
  echo "Chromium checkout has incompatible changes in integration files." >&2
  exit 1
fi

mkdir -p "$overlay_target"
/usr/bin/ditto "$overlay_source" "$overlay_target"
/usr/bin/ditto "$overlay_source/AppIcon.icns" \
  "$checkout/chrome/app/theme/chromium/mac/app.icns"
/usr/bin/ditto "$overlay_source/Assets.car" \
  "$checkout/chrome/app/theme/chromium/mac/Assets.car"

for required_path in \
  "$overlay_target/LimitlessRoot.swift" \
  "$overlay_target/limitless_browser_window.mm" \
  "$overlay_target/glyphs/glyph-limitless.svg" \
  "$checkout/chrome/app/theme/chromium/mac/app.icns" \
  "$checkout/chrome/app/theme/chromium/mac/Assets.car"; do
  if [ ! -f "$required_path" ]; then
    echo "Prepared checkout is missing: $required_path" >&2
    exit 1
  fi
done

echo "Prepared Limitless integration in $checkout"
echo "Run gn gen with mac_min_system_version set to 14.0."
