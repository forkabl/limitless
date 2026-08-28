#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${LIMITLESS_BUILD_DIR:-"$repo_root/ungoogled-chromium-macos/build/src/out/Default"}
source_app="$build_dir/Limitless.app"
release_dir=${LIMITLESS_RELEASE_DIR:-"$repo_root/dist"}
version=${LIMITLESS_VERSION:-}
architecture=${LIMITLESS_ARCHITECTURE:-$(uname -m)}
signing_identity=${LIMITLESS_SIGNING_IDENTITY:-}
notary_profile=${LIMITLESS_NOTARY_PROFILE:-}

if [ -z "$version" ]; then
  echo "LIMITLESS_VERSION is required." >&2
  exit 1
fi
if [ -z "$signing_identity" ]; then
  echo "LIMITLESS_SIGNING_IDENTITY is required." >&2
  exit 1
fi
if [ -z "$notary_profile" ]; then
  echo "LIMITLESS_NOTARY_PROFILE is required." >&2
  exit 1
fi
if [ ! -d "$source_app" ]; then
  echo "Missing build product: $source_app" >&2
  exit 1
fi
if ! command -v trash >/dev/null 2>&1; then
  echo "The trash command is required for safe artifact replacement." >&2
  exit 1
fi

case "$version" in
  *[!A-Za-z0-9._-]*)
    echo "LIMITLESS_VERSION contains unsupported filename characters." >&2
    exit 1
    ;;
esac

task_temp=$(mktemp -d "${TMPDIR:-/tmp}/limitless-release.XXXXXX")
trap 'trash "$task_temp"' EXIT HUP INT TERM
staged_app="$task_temp/Limitless.app"
/usr/bin/ditto "$source_app" "$staged_app"

sign_path() {
  sign_target=$1
  entitlements=$(mktemp "$task_temp/entitlements.XXXXXX")
  if /usr/bin/codesign -d --entitlements :- "$sign_target" >"$entitlements" 2>/dev/null && \
      grep -q '<plist' "$entitlements"; then
    /usr/bin/codesign --force --timestamp --options runtime \
      --entitlements "$entitlements" --sign "$signing_identity" "$sign_target"
  else
    /usr/bin/codesign --force --timestamp --options runtime \
      --sign "$signing_identity" "$sign_target"
  fi
  trash "$entitlements"
}

# Sign nested code from the inside out, then seal every nested bundle and the
# main application. This supports Chromium helper apps, frameworks, XPC
# services, and dylibs without assuming a fixed helper inventory.
find "$staged_app/Contents" -type f -print0 | while IFS= read -r -d '' candidate; do
  if /usr/bin/file "$candidate" | grep -q 'Mach-O'; then
    sign_path "$candidate"
  fi
done

find "$staged_app/Contents" -depth -type d \
  \( -name '*.framework' -o -name '*.xpc' -o -name '*.appex' -o -name '*.app' \) \
  -print0 | while IFS= read -r -d '' nested_bundle; do
    sign_path "$nested_bundle"
  done

sign_path "$staged_app"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$staged_app"

notary_zip="$task_temp/Limitless-notarization.zip"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$staged_app" "$notary_zip"
/usr/bin/xcrun notarytool submit "$notary_zip" \
  --keychain-profile "$notary_profile" --wait
/usr/bin/xcrun stapler staple "$staged_app"
/usr/bin/xcrun stapler validate "$staged_app"

mkdir -p "$release_dir"
zip_path="$release_dir/Limitless-$version-macOS-$architecture.zip"
dmg_path="$release_dir/Limitless.dmg"
[ ! -e "$zip_path" ] || trash "$zip_path"
[ ! -e "$dmg_path" ] || trash "$dmg_path"

/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$staged_app" "$zip_path"
/usr/bin/hdiutil create -fs HFS+ -format UDZO -volname Limitless \
  -srcfolder "$staged_app" "$dmg_path"
/usr/bin/xcrun notarytool submit "$dmg_path" \
  --keychain-profile "$notary_profile" --wait
/usr/bin/xcrun stapler staple "$dmg_path"
/usr/bin/xcrun stapler validate "$dmg_path"

"$repo_root/scripts/verify_rebrand.sh" "$staged_app"
echo "Created $zip_path"
echo "Created $dmg_path"
