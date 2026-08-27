# Build and repository integration

Limitless v1 builds, tests, packages, and releases on macOS only. The repository and target boundary should remain portable, but no Linux or Windows build machinery is a v1 deliverable.

## Baseline observed

- `chromium/src` is at `2b4613f0a24d234b139f1246028239a2f5dde58c`.
- `limitless/chromium.version` contains the same full SHA.
- The Chromium and Limitless worktrees were clean before Phase 0 documents were created.
- `chromium/src/out/Default/args.gn` contains only the generated-file comment, so the existing local output uses Chromium's default GN arguments.
- `chromium/.gclient` has one `src` solution and no custom dependencies or variables.

The effective `out/Default` configuration reported by the pinned `buildtools/mac/gn args out/Default --list --short` is a macOS arm64 debug component build:

```text
host_os = "mac"
host_cpu = "arm64"
target_os = ""       # host default
target_cpu = ""      # host default
is_debug = true
is_component_build = true
dcheck_always_on = true
is_official_build = false
symbol_level = 2
chrome_pgo_phase = 0
use_remoteexec = false
use_siso = true
```

`out/Default/Chromium.app/Contents/MacOS/Chromium` is present and executable. This output is evidence of the local baseline only. Phase 1 must generate and use a separate deterministic Limitless output directory.

No Phase 0 work modifies Chromium, generates build output, or changes the pin.

## Integration decision

Limitless-owned C++, TypeScript, CSS, resources, tests, scripts, branding metadata, and patch metadata remain in the `limitless/` repository. Chromium consumes them through a GN system-absolute external label supplied by a build argument.

The pinned `buildtools/mac/gn help labels` explicitly permits system-absolute labels and recommends supplying their paths through a build argument. The same GN help says target `sources` may be absolute paths. Therefore the build does not need a symlink, copy, submodule, or generated source overlay under `chromium/src`.

The initial validation patch adds one `limitless_root` argument to `chrome/browser/ui/webui_browser/BUILD.gn`. When empty, the Chromium graph and resources are unchanged. When set, that target depends on an external target such as:

```gn
"$limitless_root/build/chromium:resources"
```

For v1 macOS builds, `limitless_root` is a normal absolute POSIX path. A future Windows port would need GN's system-absolute label form, such as `/C:/work/limitless`, with forward slashes; that normalization is deferred and will belong in scripts rather than manual instructions.

### macOS external-label probe

Phase 0 exercised this GN mechanism on the pinned macOS checkout with a disposable directory outside `chromium/src`. Its `BUILD.gn` contained:

```gn
group("probe") {
  deps = [ "//base" ]
}
```

From `chromium/src`, the command shape was:

```text
buildtools/mac/gn gen /tmp/<probe>/out \
  --root=/absolute/path/to/chromium/src \
  --root-target=/tmp/<probe>:probe \
  --args='is_debug=true'
```

GN completed successfully, making 2,813 targets from 620 files. The generated `toolchain.ninja` contained a `phony/ABS_PATH/tmp/<probe>/probe` edge that depended on `libbase.dylib.TOC`, proving that the external target participated in Chromium's graph and could depend back on `//base`. The disposable tree was moved to Trash after verification, so no probe inputs or outputs remain. This proves the label mechanism on this pinned macOS checkout. The proposed external `build_webui` target remains a Gate 2 check. Windows path handling is deferred beyond v1.

## Proposed Limitless layout

```text
limitless/
  branding/
    BRANDING
    icons/
  build/chromium/
    BUILD.gn
  docs/architecture/
  patches/chromium/
    manifest.json
    series
    0001-webui-browser-limitless-resources.patch
    0002-browser-view-swiftui-boundary-probe.patch
    0003-webui-browser-tab-handle-scope.patch
    0004-webui-browser-guest-handler-lifetime.patch
    0005-browser-view-swiftui-tab-rail.patch
    0006-browser-view-leader-shortcuts.patch
  shell/
    agents/
      herdr_agent_provider.{h,cc}
      agent_presence_registry.{h,cc}
    native/swiftui_rail/
      BUILD.gn
      limitless_swiftui_rail.swift
      limitless_swiftui_rail_host.{h,mm}
    shortcuts/
      BUILD.gn
      limitless_leader_shortcut_controller.{h,mm}
      limitless_command_dispatcher_delegate_mac.{h,mm}
    browser.mojom adapters, C++ only when required
    resources/
      app.html.ts
      app.ts
      app.css
      icons/
  tests/
    browser/
    e2e/
  tools/chromium/
    verify
    apply-patches
    gen
    build
    run
    test
```

The six patch filenames shown are the current experimental series. Other tree entries describe the intended product layout. Only ordered, hashed patch files may alter upstream-owned paths.

