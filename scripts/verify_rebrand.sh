#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

legacy_title=$(printf '\115\157\162\151')
legacy_lower=$(printf '\155\157\162\151')
legacy_upper=$(printf '\115\117\122\111')
failed=0

fail() {
  echo "error: $*" >&2
  failed=1
}

if rg -n --hidden -S "$legacy_title|$legacy_lower|$legacy_upper" \
  -g '!.git/**' -g '!*.icns' -g '!*.car' .; then
  fail "legacy brand text remains in first-party files"
fi

if find . -path './.git' -prune -o -print | grep -i "$legacy_lower"; then
  fail "legacy brand text remains in a first-party path"
fi

asset_catalog="ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/Assets.car"
if strings "$asset_catalog" | grep -i "$legacy_lower"; then
  fail "legacy brand text remains in Assets.car"
fi

if ! assetutil -Z "$asset_catalog" >/dev/null; then
  fail "Assets.car failed its integrity check"
fi

required_files='assets/brand/limitless.png
chromium.version
patches/chromium/limitless-integration.patch
ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/AppIcon.icns
ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/glyphs/glyph-limitless.svg
config/limitless-product.json'
printf '%s\n' "$required_files" | while IFS= read -r required_file; do
  if [ ! -f "$required_file" ]; then
    echo "error: missing required brand file: $required_file" >&2
    exit 1
  fi
done || failed=1

if [ "$(jq -r '.productName' config/limitless-product.json)" != "Limitless" ]; then
  fail "the product contract has the wrong product name"
fi
if [ "$(jq -r '.bundleIdentifier' config/limitless-product.json)" != "com.limitless.browser" ]; then
  fail "the product contract has the wrong bundle identifier"
fi
if [ "$(jq -r '.minimumMacOSVersion' config/limitless-product.json)" != "14.0" ]; then
  fail "the product contract has the wrong minimum macOS version"
fi
if [ "$(jq -r '.chromiumRevision' config/limitless-product.json)" != \
    "$(tr -d '[:space:]' <chromium.version)" ]; then
  fail "the product contract and Chromium revision pin disagree"
fi

if ! git diff --check; then
  fail "the worktree contains whitespace errors"
fi

if [ "$#" -gt 0 ]; then
  app_path=$1
  info_plist="$app_path/Contents/Info.plist"
  if [ ! -f "$info_plist" ]; then
    fail "missing app Info.plist: $info_plist"
  else
    bundle_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$info_plist" 2>/dev/null || true)
    executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$info_plist" 2>/dev/null || true)
    display_name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleDisplayName' "$info_plist" 2>/dev/null || \
      /usr/libexec/PlistBuddy -c 'Print :CFBundleName' "$info_plist" 2>/dev/null || true)
    minimum_system_version=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$info_plist" 2>/dev/null || true)

    [ "$bundle_id" = "com.limitless.browser" ] || fail "unexpected CFBundleIdentifier: $bundle_id"
    [ "$executable" = "Limitless" ] || fail "unexpected CFBundleExecutable: $executable"
    [ "$display_name" = "Limitless" ] || fail "unexpected app display name: $display_name"
    [ "$minimum_system_version" = "14.0" ] || \
      fail "unexpected LSMinimumSystemVersion: $minimum_system_version"
    [ -x "$app_path/Contents/MacOS/Limitless" ] || fail "the Limitless executable is missing"
    [ -d "$app_path/Contents/Frameworks/Limitless Framework.framework" ] || \
      fail "the Limitless framework bundle is missing"
    [ -f "$app_path/Contents/Resources/LimitlessGlyphs/glyph-limitless.svg" ] || \
      fail "the bundled Limitless glyph library is missing"

    find "$app_path/Contents" -name Info.plist -print0 |
    while IFS= read -r -d '' nested_plist; do
      nested_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$nested_plist" 2>/dev/null || true)
      case "$nested_id" in
        com.limitless.browser*) ;;
        *)
          echo "error: unexpected nested bundle identifier $nested_id in $nested_plist" >&2
          exit 1
          ;;
      esac
    done || failed=1

    if find "$app_path" -print | grep -i "$legacy_lower"; then
      fail "legacy brand text remains in the packaged app paths"
    fi
    if ! /usr/bin/codesign --verify --deep --strict --verbose=2 "$app_path"; then
      fail "the packaged app failed code-signing verification"
    fi
  fi
fi

if [ "$failed" -ne 0 ]; then
  exit 1
fi

echo "Limitless rebrand verification passed."
