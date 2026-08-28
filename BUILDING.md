# Building Limitless Browser

This repository now contains the Limitless browser work on top of ungoogled-chromium
instead of the old CEF app.

## Layout

- `ungoogled-chromium-macos/build/src` is the Chromium source tree used for
  day-to-day builds.
- `out/Default/Limitless.app` inside that source tree is the local build output.
- `$HOME/Downloads/Limitless.app` is the packaged app used for manual testing.
- `depot_tools` and `binshims` are checked out beside the source tree and should
  be placed on `PATH` for builds.

## Prerequisites

- macOS with Xcode installed at `/Applications/Xcode.app`.
- The Chromium checkout and generated `out/Default` build directory already
  present under `ungoogled-chromium-macos/build/src`.
- The checkout must resolve to the exact revision recorded in
  `chromium.version`.
- Enough free disk space for Chromium link output and dSYMs.

## Build

On a populated checkout that still contains the previous local integration,
run the idempotent rebrand transform once before generating or building:

```sh
scripts/rebrand_chromium_integration.sh
```

The transform updates ignored Chromium integration files around the tracked
Limitless overlay. Review `chrome/browser/ui/BUILD.gn` and the generated app
metadata after it runs.

Run from the repository root:

```sh
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root/ungoogled-chromium-macos/build/src"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$repo_root/binshims:$repo_root/depot_tools:$PATH"
ninja -j 16 -l 24 -C out/Default chrome
```

The normal successful build still prints warnings from Swift deployment-target
mismatch and `CoreAudioTypes` auto-linking. Treat ninja exit code `0` as the
source of truth.

## Package To Downloads

After a successful build:

```sh
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root/ungoogled-chromium-macos/build/src"
if [ -e "$HOME/Downloads/Limitless.app" ]; then
  trash "$HOME/Downloads/Limitless.app"
fi
/usr/bin/ditto "out/Default/Limitless.app" "$HOME/Downloads/Limitless.app"
```

Use `trash`, not `rm -rf`, when replacing old app bundles.

Verify the packaged framework timestamp:

```sh
find "$HOME/Downloads/Limitless.app/Contents/Frameworks" \
  -type f -name 'Limitless Framework' \
  -exec stat -f '%Sm %N' -t '%Y-%m-%d %H:%M:%S' {} \;
```

## Run

Open the packaged app:

```sh
/usr/bin/open -n "$HOME/Downloads/Limitless.app"
```

For isolated testing without reusing the main Chromium profile:

```sh
profile="$HOME/Library/Application Support/LimitlessTestProfile"
mkdir -p "$profile"
/usr/bin/open -n "$HOME/Downloads/Limitless.app" --args \
  --user-data-dir="$profile" \
  --no-first-run
```

Check that it stayed running:

```sh
pgrep -fl "$HOME/Downloads/Limitless.app/Contents/MacOS/Limitless"
```

## Logging

For keyboard shortcut diagnostics, current instrumented builds emit `LIMITLESS-KEY`
messages. To capture launch logs directly:

```sh
"$HOME/Downloads/Limitless.app/Contents/MacOS/Limitless" \
  --enable-logging=stderr \
  --v=0
```

You can also confirm an instrumented packaged binary contains the diagnostics:

```sh
find "$HOME/Downloads/Limitless.app/Contents/Frameworks" \
  -type f -name 'Limitless Framework' \
  -exec strings -a {} \; | grep -m 5 'LIMITLESS-KEY'
```

## Avoiding Stale Binaries

If an already-running browser process is reused, it may still have the old
framework loaded. For shortcut testing, prefer a fresh `open -n` launch with a
separate `--user-data-dir`, or quit the existing app before testing the newly
packaged bundle.

## Verify Branding

Run the source and asset contract checks after changing product code or
resources:

```sh
scripts/verify_rebrand.sh
```

Pass an application path to verify its bundle identity and code signature too:

```sh
scripts/verify_rebrand.sh "$HOME/Downloads/Limitless.app"
```

The canonical product identity is declared in
`config/limitless-product.json`. Limitless is intentionally a clean-break app
and does not read, move, or delete data belonging to an earlier product name.

## Signed Release

The release script signs nested Chromium code, notarizes and staples the app,
then emits a versioned ZIP and `Limitless.dmg`:

```sh
export LIMITLESS_VERSION=1.0.0
export LIMITLESS_SIGNING_IDENTITY='Developer ID Application: Example (TEAMID)'
export LIMITLESS_NOTARY_PROFILE='limitless-notary'
scripts/package_release.sh
```

The credentials referenced by the keychain profile stay outside the
repository. Override `LIMITLESS_BUILD_DIR`, `LIMITLESS_RELEASE_DIR`, or
`LIMITLESS_ARCHITECTURE` when packaging a non-default build.

## Common Build Loop

```sh
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root/ungoogled-chromium-macos/build/src"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$repo_root/binshims:$repo_root/depot_tools:$PATH"
ninja -j 16 -l 24 -C out/Default chrome

if [ -e "$HOME/Downloads/Limitless.app" ]; then
  trash "$HOME/Downloads/Limitless.app"
fi
/usr/bin/ditto "out/Default/Limitless.app" "$HOME/Downloads/Limitless.app"
/usr/bin/open -n "$HOME/Downloads/Limitless.app"
```
