# Building Limitless Browser

Limitless is a native SwiftUI browser shell integrated into a pinned Chromium
checkout. The Chromium checkout is intentionally kept outside this Git
repository because a complete source tree and build output consume tens of
gigabytes.

## Tracked and generated files

- `ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless` contains the
  tracked Limitless source overlay and brand resources.
- `patches/chromium/limitless-integration.patch` contains the reviewed changes
  to Chromium build files and browser factories.
- `chromium.version` pins the only supported Chromium revision.
- `<chromium-src>/out/Default/Limitless.app` is the generated local app.

## Prerequisites

- macOS 14 or newer.
- Xcode at `/Applications/Xcode.app`.
- `depot_tools` on `PATH`.
- Enough free space for the Chromium source, object files, and app bundle.

## Create the Chromium checkout

Keep the `.gclient` file in a dedicated parent directory. The following creates
an exact checkout from the repository pin:

```sh
repo_root="$(git rev-parse --show-toplevel)"
chromium_root="$HOME/Developer/limitless-chromium"
revision="$(tr -d '[:space:]' < "$repo_root/chromium.version")"

mkdir -p "$chromium_root"
cd "$chromium_root"
gclient config --name src https://chromium.googlesource.com/chromium/src.git
gclient sync --no-history --nohooks --revision "src@$revision"
gclient runhooks
```

## Prepare and build

The preparation script validates the revision, applies the pinned integration
patch, copies the tracked source overlay, installs the app icon and asset
catalog, and can be run repeatedly.

```sh
repo_root="$(git rev-parse --show-toplevel)"
chromium_src="$HOME/Developer/limitless-chromium/src"

"$repo_root/scripts/rebrand_chromium_integration.sh" "$chromium_src"
cd "$chromium_src"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

buildtools/mac/gn gen out/Default --args='target_cpu="arm64"
is_debug=true
is_component_build=true
symbol_level=0
blink_symbol_level=0
v8_symbol_level=0
use_remoteexec=false
use_siso=true
mac_min_system_version="14.0"'

autoninja -C out/Default chrome
```

The SwiftUI shell uses macOS 14 APIs and Swift 5 language mode. The integration
patch enables Chromium's existing Apple Swift tool for macOS and supplies the
matching compiler flags.

## Run an isolated local build

Launch with a separate profile so development state never touches a normal
Limitless profile:

```sh
chromium_src="$HOME/Developer/limitless-chromium/src"
profile="$HOME/Library/Application Support/LimitlessTestProfile"

mkdir -p "$profile"
/usr/bin/open -n "$chromium_src/out/Default/Limitless.app" --args \
  --user-data-dir="$profile" \
  --no-first-run
```

Confirm that the expected executable stayed running:

```sh
pgrep -fl '/Limitless.app/Contents/MacOS/Limitless'
```

## Verify the app

Run source and asset contract checks from the repository:

```sh
scripts/verify_rebrand.sh
tests/rebrand_contract_test.sh
```

Pass the app path to verify its product name, bundle identifiers, minimum macOS
version, nested bundles, brand resources, and code signature:

```sh
scripts/verify_rebrand.sh \
  "$HOME/Developer/limitless-chromium/src/out/Default/Limitless.app"
```

The canonical product identity lives in `config/limitless-product.json`.
Limitless is a clean-break app and does not read, move, or delete data belonging
to an earlier product identity.

## Package a signed release

The release script signs nested Chromium code, notarizes and staples the app,
then emits a versioned ZIP and `Limitless.dmg`:

```sh
export LIMITLESS_BUILD_DIR="$HOME/Developer/limitless-chromium/src/out/Default"
export LIMITLESS_VERSION=1.0.0
export LIMITLESS_SIGNING_IDENTITY='Developer ID Application: Example (TEAMID)'
export LIMITLESS_NOTARY_PROFILE='limitless-notary'
scripts/package_release.sh
```

The keychain profile keeps notarization credentials outside the repository.
Override `LIMITLESS_RELEASE_DIR` or `LIMITLESS_ARCHITECTURE` when needed.

## Common development loop

After changing files in the tracked overlay, prepare again before rebuilding:

```sh
repo_root="$(git rev-parse --show-toplevel)"
chromium_src="$HOME/Developer/limitless-chromium/src"

"$repo_root/scripts/rebrand_chromium_integration.sh" "$chromium_src"
autoninja -C "$chromium_src/out/Default" chrome
scripts/verify_rebrand.sh "$chromium_src/out/Default/Limitless.app"
```
