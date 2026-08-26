# Phase 1 reproducible foundation

## Outcome and scope

Phase 1 establishes a deterministic macOS arm64 foundation for the pinned Chromium checkout. It verifies the source pin and host, validates and applies the recorded patch series, generates one dedicated GN output, builds the real app and browser tests, and launches the `.app` with an isolated profile.

This phase does not approve the product shell architecture at Gate 2. The foundation keeps Chromium's existing WebUI Browser resource map, upstream `<webui-browser-app>`, and frontend bootstrap active. A narrowly scoped WebUI data-source request filter serves the Limitless root HTML plus the namespaced `limitless/app.css` and `limitless/app.js` payloads. Unmatched requests continue through Chromium's existing WebUI Browser resource map. The foundation verifies that the real active tab attaches through SurfaceEmbed. Accessibility stitching, native-window parity, focus transfer, IME, and complete embedded-tab behavior remain explicit Gate 2 work.

Supported Phase 1 host:

```text
OS: macOS
CPU: arm64
Chromium: 2b4613f0a24d234b139f1246028239a2f5dde58c
Output: chromium/src/out/LimitlessDebug
App: chromium/src/out/LimitlessDebug/Limitless.app
```

Supported Phase 1 workflow entry points, build targets, and app smoke tests require macOS arm64. Intel macOS is not implied by this foundation.

## Repository boundary

Use sibling repositories in one workspace:

```text
limitless-workspace/
  limitless/
  chromium/
    .gclient
    src/
```

Limitless owns scripts, source resources, tests, branding inputs, documentation, and patch metadata. Chromium is a pinned dependency. Do not copy Limitless files into Chromium, edit generated files, or reuse a developer's ordinary Chrome or Chromium profile.

Phase 1 expects these authority files:

- `chromium.version`: the exact upstream revision.
- `patches/chromium/manifest.json`: base revision, ordered patch records, SHA-256 values, touched files and symbols, owners, tests, drift signals, and removal conditions.
- `patches/chromium/series`: patch filenames in application order.
- `branding/BRANDING`: the non-Google product identity used by the validation bundle.
- `build/chromium/BUILD.gn`: external GN targets reached through the system-absolute `limitless_root` argument.

Generated output under `chromium/src/out/` is disposable and must not be committed.

## Host prerequisites

Before checkout:

1. Install Xcode and select its command-line tools with `xcode-select`.
2. Check out `depot_tools` as the workspace sibling `limitless-workspace/depot_tools` and put it first on `PATH`.
3. Confirm `uname -s` reports `Darwin` and `uname -m` reports `arm64`.
4. Ensure enough disk space for a Chromium checkout and debug component build.
5. Use a case-sensitive or Chromium-supported filesystem configuration.

The verification script reports host, pin, and worktree state. It does not fetch, reset, clean, switch revisions, or discard local work.

## Fresh-checkout workflow

The following is the supported empty-workspace route. Substitute the actual Limitless repository URL.

```bash
mkdir limitless-workspace
cd limitless-workspace

git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git depot_tools
export PATH="$PWD/depot_tools:$PATH"

git clone <limitless-repository-url> limitless

mkdir chromium
cd chromium
gclient config --name=src https://chromium.googlesource.com/chromium/src.git
PIN="$(tr -d '[:space:]' < ../limitless/chromium.version)"
gclient sync --revision "src@$PIN" --no-history
gclient runhooks
cd ..
```

Do not replace `PIN` with a branch, tag, short SHA, or current Chromium head. Verify the checkout before applying anything:

```bash
./limitless/tools/chromium/verify --require-clean
```

Expected invariant:

```text
limitless/chromium.version
  == git -C chromium/src rev-parse HEAD
  == 2b4613f0a24d234b139f1246028239a2f5dde58c
```

### Pre-patch contract tests

Run the negative fixtures and strict patch dry-run while Chromium is still clean and unpatched:

```bash
./limitless/tests/foundation/run.sh pre-patch
```

This test stage proves:

- the host and exact pin are accepted;
- a pin mismatch is rejected;
- malformed manifest JSON is rejected;
- a changed patch checksum is rejected;
- a manifest and series mismatch is rejected;
- `apply-patches --check` succeeds without changing any tracked or untracked Chromium state.

