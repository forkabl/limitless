# Gate 2B SwiftUI boundary evidence

Status: **Executable workspace, Agents, and leader-shortcut rail candidate,
production selection still open**

This record compares two ways to compile and link a bounded SwiftUI view into
the existing Chromium `BrowserView` on the macOS arm64 v1 target. It also
records what the current runtime proves and what still needs an interactive
macOS session. The placeholder has been replaced by the first authoritative
tab rail, suppresses Chromium's horizontal tab strip, and now includes a
display-only Agents section backed by the current Herdr session plus a bounded
leader-shortcut journey. This record does not approve its pixels, retained
Chromium toolbar and top surfaces, external accessibility, hardware input, or
a production shell.

`docs/features.md` remains the product source of truth. The bounded candidate
does not reinterpret the required compact vertical shell.

## Shared architecture constraints

Both experiments preserve the same ownership rules:

- Chromium owns `Browser`, tabs, `WebContents`, navigation, profiles, security
  state, prompts, sessions, extensions, and native window lifecycle.
- Chromium's existing `BrowserView`, `views::Widget`, and `NSWindow` remain in
  place. The probe does not add a third `BrowserWindow` implementation.
- A `views::NativeViewHost` attaches one product-owned `NSHostingView` only for
  normal windows and only behind `--limitless-swiftui-boundary-probe`.
- The rail observes Chromium's per-window `TabListInterface` and projects exact
  tab handles, order, titles, and active state into SwiftUI. It does not create
  parallel tab or workspace authority.
- A product-owned provider invokes and parses Herdr away from the UI thread. A
  transient browser-process registry publishes only display data to SwiftUI.
  Herdr remains authoritative for the current session's presence snapshot.
- Herdr presence grants no browser capability. Running under the same local
  user does not authenticate an agent or authorize access to tabs, profiles,
  settings, commands, or page data.
- One product-owned controller owns leader state and a typed command allowlist
  for one exact normal browser window. Renderer, Views, and in-process AppKit
  adapters feed that controller; neither SwiftUI nor a renderer acquires
  command authority.
- The SwiftUI view never receives or reparents a tab's native `WebContents`
  view and owns no duplicate browser state.
- Popup and other secondary window types retain their existing Chromium UI.

## Build-route comparison

| Evidence | Limitless-owned action | Generic Chromium macOS Swift enablement |
| --- | --- | --- |
| Chromium build-system files | 0 | 3 central files |
| Build-system diff | Product files only | 34 additions, 6 deletions |
| Full Chrome link | `chrome` and `browser_tests` passed | Not exercised |
| Component and non-component link | Current component Chrome passed | Small native probes passed both modes |
| Runtime | Real Limitless window created with the host linked | Native probes constructed an `NSHostingView` |
| Generated interface | Pre-Agents measurement: 13,370-byte Objective-C header, no user or temporary paths | Clean header with equivalent checks |
| Swift-only rebuild | 4 steps: Swift action, one small archive, `libchrome_dll`, `browser_tests` | Swift action and final probe link |
| Immediate no-op | 0 steps, 5.63 seconds | 0 steps, 0.02 to 0.12 seconds in the small graph |
| Release policy | Explicit `-O -whole-module-optimization` | Not integrated with Chromium optimization configs |
| Remote execution | Product action is local and bounded | Not tested |
| Undeclared outputs | None observed in `swiftui_action` | Xcode emitted `dependency-scan.dia` |
| Rebase scope | Product target plus the selected BrowserView seam | Chromium Apple toolchain behavior on every rebase |

The generic experiment changed:

- `build/toolchain/apple/toolchain.gni`;
- `build/config/apple/swift_source_set.gni`;
- `build/config/mac/BUILD.gn`.

It proved that Chromium's existing Swift machinery can be extended to macOS,
but the pinned source explicitly disables the Swift tool there. The experiment
did not link Chrome, did not prove release optimization or remote execution,
and produced an undeclared Xcode diagnostic file. Those are broad toolchain
responsibilities for one bounded product module.