The fifth patch constructs the native SwiftUI tab rail and suppresses
Chromium's horizontal tab strip for that window. This is behavior evidence,
not visual acceptance. Chromium toolbar and other top surfaces remain, and the
pixel gate stays red until a real exact-window PNG can be inspected. External
AX and native input acceptance also remain red.

The display-only Agents slice itself adds no upstream file or patch. Its
provider, transient registry, SwiftUI projection, bridge, and tests are
product-owned. The existing `shell/native/swiftui_rail:rail` target depends on
`shell/agents:agent_presence`; the external browser-test target also links
`shell/agents:agent_presence_tests`. The slice reuses the BrowserView host seam
already accounted for by patches `0002` and `0005`. The later leader-shortcut
slice is patch `0006`: it overlaps `browser_view.cc` and adds only
`browser_native_widget_mac.mm` to the combined upstream inventory.

## GN target design

### Product-owned leader shortcut target

`shell/shortcuts:leader_shortcuts` owns the typed per-window state machine,
command map, command-line policy, macOS event deduplication, timeout, exact
window monitoring, and product command-dispatcher delegate. The existing
`shell/native/swiftui_rail:rail` target publicly depends on it and supplies the
authorized Chromium tab actions plus the SwiftUI armed-state projection.
`build/chromium:swiftui_browser_tests` links the same production targets.

Patch `0006` contains only two calls into that product boundary:
`BrowserView::PreHandleKeyboardEvent()` for renderer-originated events and
`BrowserNativeWidgetMac::OnWindowInitialized()` for the native macOS command
dispatcher. Disabled or ineligible windows receive Chromium's stock delegate.
The focused browser journeys exercise renderer, Views-control, and in-process
Application-target AppKit routing, but external macOS input, IME, AX, and pixel
acceptance remain real-app gates.

### Product-owned Herdr presence targets

`shell/agents:agent_presence` owns the Herdr CLI adapter and browser-process
presence registry. SwiftUI receives sanitized arrays through the existing
Objective-C++ host and never launches a process or parses JSON. The registry is
transient, starts when the first regular-window rail observes it, stops when the
last observer leaves, coalesces unchanged snapshots, and grants no browser
capability. Off-the-record rails do not observe it.

Production executable resolution accepts an explicit absolute
`--limitless-herdr-executable` override, then only
`/opt/homebrew/bin/herdr` and `/usr/local/bin/herdr`. It does not search the
inherited `PATH` or accept an environment-provided binary path. A candidate must
be a regular executable. Each poll uses the exact argv
`[herdr, api, snapshot]`, without a shell or interpolation, on one sequenced
off-UI worker. The provider enforces a short timeout, prevents overlapping
polls, rejects late replies after teardown, bounds and validates the response,
and kills and reaps child process groups on failure.

The adapter projects every agent in the current Herdr session across that
session's workspaces. It does not aggregate named Herdr sessions and cannot
prove that a visible agent is using Limitless. Same-user presence is not
authentication or browser authorization. Missing, denied, unavailable, timed
out, malformed, and incompatible results clear rows and publish an explicit
state; no unlabeled last-good snapshot is retained.

`shell/agents:agent_presence_tests` contains the nine focused provider and
registry tests, including residual process-group descendant cleanup after a
normal or nonzero leader exit. `build/chromium:swiftui_browser_tests` links
those tests and the native rail suite, including the three focused Herdr
browser journeys and seven Leader journeys. With retries disabled, the
provider and registry suite passes 9 of 9, the focused Herdr trio passes 3 of
3, and the current full native suite passes 17 of 17.

The starvation journey uses 40 real Chromium tabs and 12 agents. Transparent,
non-hit-testable, non-accessible `NSViewRepresentable` probes occupy the actual
`GeometryReader`-proposed bounds for both viewports and both scroll contents.
The Objective-C++ bridge forces native layout and reads their converted
post-layout frames, so the proof validates positive area, rail containment,
disjoint viewports, minimum and maximum viewport bounds, and overflowing
content without relying on inferred row heights. This target expansion changes
only the Limitless build graph. Patch hashes, upstream files, and watched
symbols remain unchanged.

### Product resource target

`limitless/build/chromium/BUILD.gn` should import Chromium's `//ui/webui/resources/tools/build_webui.gni` and invoke `build_webui("build")`. At the pinned revision the template ignores its invocation name and emits fixed child targets including `:resources`, `:build_ts`, `:lint_ts`, and `:lint_css`. The integration dependency must therefore name `$limitless_root/build/chromium:resources`, not the invocation label. This product build should:

- compile the Limitless HTML, TypeScript, and CSS;
- use a unique GRD prefix such as `limitless_shell`;
- emit a trusted WebUI resource map and resource header;
- depend on the existing WebUI Browser Mojo TypeScript generator and only the browser API generators actually imported by the frontend;
- run the WebUI TypeScript, CSS, and resource lint targets;
- expose no C++ source dependency back to `//chrome/browser/ui/webui_browser:impl`.

The dependency direction is intentionally one-way:

```text
//chrome/browser/ui/webui_browser:impl
  -> /absolute/path/to/limitless/build/chromium:resources
       -> WebUI and Mojo generator leaf targets
```

The product resource target must not depend on `:impl`, which would form a cycle. Depending on `:browser_mojo_bindings_ts__generator` is safe because it is a separate generation target.

### Pinned WebUI resource selection

`chrome/browser/ui/webui_browser/webui_browser_ui.cc`, `WebUIBrowserUI::WebUIBrowserUI()`, currently installs `kWebuiBrowserResources` and `IDR_WEBUI_BROWSER_WEBUI_BROWSER_HTML` into the `chrome://webui-browser` data source. The validation patch should select the Limitless-generated map and HTML resource when `limitless_root` is set, while retaining:

- the `WebUIBrowserUI` controller type;
- the `chrome://webui-browser` URL and binder restriction;
- existing tab-strip, toolbar-shared, favicon, localized-string, and Mojo plumbing until Limitless deliberately replaces each dependency;
- the same trusted WebUI content-security policy plus the narrow `object-src 'self'` override required by SurfaceEmbed.

Keeping the controller and URL means `chrome/browser/chrome_content_browser_client_receiver_bindings.cc` continues to expose `SurfaceEmbedHost` only to `WebUIBrowserUI`. A separate custom scheme or broad binder registration is unnecessary.

### Platform-specific product sources

When C++ becomes necessary, use separate product targets instead of `#if` branches throughout shared code:

```gn
source_set("shell_shared") {
  sources = [ ... ]
}

source_set("shell_platform") {
  assert(is_mac, "Limitless v1 supports macOS only")
  sources = [ "..._mac.mm" ]
}
```

Shared behavior depends on an explicit product platform interface. The macOS target may depend on Chromium's Cocoa and native APIs. The shared shell may not branch on platform for ordinary layout or browser state. A future platform adds a new implementation behind this interface after a separate support decision.

## Resources, strings, icons, and branding

### Shell resources

Use `build_webui`, generated GRIT maps, and generated Mojo bindings exactly as the pinned targets do:

- `chrome/browser/resources/webui_browser/BUILD.gn` is the reference `build_webui` target;
- `chrome/browser/ui/webui_browser/BUILD.gn` is the reference Mojo and C++ dependency graph;
- `chrome/browser/ui/webui_browser/webui_browser_ui.cc` is the trusted data-source registration point.

Never edit generated resource headers, `.pak` files, TypeScript bindings, or path maps.

### Product name

`build/config/chrome_build.gni` already exposes `branding_file_path` as a GN argument. `build/util/branding.gni` reads it and produces product full name, short name, installer names, macOS bundle ID, creator code, and team ID. Phase 1 can point this argument at `limitless/branding/BRANDING` without setting `is_chrome_branded=true`.

Do not use `is_chrome_branded` for Limitless. At this revision it selects Google Chrome and internal-source behavior, including more than visual branding.

### Branded strings and icons

The built-in mechanism is not a generic third-brand system:

- `branding_path_product` selects relative files such as `chrome/app/chromium_strings.grd` and `components/components_chromium_strings.grd`;
- `branding_path_component` selects relative icon trees under `chrome/app/theme`;
- `chrome/BUILD.gn` and Linux installer build files interpolate that component in platform bundle paths.

For the Gate 2 validation binary, retain Chromium's strings and icons while the external `BRANDING` file gives the binary a clear Limitless product identifier. Before macOS distribution, add a separate, reviewed branding patch that introduces a generic external asset-root argument or explicit Limitless resource targets. Do not create untracked overlays or symlinks under `chromium/src`.

## Deterministic developer workflow

The scripts below are deliverables for Phase 1. Each refuses to continue on a mismatch instead of fixing it silently.

### 1. Verify

`tools/chromium/verify` must:

1. resolve both repository roots relative to the script, not the caller's current directory;
2. compare `git -C chromium/src rev-parse HEAD` with the exact contents of `chromium.version`;
3. require the expected full SHA;
4. report both worktree statuses;
5. refuse patch application if Chromium contains changes outside the recorded patch series;
6. verify patch and input-file hashes from the manifest.

It must never reset, clean, fetch, switch, or discard work.

### 2. Apply patches

`tools/chromium/apply-patches` reads `patches/chromium/series` in order. For every patch it runs a strict check before applying. It does not use a three-way fallback or silently accept offsets. A patch manifest records:

- pinned Chromium base SHA;
- ordered patch filename and SHA-256;
- upstream files and symbols touched;
- owning Limitless target;
- required tests;
- expected removal condition.

The script must also support a read-only `--check` mode for CI and rebase rehearsal.

### 3. Generate

`tools/chromium/gen` writes a complete, generated `args.gn` from a checked-in template and runs GN formatting before `gn gen`. Required validation arguments are conceptually:

```gn
limitless_root = "/absolute/path/to/limitless"
branding_file_path = "/absolute/path/to/limitless/branding/BRANDING"
is_debug = true
```

The script chooses a dedicated output such as `out/LimitlessDebug`. It must not reuse `out/Default`, whose manual state is unknown. The absolute paths are derived, never checked in.

### 4. Build

`tools/chromium/build` runs:

```text
autoninja -C out/LimitlessDebug chrome browser_tests interactive_ui_tests
```

The exact test targets may be narrowed for iteration, but CI builds the real `chrome` target and the browser tests that contain Limitless and upstream WebUI Browser coverage.

### 5. Launch

For the Gate 2 experiment only, `tools/chromium/run` launches the built browser with:

```text
--enable-features=Webium,SurfaceEmbed
--user-data-dir=<limitless-owned explicit test profile>
```

The v1 launcher selects the executable inside the generated macOS product `.app` bundle. It fails clearly on a non-macOS host rather than implying unsupported platform behavior.

The profile directory must be scoped to the exact output and must never point at a developer's normal Chromium or Chrome profile. E2E uses a fresh temporary profile unless a test explicitly covers persistence.

The production binary must eventually enable the selected substrate through a product build default or an early product feature configuration hook. Shipping a desktop shortcut that depends on optional validation flags is not acceptable. This is deliberately deferred until Gate 2 proves SurfaceEmbed viable.

## CI reproduction

The macOS CI job performs these steps from an empty workspace:

1. checkout `limitless/` at the requested commit;
2. read the full Chromium SHA from `chromium.version`;
3. create Chromium using `gclient` and sync the `src` solution to that exact SHA;
4. run Chromium hooks using the same depot_tools revision policy as local development;
5. verify clean state and toolchain prerequisites;
6. verify and apply the hashed patch series;
7. generate arguments with the CI absolute `limitless_root`;
8. build the real browser and test targets;
9. run unit, browser, interactive UI, E2E, and resource lint tests;
10. archive test logs, screenshots, crash dumps, GN args, patch manifest, and the exact two repository SHAs.

CI never tracks `out/`, generated GRIT files, or a patched Chromium worktree as source artifacts.

## Chromium update workflow

An update is prepared on a branch, not in the working pinned checkout:

1. record old and proposed full SHAs;
2. sync a clean checkout to the proposed SHA;
3. run strict patch `--check` with no three-way fallback;
4. inspect upstream changes to every watched symbol listed in `patch-surface.md`;
5. update each failing patch as a separate reviewable change;
6. regenerate GN, build the macOS app, and run the full macOS suite on the supported host and CPU matrix;
7. compare screenshots and accessibility trees for critical journeys;
8. update architecture maps if ownership changed;
9. record patch additions, removals, conflicts, and line-count change;
10. change `chromium.version` only after the update passes.

The patch count, touched upstream files, and churn per update are CI metrics. Growth is an architecture warning.

## Tests required for build integration

- GN generation with `limitless_root=""` proves unmodified Chromium behavior remains available.
- GN generation with a valid Limitless root resolves the external label and generated resource map.
- GN generation with a missing or malformed root fails with an actionable message.
- A resource smoke test loads `chrome://webui-browser` and asserts a Limitless-owned semantic element and build identifier.
- A process test asserts the normal window is `WebUIBrowserWindow` when the validation features are enabled, while popup, app, picture-in-picture, and DevTools window types retain their intended implementation.
- Resource lint, TypeScript compile, generated-resource checks, and duplicate-resource-ID checks run in CI.
- The launch smoke test starts the actual product bundle or executable with a fresh profile, navigates, and exits cleanly.

## Known unknowns

- Apple silicon is the proven local baseline. Decide whether Intel is part of v1 before packaging; if so, add real x86_64 generation, build, run, and packaging coverage.
- Windows external-label and generated-directory behavior remains unverified and deferred beyond v1.
- The final generic branding asset-root design is not proven and should not be mixed into the Gate 2 substrate experiment.
- Production default enablement for Webium and SurfaceEmbed should be designed only after Gate 2. It must preserve command-line override semantics and run before feature-list creation in every browser process startup path.
- Packaging, signing, notarization, updates, crash reporting, and distribution channel identity are required before the macOS v1 release, but remain outside the Gate 2 substrate experiment. Their product identifiers must not reuse Google Chrome identities or endpoints.