The failure fixtures copy the Limitless repository into a temporary workspace and point its sibling `chromium/src` at the real checkout through a read-only symlink. The test mutates only the temporary Limitless copy. Temporary fixture directories are removed when the test exits.

### Apply the recorded series

```bash
./limitless/tools/chromium/apply-patches --check
./limitless/tools/chromium/apply-patches
./limitless/tools/chromium/verify --require-patched
```

`--check` is valid only on an exact-base, clean, unpatched checkout. On an already correctly patched checkout, use `verify --require-patched`. The apply command has no three-way fallback and must not accept fuzz, offsets, extra files, missing files, or checksum drift.

If a check fails, stop. Do not hand-apply the patch, edit its generated diff, or reset the checkout to make the failure disappear.

### Post-patch GN contract tests

```bash
./limitless/tests/foundation/run.sh post-patch
```

The post-patch test creates all GN outputs under a temporary directory and leaves Chromium source untouched. It generates the WebUI Browser test graph three ways:

| `limitless_root` | Expected result |
| --- | --- |
| Empty string | GN succeeds and includes no Limitless external targets. |
| Absolute path to this Limitless repository | GN succeeds and includes `build/chromium:resources` and `build/chromium:webui_browser_tests`. |
| Missing directory | GN fails with an actionable `limitless_root` or `BUILD.gn` diagnostic. |

The external root is a GN system-absolute label. Do not create a symlink or source overlay under Chromium to replace it.

The Phase 1 GN wiring contract is exact:

- Chromium's `//chrome/browser/ui/webui_browser:impl` conditionally depends on `$limitless_root/build/chromium:resources` when `limitless_root` is nonempty.
- `$limitless_root/build/chromium:resources` forwards to `$limitless_root/shell/resources:resources`, which owns WebUI preprocessing and generates the embedded HTML, CSS, and JavaScript payloads consumed by the request filter.
- Chromium's `//chrome/browser/ui/webui_browser:browser_tests` conditionally depends on `$limitless_root/build/chromium:webui_browser_tests` when `limitless_root` is nonempty.
- `$limitless_root/build/chromium:webui_browser_tests` compiles `../../tests/browser/limitless_webui_browser_browsertest.cc` with `HAS_OUT_OF_PROC_TEST_RUNNER`.
- Its direct Chromium dependencies are `//base/test:test_support`, `//chrome/browser/ui`, `//chrome/browser/ui/webui_browser`, `//chrome/browser/ui/webui_browser:impl`, `//chrome/common:chrome_features`, `//chrome/test:test_support`, `//components/surface_embed/common`, `//content/public/browser`, `//content/test:test_support`, `//ui/base`, and `//url`.

Phase 1 adds no product Mojo or binder API. The browser tests consume the existing Browser, WebUI Browser, and SurfaceEmbed test surfaces.

### Generate the deterministic output

Preview the command and complete arguments:

```bash
./limitless/tools/chromium/gen --dry-run
```

Generate the fixed output:

```bash
./limitless/tools/chromium/gen
```

The tool derives absolute repository paths and writes the complete generated configuration for `chromium/src/out/LimitlessDebug`. It does not reuse `out/Default`. Important arguments include:

```gn
target_os = "mac"
target_cpu = "arm64"
is_debug = true
is_component_build = true
limitless_root = "/absolute/path/to/limitless"
branding_file_path = "/absolute/path/to/limitless/branding/BRANDING"
```

Never hand-edit the generated `args.gn` as part of the normal workflow.

### Build

Inspect the build invocation if needed:

```bash
./limitless/tools/chromium/build --dry-run chrome browser_tests
```

Build the app and the Phase 1 browser tests:

```bash
./limitless/tools/chromium/build chrome browser_tests
```

WebUI preprocessing, TypeScript compilation, CSS linting, embedded-payload generation, and the external browser-test source are dependencies of the corresponding GN targets. The request filter serves those generated payloads without registering a second resource map. Build intermediates remain in the Chromium output tree. The Limitless target does not replace Chromium's existing WebUI Browser resource map or upstream frontend.

## Automated tests

### Foundation contracts

Run the stage appropriate to checkout state:

```bash
./limitless/tests/foundation/run.sh pre-patch
./limitless/tests/foundation/run.sh post-patch
```