The selected experiment build route is therefore the Limitless-owned action.
It emits one Swift object, generated Objective-C header, module, documentation,
source-info, and ABI files. A normal non-complete GN static library carries the
object to the first final link. This follows Chromium's generated-object pattern
in `third_party/nasm/nasm_assemble.gni` and avoids exposing a generated archive
through a path-valued `libs` config.

The first action version exposed its archive through `public_configs`. The
generated archive then appeared on 40 Ninja edges, including 26 unrelated
archives, and a Swift-only edit took 28 steps and 41.51 seconds. After the
object-carrier correction, a body-only Swift edit and its reversal each took
exactly four steps. They completed in 45.80 and 40.97 seconds respectively,
with no C++ or Objective-C++ compilation and no unrelated archive work.

`-add_ast_path` is intentionally absent. It is debugger metadata rather than a
link or runtime requirement. Adding it as an opaque linker flag would discard
GN's dependency modeling. It can be reconsidered if native Swift debugger
visibility becomes a product requirement.

## Link and generated-output evidence

Environment:

- macOS 26.6.2 on arm64;
- Chromium `2b4613f0a24d234b139f1246028239a2f5dde58c`;
- Xcode 26.6, build `17F113`;
- Swift 6.3.3;
- target `arm64-apple-macos13.0`.

The following sizes are the retained pre-Agents measurement from the original
tab-rail checkpoint. The Agents bridge adds Objective-C-visible selectors and
properties, so these values are not claimed as current artifact sizes:

- Swift object: 94,584 bytes;
- Objective-C header: 13,370 bytes;
- Swift module: 24,764 bytes;
- Swift documentation: 420 bytes;
- Swift source info: 1,144 bytes;
- ABI descriptor: 149 bytes.

The generated header contains no `/Users`, `/Applications`, or `/tmp` path.
The object exports `LimitlessSwiftUIRailFactory`. `libchrome_dll.dylib` links
ApplicationServices, AppKit, Foundation, SwiftUI, and `libswiftCore`. Full
`chrome` and `browser_tests` targets link successfully, and an immediate
rebuild performs zero steps.

Chromium's raw Ninja `missingdeps` scan reported no Limitless path. It did
report two pre-existing CrabbyAVIF generated-SDK dependency paths, so the global
scan is not claimed as clean.

## Browser integration evidence

The pre-Agents checkpoint passed seven focused browser tests:

1. The SwiftUI `NSView` attaches to the existing normal browser `NSWindow`,
   reserves a 216-point leading region, suppresses the Chromium horizontal tab
   strip with upstream tab-strip unification disabled, and leaves the real
   content view to its right.
2. The Views host is keyboard-focusable and appears as a named accessibility
   pane in Chromium's accessible-pane ordering.
3. Three real Chromium tabs project exact handles, order, titles, and active
   state into the snapshot consumed by SwiftUI. Title changes update the rail;
   rail activation and close intents mutate the owning tab list; stale handles
   are rejected; and surviving `WebContents` identities remain unchanged. A
   real `TabStripModel` move updates the SwiftUI snapshot to the exact new
   handle order while preserving those identities. The active page title also
   remains present in Chromium's computed accessible window title. Native AX
   propagation remains part of the red real-app gate.
4. Two normal windows receive independent rail state. A foreign window's live
   tab handle is rejected by the first window's activation and close paths.
5. With upstream tab-strip unification and vertical tabs enabled, switching the
   upstream vertical-mode preference constructs neither a horizontal nor a
   vertical Chromium tab strip for the native-rail window.
6. A second normal window gets an independent host. Closing it detaches and
   releases the hosted view, while a popup receives no host.
7. With the switch absent, the ordinary `BrowserView` is unchanged.

The expanded Agents milestone adds three outside-in browser journeys. At that
pre-Leader checkpoint, all three passed with retries disabled and the full
SwiftUI browser suite passed 10 of 10:

1. Every agent in one authoritative Herdr session projects across workspace
   IDs into a regular window with friendly workspace labels, terminal-title
   fallbacks, textual status, and the exact Herdr focused field. An
   off-the-record window does not subscribe and remains explicitly disabled.
2. Forty real Chromium tabs and twelve agents keep both `Workspace` and
   `Agents` headings discoverable. The workspace and agent lists scroll
   independently; Workspace retains at least 80 points of flexible viewport and
   Agents remains visible with a viewport capped at 160 points. Transparent,
   non-hit-testable, non-accessible native probes sit in the real
   `GeometryReader` backgrounds at SwiftUI's proposed bounds. The test reads
   their post-layout `NSView` frames directly and proves positive, in-rail,
   disjoint viewports whose content is larger than each viewport.
3. Refresh changes, empty state, explicit source failures, and recovery update
   the real SwiftUI snapshot without fabricating rows or changing tab state.

Nine focused provider and registry tests pass and cover strict parsing,
protocol and size rejection, exact process invocation, timeout and child cleanup, residual
process-group descendants after normal or nonzero leader exit, no overlapping
refresh, changed publication, unchanged-result coalescing, stop and late-reply
rejection, and transient registry lifecycle. These are product-owned tests and
do not expand Chromium's patch surface.

The leader milestone adds one typed controller per eligible normal window. The
exact prefix is Control+B and the exact second-key map is `n` and `p` for
next/previous tab, `1` through `9` for exact one-based tab selection, Shift+X
for close, and `b` for sidebar visibility. Escape, timeout after two seconds,
an unknown key, window resignation, and teardown cancel the armed state. A
sharp zero-radius status row exposes the available map while armed. The policy
is enabled by default and can be restored to stock Chromium behavior with
`--limitless-leader-shortcuts=disabled`.

Renderer events, the BrowserView root pre-target Views route, and an
in-process macOS CommandDispatcher/local-monitor route all feed the same
window controller and timer. The browser tests use EventGenerator's
`APPLICATION` target to synthesize an `NSEvent` and pass it through `NSApp`.
They prove that Control+B works while the omnibox is focused, the unmodified
second key is consumed by the exact window's local monitor, enabled Leader
precedes Chromium's high-priority accelerator substrate, and disabled or popup
windows retain stock routing. Multi-window tests prove that an event for one
`NSWindow` cannot arm or operate another window. Closing an armed browser
stops its timer, monitor, observer, and weak callbacks.

The renderer route also proves that editable page input retains Control+B and
ordinary text, unknown keys fall through after cancelling Leader, and an
unrelated Command+L continues through Chromium's macOS redispatch path without
changing the committed page or active tab. A retained identical `NSEvent`
routed through renderer and Views adapters increments the arm generation only
once and leaves the exact timer deadline unchanged. This is direct
deduplication evidence rather than an inference from command results.

The tab and agent action tests still make no native-click claim. The leader
`APPLICATION` tests are synthetic, in-process AppKit routing evidence, not
external hardware-input acceptance. Chromium's non-interactive browser-test
harness disables platform accessibility integration. External keyboard and
hardware behavior, IME and marked-text behavior, VoiceOver and native AX
actions, and pixel acceptance therefore remain red and belong to the real-app
gate.

Patch `0006-browser-view-leader-shortcuts.patch` touches exactly two upstream
symbols: `BrowserView::PreHandleKeyboardEvent` in
`chrome/browser/ui/views/frame/browser_view.cc` and
`BrowserNativeWidgetMac::OnWindowInitialized` in
`chrome/browser/ui/views/frame/browser_native_widget_mac.mm`. Because the first
file was already present in the rail patch, the ordered six-patch series now
touches 10 unique upstream files. The controller, adapters, status surface,
typed commands, and tests remain product-owned.

