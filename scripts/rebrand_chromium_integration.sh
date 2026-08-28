#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
checkout=${1:-"$repo_root/ungoogled-chromium-macos/build/src"}
legacy_title=$(printf '\115\157\162\151')
legacy_lower=$(printf '\155\157\162\151')
legacy_upper=$(printf '\115\117\122\111')

if [ ! -d "$checkout/chrome" ]; then
  echo "Chromium checkout is not populated: $checkout" >&2
  exit 1
fi

expected_revision=$(tr -d '[:space:]' <"$repo_root/chromium.version")
if [ -d "$checkout/.git" ]; then
  actual_revision=$(git -C "$checkout" rev-parse HEAD)
  if [ "$actual_revision" != "$expected_revision" ]; then
    echo "Chromium revision mismatch." >&2
    echo "Expected: $expected_revision" >&2
    echo "Actual:   $actual_revision" >&2
    exit 1
  fi
fi

overlay="$checkout/chrome/browser/ui/limitless"
build_file="$checkout/chrome/browser/ui/BUILD.gn"

if [ ! -f "$build_file" ]; then
  echo "Missing Chromium UI build file: $build_file" >&2
  echo "Restore the pinned Chromium checkout before applying integration changes." >&2
  exit 1
fi

# Rebrand the local Chromium integration that surrounds the tracked overlay.
# The full Chromium checkout remains ignored, so this transform is kept
# deterministic and idempotent for every developer checkout.
rg -l -0 -S "$legacy_title|$legacy_lower|$legacy_upper" \
  "$checkout/chrome" \
  -g '!browser/ui/limitless/**' \
  -g '!test/data/**' \
  -g '!*.pak' -g '!*.bin' -g '!*.car' -g '!*.icns' |
while IFS= read -r -d '' integration_file; do
  perl -pi -e \
    "s/$legacy_upper/LIMITLESS/g; s/$legacy_title/Limitless/g; s/$legacy_lower/limitless/g" \
    "$integration_file"
done

find "$checkout/chrome" -depth \
  \( -iname "*$legacy_lower*" \) -print0 |
while IFS= read -r -d '' old_path; do
  old_dir=$(dirname "$old_path")
  old_name=$(basename "$old_path")
  new_name=$(printf '%s' "$old_name" | sed \
    -e "s/$legacy_upper/LIMITLESS/g" \
    -e "s/$legacy_title/Limitless/g" \
    -e "s/$legacy_lower/limitless/g")
  if [ "$old_name" != "$new_name" ]; then
    mv "$old_path" "$old_dir/$new_name"
  fi
done

if [ ! -d "$overlay" ]; then
  echo "The tracked Limitless overlay is missing: $overlay" >&2
  exit 1
fi

if ! rg -q -F 'glyph-limitless.svg' "$build_file"; then
  if ! rg -q -F 'glyph-star.svg' "$build_file"; then
    echo "Could not locate the Limitless glyph resource list in $build_file" >&2
    exit 1
  fi
  perl -0pi -e \
    's|^(\s*)"([^"]*/glyph-)star(\.svg",\s*)$|$&\n$1"$2limitless$3|m' \
    "$build_file"
fi

if ! rg -q -F 'limitless_ui_swift' "$build_file"; then
  echo "The limitless_ui_swift target is missing from $build_file" >&2
  exit 1
fi

echo "Chromium integration uses Limitless names under $checkout/chrome"
echo "Review chrome/browser/ui/BUILD.gn and generated bundle metadata before building."