Do not run `pre-patch` after applying the series. Its patch dry-run intentionally requires the exact unpatched base.

### Browser process tests

The Limitless browser test filter is:

```text
LimitlessWebUIBrowserTest.*
```

Run it through the project test wrapper:

```bash
./limitless/tools/chromium/test \
  --browser-filter 'LimitlessWebUIBrowserTest.*' \
  --skip-interactive
```

The test wrapper intentionally supplies no process-wide feature switch. The
`LimitlessWebUIBrowserTest` and `WebUIBrowserSurfaceEmbedPixelTest` fixtures
enable Webium plus SurfaceEmbed. Upstream functional and interactive fixtures
retain Webium plus `AttachUnownedInnerWebContents`, so a SurfaceEmbed test cannot
silently replace the attachment mode expected by upstream regression tests.
Ordinary launches have no test fixture, so `tools/chromium/run` and the E2E
launcher intentionally pass `--enable-features=Webium,SurfaceEmbed`.

The tests prove:

1. `chrome://webui-browser` loads the filtered root and namespaced Limitless assets while upstream `webui_browser.js` and other unmatched resources continue through Chromium's existing map, then publishes:
   - `#limitless-shell`;
   - `#limitless-shell[data-ready="true"]` after JavaScript initialization;
   - `#limitless-build-marker`;
   - `data-build-id="limitless-phase1-macos-arm64-2b4613f0"`;
   - `data-chromium-revision="2b4613f0a24d234b139f1246028239a2f5dde58c"`.
2. A normal window is a `WebUIBrowserWindow`, not a `BrowserView`, and its active tab attaches through SurfaceEmbed after shell readiness.
3. A `TYPE_POPUP` window remains on the standard `BrowserView` path.

### Real app launch and clean shutdown

After building `chrome`, launch the actual binary inside the generated app bundle:

```bash
./limitless/tests/e2e/app_launch_smoke.sh
```

The E2E script:

- verifies macOS arm64 and the `.app` structure;
- resolves `CFBundleExecutable` from `Info.plist`;
- creates a unique temporary user-data directory;
- launches the bundle executable with Webium and SurfaceEmbed enabled;
- waits for the browser CDP endpoint instead of using a fixed delay;
- attaches through browser-level CDP to the live `chrome://webui-browser/` target;
- requires `#limitless-shell[data-ready="true"]`, the exact Phase 1 build ID, and the exact Chromium revision from the live DOM;
- independently removes an optional leading `@` from `Browser.getVersion.revision`, then requires either the pinned Chromium revision or Chromium's 40-zero dummy `LASTCHANGE` value used by non-official builds, and records which case occurred;
- sends `Browser.close` only after the live shell contract succeeds, while still requesting a clean close after validation failures;
- waits for exit status zero;
- verifies profile singleton locks were removed;
- fails on fatal, failed-CHECK, or sanitizer output;
- records stdout, stderr, Chrome logging, the CDP validation JSON, and a result summary.

`cdp_validation.json` records the raw `browser_revision` and a
`browser_revision_status` of `exact` or `unversioned-local`. The current
non-official Phase 1 build is expected to use Chromium's 40-zero dummy
`LASTCHANGE` value. Component-build mode is unrelated. This dummy value is
accepted only for the browser protocol field. The embedded DOM marker must
always equal the exact revision in `chromium.version`.

To place logs in a CI artifact directory:

```bash
./limitless/tests/e2e/app_launch_smoke.sh \
  --artifacts /absolute/path/to/artifacts/phase-1-launch
```

The temporary profile is deleted after success. On failure it is retained with the logs for diagnosis. The script never points Chromium at a normal user profile.

## CI order

A fresh macOS arm64 CI job uses this order:

1. Check out Limitless and sync Chromium to `chromium.version`.
2. Run hooks.
3. Run `verify --require-clean`.
4. Run `tests/foundation/run.sh pre-patch`.
5. Run `apply-patches --check`, then apply the series.
6. Run `verify --require-patched`.
7. Run `tests/foundation/run.sh post-patch`.
8. Run `gen`.
9. Build `chrome` and `browser_tests`.
10. Run `LimitlessWebUIBrowserTest.*`.
11. Run the real-app E2E launch with an explicit artifact directory.
12. Archive logs, test XML, app-launch artifacts, generated GN args, exact repository SHAs, and patch verification output.