At the pre-Leader Agents checkpoint, the full SwiftUI browser suite passed 10
of 10. With the Leader milestone included and retries disabled, the current
SwiftUI browser suite passes 17 of 17, the focused Herdr browser trio passes 3
of 3, and the provider and registry suite passes 9 of 9. The previously run
Limitless WebUI product suite passes 12 of 12. The wider 35-test browser gate
remains red in the separate upstream WebUI candidate:
`WebUIBrowserTest.FullscreenEnterAndExit` fails its fullscreen assertion and
`WebUIBrowserTest.TabFullscreenEnterAndExit` times out. The same run preserves
the known `WebUIBrowserWindow` and `WebUIStubLocationBar` evidence classified
in `gate-2-rejected-shell-baseline.md`. These failures prevent an all-green
Gate 2 result, but they do not invalidate the passing BrowserView-hosted native
rail tests.

The upstream-shell lifecycle proof also passes five consecutive iterations
after removing an input race. Its renderer-level keyboard path now selects the
existing address with Shift-Home and clears it with one Backspace, waits until
the prior autocomplete query is retired, types the destination, and waits for
the current default match before Enter. The earlier character-by-character
Backspace loop raced inline autocomplete updates and could leave one deletion
unapplied. The corrected proof still makes no native AppKit input claim.

## Herdr presence provider contract

The first adapter resolves its executable once. Resolution accepts an explicit
absolute `--limitless-herdr-executable` override, then checks only
`/opt/homebrew/bin/herdr` and `/usr/local/bin/herdr`. It does not inspect the
inherited `PATH` and does not accept `HERDR_BIN_PATH`. Every candidate must be a
regular executable file. The explicit switch is a trusted operator and test
configuration surface, not an agent-controlled setting.

Each refresh executes the exact argv `[herdr, api, snapshot]` without a shell,
string interpolation, or UI-thread process work. Polls use one sequenced worker,
a two-second interval, and a 1.5-second command timeout. An in-flight request
coalesces additional refresh demand into one pending refresh, and an unchanged
snapshot does not republish. A closing rail detaches from the registry; provider
Stop after the last regular-window observer leaves invalidates late replies.
Output, identifier, title, version, workspace, and agent counts are bounded. The
parser validates the top-level response, result type, snapshot, protocol 20,
version, stable pane and workspace identifiers, statuses, and focused values
before publishing. Child process groups are killed and reaped on timeout and
failure.

The adapter displays all agents returned by `herdr api snapshot` for the current
Herdr session, including agents in different workspaces in that session. It does
not enumerate or aggregate named Herdr sessions. It preserves the returned row
order and projects `pane_id`, `workspace_id`, `agent`, `agent_status`,
`terminal_title_stripped`, and `focused`. Workspace ID remains the identity and
a supplied friendly label from the workspace snapshot is presentation only.
Empty or sanitized agent and terminal titles use stable pane-based fallbacks
rather than rejecting an otherwise valid snapshot.

The failure policy favors truthful absence over unlabeled stale data. A missing
or non-executable binary, permission denial, command failure, timeout, malformed
response, or incompatible protocol clears the currently rendered agent rows and
shows the corresponding accessible state. A ready-empty snapshot shows `No live
agents`. Private windows never observe the registry and show `Agents are
unavailable in private windows`. The current slice has no stale timestamp or
last-good presentation.

This is a display-only presence boundary. It cannot establish that an agent is
using Limitless, authenticate an agent, scope browser authority, or grant
commands, settings writes, page access, revoke or kill controls, or audit-log
entries. Those capabilities remain gated future work.

## Real-app gate

`tests/e2e/gate2b_app_smoke.sh` launches the actual `Limitless.app` executable
with a fresh profile, Webium disabled, the explicit boundary switch, and a
random CDP port. The strict gate requires:

- the pinned build revision embedded in the app bundle's `SCMRevision`, the
  expected bundle and executable identity, and one real `about:blank` page
  target;
- no `chrome://webui-browser` shell target;
- exactly one layer-zero CoreGraphics window owned by the launched PID;
- external AX identifiers for the rail and a real tab row whose parent chains
  reach the same `AXWindow`;