No CI step commits patched Chromium, generated resources, `out/`, profiles, logs, or crash dumps.

## Failure interpretation

| Failure | Meaning and response |
| --- | --- |
| Host rejection | This foundation does not support the current platform or CPU. Do not bypass it with a flag. |
| Pin mismatch | The Chromium checkout and Limitless inputs are not one reproducible unit. Sync a separate clean checkout to the recorded pin. |
| Manifest or checksum mismatch | Patch metadata or content changed without review. Regenerate and review the coherent patch record. |
| Patch dry-run changes source | This is a tooling defect. The check mode must be read-only. |
| Empty-root GN failure | The upstream-default build path regressed. Limitless integration must remain conditional. |
| Valid-root GN failure | The external target, resource graph, or generated path is broken. Do not copy files into Chromium as a workaround. |
| Invalid-root GN success | The build hook is accepting ambiguous input and must be fixed. |
| Semantic marker failure | The request filter is absent, matches the wrong paths, fails to return the generated Limitless payloads, or frontend initialization failed. Chromium's existing WebUI Browser map should remain active. |
| Normal-window class failure | Feature selection or startup ordering changed. Do not patch the browser-window factory. |
| Popup fallback failure | Limitless escaped the approved `TYPE_NORMAL` boundary. Restore standard secondary-window behavior. |
| E2E timeout or nonzero exit | Inspect retained profile and logs before rerunning. Do not substitute a forced kill as a passing shutdown. |

## Gate 2 accessibility and native-window handoff

Phase 1 hands the following unresolved checks to the Gate 2 thin slice. The foundation proves connector attachment, but these checks require a user-visible browser journey through the embedded tab.

### Accessibility

- Navigate and switch real active tab `WebContents` through the existing SurfaceEmbed attachment.
- Inspect the combined shell and page tree with Accessibility Inspector and VoiceOver.
- Verify shell landmarks, names, roles, values, actions, and deterministic reading order.
- Traverse from shell controls into page content and back in both directions.
- Verify focus restoration after tab changes, menus, permission prompts, DevTools, and window activation.
- Exercise DOM address input and page input with dead keys, composed text, and at least one CJK IME.
- Test increased contrast, keyboard-only operation, reduced motion, and text scaling without clipping.

The Phase 1 semantic DOM markers are automation anchors, not accessibility acceptance evidence.

### Native macOS window behavior

- Inspect traffic-light position, titlebar exclusion, drag regions, and interactive controls near the titlebar.
- Move and resize from every edge and corner; verify minimum size and Retina scaling.
- Exercise minimize, zoom or maximize, restore, native close, last-window close, and explicit application quit.
- Enter and exit browser fullscreen and tab fullscreen through native transitions.
- Move across displays with different scale factors and menu-bar placement.
- Verify Command-key shortcuts, the global application menu, menu enablement, and focus after child dialogs.
- Trigger file chooser, HTTP authentication, certificate, permission, download, and pending-download-close surfaces and inspect modality and anchoring.
- Kill the shell renderer and verify the native window and browser-owned tabs recover without state loss.

Source inspection showed that `WebUIBrowserWindow` uses its own macOS native widget rather than inheriting the standard `BrowserNativeWidgetMac` and `BrowserFrameViewMac` path wholesale. These checks are therefore architecture evidence, not polish deferred until release.

### Gate 2 stop conditions

Stop and reconsider the shell boundary if any of these require broad upstream patches, duplicated browser state, raw page scripting, inaccessible split trees, emulated native controls, forced coordinate automation, or GuestContents in a production build.

## Reproducing a foundation failure

Record all of the following before changing code:

- Limitless commit and `git status --short`;
- Chromium full SHA and `git status --short`;
- macOS version and arm64 host result;
- exact tool command and exit status;
- generated GN arguments;
- patch verification output;
- browser-test XML and filter;
- E2E stdout, stderr, Chrome log, temporary profile path, and result file;
- expected behavior and actual behavior from the user-visible journey.

For a browser bug, reproduce through the real `.app` first, then add the highest-value automated regression test before implementing a fix.