- focus set and read back through `kAXFocusedAttribute`;
- an exact-window native PNG with bounds matching CDP and CoreGraphics;
- `Browser.close`, process exit status zero, disappearance of the exact native
  window, and no singleton profile locks.

The contract suite passes 19 of 19 cases. Once a CDP connection exists, every
validation failure and interrupt still requests `Browser.close`, and a failed
close request is reported alongside the primary failure. The launcher gives
readiness, CDP, and the native probe one shared validation deadline, always
reaps the launched child, and records its exit status. Endpoint failures before
a CDP connection use the launcher's bounded process cleanup because no protocol
session exists through which to request `Browser.close`.

The Retina validator resolves the exact display containing the whole native
window, reads its current display mode, and accepts only normalized 1x or 2x
backing scales. PNG dimensions must lie inside the strict floor-to-ceiling
quantization envelope for the whole point-sized window and inside the matching
aspect-ratio envelope. The probe revalidates exact CoreGraphics and CDP window
identity and bounds around capture. Deterministic contract cases cover 1x, 2x,
fractional point bounds, wrong and unsupported scales, transparent output, and
post-capture window movement.

On 2026-08-27 the corrected real-app run resolved the exact Limitless PID and
window, exercised external AX focus, measured a 1200 by 1366 point window at 2x,
captured a correctly bounded 2400 by 2732 PNG, requested `Browser.close`, and
observed exit status zero. Visual inspection and direct PNG decoding found that
every captured pixel had zero alpha, so the validator rejected the bitmap as
fully transparent. Direct exact-window `screencapture` controls with and without
window shadow produced the same transparent result. Retina point-to-pixel
geometry and the CDP lifecycle are therefore green, while exact-window pixel
acceptance remains an external red gate because this session did not produce a
visible bitmap.

The current hardened real-app run is red because this execution session has no
usable interactive macOS accessibility or screen-capture channel. Limitless had
one onscreen CoreGraphics window, but `kAXWindows`, `kAXMainWindow`, and
`kAXFocusedWindow` returned an `AXApplication` proxy instead of `AXWindow`.
The same control against onscreen WezTerm produced the same result. An
independent Objective-C AX probe confirmed the result, Computer Use could not
start its native pipe, the full-display capture was black, and exact-window
capture failed. This is environment evidence, not a product pass.

The app bundle's embedded `SCMRevision` matched the pinned checkout. CDP still
requested `Browser.close`; the launcher reaped the process with exit status
zero, no `SingletonCookie`, `SingletonLock`, or `SingletonSocket` remained, and
the logs contained no fatal or sanitizer signature. AX never yielded an exact
window identity, so the raw CoreGraphics post-close identity check correctly
did not run. A passing run still requires that check. The latest artifacts are
retained in `/private/tmp/limitless-workspace-checkpoint.sa7KBC` for this
machine session. The browser exited with status zero, the retained profile has
no singleton locks, the CDP result is empty, and no native PNG exists, so no
visual approval is recorded. A separate Computer Use attempt also failed to
start its native pipe, so it supplied no replacement AX or screenshot evidence.

## Decision and remaining gate

The product-owned action is selected over general Chromium macOS Swift
enablement for this bounded module. The generic toolchain patch must not enter
the Limitless patch series.

The BrowserView-hosted SwiftUI seam remains a candidate, not the selected
production shell. Gate 2B still requires a rerun in an unlocked interactive
macOS session that produces the exact-window PNG and passes external AX focus,
parentage, lifecycle, and clean shutdown. The screenshot must then receive a
pixel-level review. The vertical rail itself now uses visible `Workspace` and
`Agents` headings below a 40-point macOS caption-control exclusion, dense
zero-radius rows, a crisp active marker, independent bounded scrolling, and
textual agent status. It suppresses Chromium's horizontal tab strip. Product UI
remains hard red while Chromium's toolbar and other top surfaces are retained
and no exact-window PNG or external AX tree has been inspected. The projected
Herdr focused field is not native focus acceptance.
