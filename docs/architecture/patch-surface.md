# Chromium patch surface

## Summary

This patch budget is for a macOS-only v1. Linux and Windows patch work is deferred and requires a separate architecture review before either platform enters scope.

Neither shell candidate patches the browser-window factory or adds a Limitless `BrowserWindow` subclass. The WebUI experiment reuses `WebUIBrowserWindow`. The SwiftUI experiment hosts a product-owned `NSView` inside the existing `BrowserView`. One follow-up patch replaces the SwiftUI placeholder at the existing construction call with an authoritative tab rail. A second narrow follow-up routes the product-owned per-window leader controller through Chromium's renderer prehandle and native macOS command-dispatcher seams. A third adds product workspace extra data at Chromium's existing session rebuild and restore boundaries. Two independent patches close issuance-time and post-issuance tab-capability gaps in the WebUI candidate without changing either shell boundary. The split baseline adds three separately owned patches: general session split-ratio validation, a desktop Views drag editability lifecycle notification, and test-only external interactive-test wiring.

The smallest Gate 2 validation patch changes two upstream files to let the existing `WebUIBrowserUI` load Limitless-owned resources. The validation launcher supplies `--enable-features=Webium,SurfaceEmbed`.

A distributable build is expected to need five foundation files so product targets can be discovered from one root argument and the two features can be enabled before Chromium creates its `FeatureList`. The WebUI production surface is six unique files while the two page-handler fixes remain unupstreamed and share one file. That production expansion remains unapproved until Gate 2 passes.

| Stage | Upstream files | Purpose |
| --- | ---: | --- |
| Gate 2 validation | 2 | External Limitless resource target and conditional resource selection |
| Gate 2B SwiftUI experiment | 7 | Shared external root plus bounded BrowserView construction and layout seam |
| SwiftUI tab-rail behavior | 1 | Reuse the bounded BrowserView seam to construct the authoritative product-owned tab rail |
| Herdr presence rail slice | 0 | Product-owned provider, registry, SwiftUI projection, bridge, and tests reuse the existing host seam |
| Leader shortcut routing | 2 | Feed renderer and native macOS keyboard routes into one product-owned per-window controller while preserving stock fallback |
| Workspace session metadata | 3 | Regenerate product window and tab extra data during session rebuild and deliver parsed extra data to product reconciliation during restore |
| Session split-data validation | 3 | Reject hostile non-finite or out-of-range restored ratios without losing prior valid state or later commands |
| Desktop Views drag editability lifecycle | 8 | Notify every exact-profile tab model at real drag start and after controller plus dragged-view teardown |
| Split interactive-test wiring | 1 | Link the product-owned split transfer journey into Chromium's existing interactive test executable |
| WebUI tab-handle scope fix | 1 | Same-browser membership check before guest creation |
| WebUI guest-capability lifetime fix | 1 | Permanent revocation on tab detach, contents replacement, and browser close plus per-call authority checks |
| Current combined unique files | 24 | `webui_browser/BUILD.gn`, `webui_browser_page_handler.cc`, and `browser_view.cc` each have intentional series overlap; `0010` intentionally overlaps the WebUI build file |
| Expected production foundation | 5 | Central external-root plumbing, product startup dependency, early feature defaults, and resource selection |
| Expected WebUI production surface with handler fixes | 6 | Five foundation files plus the shared page-handler file while both fixes remain unupstreamed |
| Current browser factory changes | 0 | Existing `WebUIBrowserWindow` selection is reused |
| Current SurfaceEmbed implementation changes | 0 | Existing implementation is tested as-is before any fix is proposed |

## Patch series policy

Patches live in `limitless/patches/chromium/`, are applied in the order listed by `series`, and are based on exact Chromium SHA `2b4613f0a24d234b139f1246028239a2f5dde58c`. Each manifest entry records the patch SHA-256, upstream files and symbols, dependent Limitless targets, tests, conflict signals, and removal condition.

Application rules:

- require the exact recorded Chromium base;
- require a clean Chromium worktree before applying;
- run a strict dry-run check before mutation;
- do not use a three-way fallback, fuzz, or silent offset acceptance;
- apply one coherent patch at a time;
- verify the resulting touched-file set against the manifest;
- never include generated files, `CHANGELOG.md`, product styling, or business logic in an upstream patch.

## Initial Gate 2 validation patch

### `0001-webui-browser-limitless-resources.patch`

#### Upstream files and symbols

1. `chrome/browser/ui/webui_browser/BUILD.gn`
   - add a `limitless_root` string argument with an empty default;
   - when non-empty, add the system-absolute external `:resources` and product `:webui_browser_tests` targets to `source_set("impl")` and `source_set("browser_tests")` as appropriate;
   - add a target-local `LIMITLESS_BUILD` define only to the targets that select generated Limitless resources;
   - assert that the external `BUILD.gn` exists and fail with an actionable path message.
2. `chrome/browser/ui/webui_browser/webui_browser_ui.cc`
   - in `WebUIBrowserUI::WebUIBrowserUI()`, conditionally include and install the generated Limitless resource map and HTML entry resource;
   - retain `WebUIBrowserUI`, the `chrome://webui-browser` origin, existing Mojo binders, tab-strip APIs, shared toolbar resources needed by the initial frontend, localized strings, favicon source, and SurfaceEmbed CSP;
   - leave the Chromium resource map as the default when `LIMITLESS_BUILD` is absent.

#### Why an existing seam is not sufficient

The factory and feature paths are already sufficient. Resource selection is not: the pinned `WebUIBrowserUI` constructor directly names `kWebuiBrowserResources` and `IDR_WEBUI_BROWSER_WEBUI_BROWSER_HTML`. GN has no existing embedder callback or alternate-resource argument at that point.

#### Dependent Limitless code

- `$limitless_root/build/chromium:resources`, generated by the pinned `build_webui` template;
- Limitless WebUI TypeScript, HTML, CSS, icons, and generated resource map;
- a Limitless browser-test source set containing the semantic resource smoke test.

#### Tests

- GN generation with no `limitless_root`, proving Chromium-only behavior;
- GN generation with a valid external root;
- GN generation with an invalid root, proving a clear failure;
- TypeScript compile, WebUI lint, CSS lint, GRIT generation, and duplicate-ID checks;
- a browser test that opens a normal WebUI Browser window and finds a unique Limitless semantic element and build identifier;
- existing `WebUIBrowserTest.StartupAndShutdown`, navigation, fullscreen, DevTools, focus, and close tests;
- new SurfaceEmbed-first tests described below.

#### Expected rebase conflict signals

- `WebUIBrowserUI::WebUIBrowserUI()` no longer creates the `chrome://webui-browser` data source;
- `kWebuiBrowserResources` or the default HTML resource identifier is renamed or removed;
- `source_set("impl")` or `source_set("browser_tests")` is split or renamed;
- the `build_webui` or generated-resource-map contract changes;
- the WebUI Browser origin, controller type, or binder restriction changes;
- the Limitless external label no longer resolves under GN's external-label rules.

#### Removal or upstreaming path

Propose a generic alternate WebUI Browser resource-provider or embedder target to Chromium. Once the upstream target accepts a resource map and entry resource without product conditionals, remove both hunks. If WebUI Browser itself gains a supported custom-shell target, use that contract directly.

## Gate 2B SwiftUI experiment patch

### `0002-browser-view-swiftui-boundary-probe.patch`

This is an experimental seam, not production shell approval.

#### Upstream files and symbols

1. `chrome/browser/ui/limitless.gni`
   - declares one empty-by-default external product root;
   - validates the product build entry only when configured.
2. `chrome/browser/ui/BUILD.gn`
   - imports the shared argument;
   - conditionally links the product-owned SwiftUI rail into the existing UI target.
3. `chrome/browser/ui/views/frame/browser_view.cc`
   - constructs the optional host only for normal windows and behind the explicit probe switch;
   - includes it in accessible-pane order and layout registration.
4. `chrome/browser/ui/views/frame/browser_view.h`
   - stores the optional child pointer and exposes it to focused browser tests.
5. `chrome/browser/ui/views/frame/layout/browser_view_layout.h`
   - adds the optional layout participant.
6. `chrome/browser/ui/views/frame/layout/browser_view_tabbed_layout_impl.cc`
   - reserves the host's preferred leading width in minimum and proposed layout without hiding or reparenting page content.
7. `chrome/browser/ui/webui_browser/BUILD.gn`
   - imports the shared root instead of declaring it locally;
   - includes the separately owned SwiftUI browser-test source set.

#### Product-owned build route

The Swift compiler action, generated-header normalization, SwiftUI view,
Objective-C++ host, browser tests, and E2E harness remain in `limitless/`. The
action emits one object and generated module artifacts. A normal GN static
library carries that object to the final link. No Chromium Apple toolchain file
is patched.

The rejected generic alternative changed three central Chromium build files by
34 additions and 6 deletions, did not link Chrome, had incomplete release and
remote-execution evidence, and emitted an undeclared diagnostic output. See
`gate-2b-swiftui-boundary-evidence.md`.

#### Tests and acceptance boundary

- full `chrome` and `browser_tests` build;
- four focused SwiftUI BrowserView tests;
- full Limitless WebUI Browser browser-test suite to detect candidate interaction;
- generated-header path and symbol checks;
- Swift-only and no-op incremental measurements;
- strict real-app CDP, external AX, native focus, exact-window PNG, lifecycle, and clean-close E2E.

The deterministic build and browser tests pass. The real-app gate remains red
in the current headless session and must pass in an unlocked interactive macOS
session before production selection.

#### Rebase signals and removal path

Review if `BrowserView` child construction, `BrowserViewLayoutViews`, tabbed
layout calculation, accessible-pane ordering, normal-window type selection, or
the product test dependency seam changes. Remove the patch if the interactive
gate fails for a product defect, if the selected shell uses another boundary,
or when Chromium exposes a supported macOS native-shell composition seam.

## WebUI tab-handle scope patch

### `0003-webui-browser-tab-handle-scope.patch`

This patch changes only
`chrome/browser/ui/webui_browser/webui_browser_page_handler.cc`, in
`WebUIBrowserPageHandler::GetGuestIdForTabId()`.

`TabHandle::Get()` resolves liveness through a UI-process-wide handle factory.
Liveness does not authorize the requesting shell to operate on the tab. Before
creating `WebUIBrowserGuestHandler` or `GuestContentsHandle`, the handler now
requires the resolved tab to be a member of the requesting browser's
`TabStripModel`. This rejects another window's tab, including a tab in another
profile, without changing the Mojo schema or tab ownership. Inactive tabs in
the same window remain valid. Invalid, foreign, and contentless inputs report a
bad message through the self-owned `DocumentService`, which closes the handler
and drops pending replies.

The focused browser proof covers:

- an inactive tab in the requesting window is accepted;
- a live tab in another window of the same profile is rejected;
- regular-to-incognito and incognito-to-regular handles are rejected;
- a handle captured before tab close is rejected after it becomes stale.

Review this patch if `NodeId` or `TabHandle` resolution, page-handler ownership,
or `TabStripModel` membership semantics change. Remove it when Chromium performs
equivalent requesting-browser authorization, or if the WebUI candidate is
removed.

## WebUI guest-capability lifetime patch

### `0004-webui-browser-guest-handler-lifetime.patch`

This patch also changes only
`chrome/browser/ui/webui_browser/webui_browser_page_handler.cc`. It is separate
from `0003` because browser membership at capability issuance and authority
after issuance have independent tests and removal conditions. The patch
generator compares it against the complete post-`0003` tree and verifies the
intentional path overlap.

`WebUIBrowserGuestHandler` now retains the accepted `TabHandle`, the originating
browser session identity, a weak controller, and the exact `WebContents` bound
when the handler was issued. Every operation re-resolves the tab and requires
the same browser, profile, tab-strip membership, live window, and bound
contents. Page-info operations additionally require the tab to be active.
Security-icon requests for a valid inactive tab return a conservative warning
without exposing the active tab's location-bar security identity or revoking
the handler.

Normal lifecycle changes close only the guest capability without reporting a
bad renderer message. Moving or closing the tab permanently revokes the old
remote through `RegisterWillDetach`. A contents replacement or successful
browser close also revokes it. A moved tab can be controlled only through a
fresh handler issued by its destination window. The focused browser proof
covers absence of a started or pending post-transfer stale navigation, clean
close revocation, unchanged destination contents, and a fresh
destination-window handler. This patch revokes the `GuestHandler` command
remote, not the SurfaceEmbed guest ID, which remains owned by the global
`GuestContentsHandle` for that `WebContents`.

The supported background-discard journey is not claimed as green. Calling
`TabListInterface::DiscardTab` for a SurfaceEmbed-hosted tab currently reaches
`PageNodeImpl::OnAboutToBeDiscarded` and fails its primary-page-node DCHECK
because the page is represented as an embedded node. No synthetic replacement
test substitutes for that result. This is explicit Gate 2 evidence against the
current WebUI candidate; the revocation callback remains defense for any valid
contents-replacement path that does notify `TabInterface`.

Review this patch if `TabInterface` detach or discard callbacks, `TabHandle`
resolution, `BrowserWindowInterface` close callbacks, session identity,
`GuestContentsHandle` lifetime, or page-handler ownership changes. Remove it
when Chromium provides equivalent window-scoped, revocable `GuestHandler`
command capabilities, or if the WebUI candidate is removed.

## SwiftUI tab-rail behavior patch

### `0005-browser-view-swiftui-tab-rail.patch`

This patch remains narrow at one upstream file,
`chrome/browser/ui/views/frame/browser_view.cc`, but spans the BrowserView
seams required to make suppression correct. It replaces the placeholder hook
with the authoritative rail construction call, clears the child pointer during
teardown, selects an uninitialized upstream region while the native rail owns
tab presentation, blocks horizontal and vertical strip initialization across
mode changes, omits the suppressed strips from accessible panes, preserves the
active page in the accessible window title, and guards per-tab accessibility
updates that require upstream tab views. The construction and layout host seam
remains owned by `0002`; overlap on `browser_view.cc` is therefore intentional
and checked in series order.

All tab observation, snapshot projection, title observation, mutation
authorization, SwiftUI rendering, and browser tests remain in the Limitless
repository. The rail consumes exact Chromium tab handles, order, title, and
active state. Activation and close actions return through `TabListInterface`
after browser-, profile-, membership-, liveness-, and editability checks. A
real `TabStripModel` move updates the exact SwiftUI-consumed handle order while
preserving each surviving `WebContents` identity.

The behavior tests do not claim external macOS keyboard, pointer, VoiceOver, or
out-of-process AX action input. The in-process AX proof materializes SwiftUI's
native tree through the public AX API, traverses the real rail controls, and
presses real create, switch, and lazily generated move-menu items. The closed
40-tab, 12-workspace tree stays linear and contains no inactive-workspace menu
items; opening one move menu creates exactly 11 destination items. The tests
also prove the Chromium horizontal tab strip is not constructed with upstream
tab-strip unification disabled, and that neither upstream strip is constructed
after a vertical-mode transition with unification enabled. The sharp, dense
rail is still not visually accepted: retained Chromium toolbar and other top
surfaces plus the absence of an exact-window PNG keep the pixel gate red.

The display-only Agents extension does not change this patch. Its Herdr CLI
provider, transient browser-process registry, Objective-C++ projection, SwiftUI
rows, and focused tests all remain under `limitless/`. It reuses the existing
BrowserView host and product test target. The adapter shows every agent returned
by the current Herdr session across that session's workspaces, does not aggregate
named sessions, and grants no browser capability. Off-the-record windows do not
subscribe. That Herdr milestone therefore keeps the same patch SHA, upstream
file, symbols, dependent external targets, and removal condition while
expanding the recorded product validation filters. It adds no upstream file;
the later leader-key milestone is the separately accounted `0006` patch.

At the pre-split checkpoint, the focused Herdr browser trio passed 3 of 3, the
native SwiftUI suite including workspaces and Leader passed 27 of 27, and the
provider and registry suite passed 9 of 9 with retries disabled. The bounded-section proof
uses 40 real Chromium tabs, 12 workspaces, 12 agents, and transparent native
probes occupying the actual `GeometryReader`-proposed viewport and content
bounds. It reads their post-layout `NSView` frames to prove rail containment,
disjoint viewports, minimum and capped sizes, and overflowing scroll content.
These probes do not draw, handle input, or participate in accessibility.
External pixel and AX acceptance therefore remain red and the upstream patch
accounting does not change.

Review this patch if the `BrowserView` construction order no longer exposes a
live `BrowserWindowInterface`, if Chromium adds a supported product-sidebar
factory, or if the interactive Gate 2B gate finds a product defect. Remove it
with the candidate shell or when the supported seam replaces it.

## Native leader shortcut routing patch

### `0006-browser-view-leader-shortcuts.patch`

This patch changes two upstream files while keeping all leader state, command
mapping, policy, exact-window authorization, timeout behavior, SwiftUI status,
and teardown in Limitless-owned code. `BrowserView::PreHandleKeyboardEvent()`
feeds renderer-originated keyboard events into the existing rail host before
Chromium resolves ordinary accelerators. On macOS,
`BrowserNativeWidgetMac::OnWindowInitialized()` asks the same exact normal
window for a product-owned command-dispatcher delegate, then installs
Chromium's stock delegate unchanged when the controller is disabled or the
window is ineligible. The product factory is a production seam and does not
depend on the rail's testing accessor.

The controller is scoped to one normal browser window. Its first map reserves
Ctrl+B as the prefix, then accepts `n`, `p`, exact indexes `1` through `9`,
Shift+X, and `b`. Escape, a two-second timeout, an unknown key, window
deactivation, and teardown reset the armed state. No pane commands are claimed.
The command-line policy defaults to enabled, accepts the explicit value
`enabled`, and fails closed for `disabled` or any other supplied value. The
disabled path returns renderer events, native accelerators, and ordinary page
typing to Chromium unchanged.

Seven outside-in browser journeys cover the full map and sharp armed status,
cancel and timeout behavior, ordinary typing and unrelated command fallback,
the renderer and native Views routes, identical-event deduplication, an
Application-target AppKit route ahead of a high-priority accelerator,
multi-window isolation, popup exclusion, exact-window teardown, and disabled
policy fallback. These are deterministic in-process routing proofs. They do
not establish external hardware input, IME, VoiceOver, native AX, or pixel
acceptance. Retained Chromium top surfaces and the missing exact-window PNG
keep visual acceptance red.

Review this patch if `BrowserView` keyboard prehandling moves relative to
editable-content checks or stock accelerator resolution; if
`BrowserNativeWidgetMac` moves command-dispatcher installation; if the native
delegate, redispatch, first-responder, or marked-text contracts change; or if
the exact rail controller is no longer live at window initialization. Remove
it when Chromium exposes a supported per-window command seam spanning renderer
and native Views routes with stock fallback, with the candidate shell, or if
the interactive Gate 2B input gate fails for a product defect.

## Workspace session metadata patch

### `0007-session-workspace-metadata.patch`

This patch adds 45 lines across three upstream session files. All workspace
identity, validation, profile isolation, live-tab reconciliation, persistence
policy, typed operations, and SwiftUI projection remain in Limitless-owned
code.

`chrome/browser/sessions/BUILD.gn` conditionally links the product workspace
core into the existing session implementation for Limitless macOS builds.
`SessionServiceBase::BuildCommandsForBrowser()` and
`SessionServiceBase::BuildCommandsForTab()` append product window and tab extra
data while Chromium rebuilds its authoritative session command stream.
Mutation-time `AddWindowExtraData` and `AddTabExtraData` calls are insufficient
because that rebuild replaces the earlier command stream.

`SessionRestoreImpl::ProcessSessionWindows()` delivers parsed window extra data
after the real normal `BrowserWindowInterface` exists and before its restored
tabs are created. `SessionRestoreImpl::RestoreTab()` delivers parsed tab extra
data after Chromium creates the real restored `WebContents`. These are the
first narrow restore points holding both the parsed session envelope and the
authoritative product target. The product callback never takes page ownership
or retains the `WebContents`.

The focused proof covers create, switch, move, physical reorder, discard and
replacement, transfer, close, close-all, teardown, Leader ordering, exact
multiwindow and profile rejection, off-the-record ephemerality, malformed and
duplicate data, missing last-active tabs, and disabled policy. A PRE/post
browser test performs a clean shutdown and separate-process restart with two
real windows, stable product IDs, and three workspace records including two in
the source window, plus a transferred tab, exact last-active pages, and a page
closed before shutdown. A second PRE/post journey restores three Chromium-owned
split pairs across two workspaces with exact product UUID membership, order,
layout, 25/50/75 ratios, selection, native rail projection, and focused-page
input routing. Malformed tab metadata dissolves only the pair whose product
workspace membership becomes invalid, while a later duplicate product tab UUID
is rekeyed and its otherwise valid native pair survives. The product-owned
restore epoch blocks typed workspace and split mutation during partial restore,
and profile finalization plus callback-capable activation paths use weak
liveness checks. A focused same-profile regression destroys a later window from
an earlier controller's restore-driven activation callback and proves the
profile-wide finalizer skips the invalidated controller. This adds no upstream
file or symbol to the patch. SwiftUI and full retained browser suites remain
regression gates.

Review this patch if session command rebuild ownership moves out of
`BuildCommandsForBrowser()` or `BuildCommandsForTab()`, if parsed window or tab
extra data moves away from `ProcessSessionWindows()` or `RestoreTab()`, if
normal-window session restoration no longer creates authoritative tabs at
those points, if the public session-restored callback no longer runs after tab,
native split, and selected-tab reconstruction, or if Chromium introduces a
supported external session extra-data provider. Remove it when that provider
can both rebuild and restore typed window and tab metadata without direct
session hooks, or with the native workspace feature if its product journey is
removed.

## Session split visual-data validation patch

### `0008-session-split-visual-data-validation.patch`

This general safety patch changes exactly three session-component files. The
session parser treats all restored split visual data as untrusted. Before
constructing `SplitTabVisualData`, `CreateTabsAndWindows()` now requires a
finite ratio in Chromium's inclusive `[0, 1]` domain. An invalid update is
ignored without overwriting earlier valid visual data and without stopping
later session commands. Unknown layout strings retain Chromium's deterministic
side-by-side fallback and preserve a separately valid ratio.

The session unit-test source set gains the exact direct
`//components/split_tabs:split_tabs` dependency required by its new public
includes. Five `SessionServiceCommandsTest` cases cover NaN, both infinities,
out-of-range values, valid endpoints, prior-value preservation, later-command
processing, and unknown-layout fallback. No Limitless split state is persisted.

Review this patch if session split restoration leaves
`CreateTabsAndWindows()`, the ratio domain changes, or Chromium validates this
data earlier. Remove it when upstream supplies equivalent strict validation
with the same preservation and command-continuation behavior.

## Desktop Views drag editability lifecycle patch

### `0009-tab-strip-editability-lifecycle.patch`

This is a separate generic lifecycle seam across eight upstream files. After a
successful desktop Views `TabDragController::StartDrag()`, the controller
broadcasts the authoritative profile-wide noneditable level to every current
exact-profile window that has a `TabStripModel`. Stop callbacks schedule a
WeakPtr-guarded recomputation rather than trusting the callback level. Final
editability is recomputed after the drag controller is destroyed and, when tab
bounds animate, after `ResetDraggingStateDelegate` has cleared every dragged
view. A no-animation fallback and a replacement-drag guard preserve the same
event-driven contract without polling or fixed delays. Consumers treat the
notification only as a wakeup and revalidate live authority.

The generic real drag/drop interactive test requires its first false edge while
the drag controller is active, contemporaneous equality with
`TabListInterface::CanEditTabList()`, and one terminal true only after the drag
controller is inactive. Chromium's copied detach route and both enabled direct
drop controls fail before destination active-drag on the current macOS arm64
system-DnD harness. The new generic test therefore follows Chromium's existing
Mac arm64 platform disable for that route. This remains an explicit external
native-input gate, not a green drag claim. Deterministic product controller
coverage plus the native SwiftUI read-only journey prove that false-level
notifications retain authoritative pairs while disabling actions and that a
later wakeup revalidates reconciliation.

Review this patch if `TabDragController` no longer owns the successful drag
start, if dragged-view cleanup moves outside `OnBoundsAnimatorDone()`, or if
profile-wide tab-list editability changes contract. Remove it when Chromium
provides an observer whose terminal notification is guaranteed after both drag
controller and dragged-view teardown for every affected tab list.

## Split interactive-test wiring patch

### `0010-webui-browser-split-test-wiring.patch`

This test-only patch adds four lines to the existing WebUI Browser
`source_set("interactive_ui_tests")`. When `limitless_root` is configured, it
links `$limitless_root/build/chromium:split_interactive_ui_tests`. It adds no
production dependency or behavior. The overlap with
`chrome/browser/ui/webui_browser/BUILD.gn` in `0001` and `0002` is intentional
and verified in series order.

Review this patch if the interactive test source set or external-root label
contract changes. Remove it when Chromium provides a supported external
interactive browser-test dependency seam, or with the native split journey.

## Expected production-only patches

These unnumbered replacement concepts make the likely production surface visible. They are not entries in the current ordered series, must not reuse the current `0001` through `0010` filenames, and are not approved until Gate 2 succeeds.

### Future external build-root replacement

#### Upstream files and symbols

1. `build/config/chrome_build.gni`
   - declare `limitless_root = ""` once for desktop builds;
   - derive a GN-only `is_limitless_build = limitless_root != ""` value;
   - preserve all Chromium and Google Chrome branding branches when false.
2. `chrome/BUILD.gn`
   - add the external product startup target to `group("dependencies")` when `is_limitless_build` is true;
   - consume a product target whose public config supplies the narrow compile define needed by the patched startup call;
   - do not rename the upstream `chrome` build target.
3. `chrome/browser/ui/webui_browser/BUILD.gn`
   - import the central argument rather than declaring a local validation argument;
   - add external shell resource and browser-test targets only when `is_limitless_build` is true.

#### Why an existing seam is not sufficient

GN supports external system-absolute labels but Chromium has no existing argument naming the sibling Limitless repository. A single central argument avoids repeated environment lookups and path-specific local edits.

#### Tests

- GN generation on macOS with the argument empty, valid, and invalid;
- `gn check` for external product targets;
- a Chromium-only build proving the default graph is unchanged;
- a Limitless build proving the startup and resource targets are linked.

#### Rebase signals

- `chrome_build.gni` changes branding argument ownership;
- `group("dependencies")` is renamed or platform targets stop using it as a public dependency;
- GN rejects the external label on macOS;
- the product define no longer propagates to `chrome_main_delegate.cc`.

#### Removal path

Remove the patch if the repositories are deliberately colocated under Chromium's source root through a supported dependency mechanism, or if Chromium gains a generic external embedder-root argument. Do not replace it with an untracked copy or symlink.

### Future WebUI Browser resource replacement

This is the production form of the validation patch. It touches only `chrome/browser/ui/webui_browser/webui_browser_ui.cc`, while its dependency declaration moves into the central build-root patch.

Its tests, rebase signals, and removal path are the same as the validation patch.

### Future feature-default replacement

#### Upstream file and symbol

- `chrome/app/chrome_main_delegate.cc`, `ChromeMainDelegate::PostEarlyInitialization()`:
  - immediately before the desktop call to `ChromeFeatureListCreator::CreateFeatureList()`, call a product-owned `limitless::ConfigureFeatureDefaults(base::CommandLine*)` under the propagated product build define;
  - the product function uses `base::ScopedAddFeatureFlags::EnableIfNotSet()` for `features::kWebium` and `surface_embed::features::kSurfaceEmbed`;
  - an explicit user enable or disable remains authoritative;
  - child processes receive Chromium's normal propagated feature command line.

The implementation of `ConfigureFeatureDefaults()` remains in `limitless/` and links through the external startup target added to `//chrome:dependencies`.

#### Why an existing seam is not sufficient

Both selected upstream features are disabled by default. Launch flags are suitable for Gate 2, but a production app must behave correctly when launched by the OS, from a file association, by an updater, or from another process. The default must be installed before `FeatureList` creation without changing global defaults for ordinary Chromium builds.

At the pinned revision, `ChromeMainDelegate::IsInitFeatureListEarly()` returns false on macOS, and `PostEarlyInitialization()` creates the feature list after confirming it is in the browser process. The hook is therefore early enough for the browser and for feature propagation to renderers in the v1 app.

#### Tests

- unit test for merging with no feature switches;
- user `--disable-features=Webium` and `SurfaceEmbed` override tests;
- user `--enable-features` preservation test;
- process smoke test proving both features are enabled before normal-window construction and renderer plugin creation;
- launch tests through the app bundle, executable, and file association without validation flags.

#### Rebase signals

- desktop feature-list creation moves out of `PostEarlyInitialization()`;
- `IsInitFeatureListEarly()` changes for macOS;
- `ScopedAddFeatureFlags` merge precedence changes;
- Webium or SurfaceEmbed is renamed, removed, enabled by default, or replaced;
- child-process feature propagation changes.

#### Removal path

Drop this patch if WebUI Browser and SurfaceEmbed become production defaults or expose an upstream-supported product configuration before feature-list creation. If Gate 2 fails, drop it together with the WebUI resource patch.

## Branding patch boundary

No branding asset patch belongs in the substrate experiment. `branding_file_path` already supports an external Limitless `BRANDING` file for the product name and identifiers.

Before distribution, icons and fully branded strings need a separate decision because `branding_path_component` and `branding_path_product` assume Chromium or Google Chrome relative directory layouts. A future branding patch may touch:

- `build/config/chrome_build.gni` for a generic external branding asset root;
- `chrome/BUILD.gn` for macOS product icons;
- string targets in `chrome/app/BUILD.gn` and `components/strings/BUILD.gn` if Limitless supplies complete localized product strings.

Linux and Windows assets, installers, and package metadata are deferred beyond v1.

This work must not set `is_chrome_branded=true`, materialize untracked asset trees, or mix packaging logic into the UI-substrate patches.

## Explicit non-patches

The initial architecture should not modify:

- `chrome/browser/ui/views/frame/browser_window_factory.cc`;
- `chrome/browser/ui/browser.cc`;
- `chrome/browser/ui/browser_window/internal/browser_window_features.cc`;
- `chrome/browser/ui/browser_window.h`;
- `components/surface_embed/browser/surface_embed_host.cc`;
- `components/surface_embed/renderer/surface_embed_web_plugin.cc`;
- sandbox, site-isolation, permission, Safe Browsing, certificate, or network code;
- generated resource files or any `CHANGELOG.md`.

If a Gate 2 failure appears to require one of these changes, first reproduce it in the built browser, add a failing end-to-end or highest-value regression test, and revisit the architecture. A fix is not presumed to be authorized merely because a likely file has been identified.

## Watched upstream symbols and rebase contract

The update check records source hashes or structural checks for these symbols. A change forces architecture review even if patches apply cleanly.

| Area | Pinned path and symbol | Why it matters |
| --- | --- | --- |
| Window selection | `chrome/browser/ui/views/frame/browser_window_factory.cc`, `BrowserWindow::CreateBrowserWindow()` | Must continue selecting `WebUIBrowserWindow` only for intended normal windows |
| Browser lifetime | `chrome/browser/ui/browser.cc`, `Browser::Browser()` and `Browser::~Browser()` | Window and feature initialization and teardown order |
| Dual feature path | `chrome/browser/ui/browser_window/internal/browser_window_features.cc`, `Init()` and `InitPostWindowConstruction()` | Prevents accidental fallthrough to stub behavior |
| Shell window | `chrome/browser/ui/webui_browser/webui_browser_window.cc`, constructor, close path, accelerators, active-tab updates, fullscreen | Main native and shell lifecycle |
| macOS window | `chrome/browser/ui/webui_browser/webui_browser_window_mac.mm`, `WebUIBrowserNativeWidgetMac` | Menus, shortcuts, title bar, native window class |
| Native hit testing | `chrome/browser/ui/webui_browser/webui_browser_client_view.cc`, draggable regions and permission-anchor updates | Dragging and security prompt placement |
| Trusted controller | `chrome/browser/ui/webui_browser/webui_browser_ui.cc`, constructor and binder methods | Resources, CSP, Mojo exposure, SurfaceEmbed flag |
| Privileged requests | `chrome/browser/ui/webui_browser/webui_browser_page_handler.cc`, `WebUIBrowserPageHandler` and guest handler | Browser-side validation and commands |
| API contract | `chrome/browser/ui/webui_browser/browser.mojom` | Frontend capability surface |
| Surface binder | `chrome/browser/chrome_content_browser_client_receiver_bindings.cc`, SurfaceEmbed binder registration | Restricts the renderer that can request embedding |
| Surface ownership | `components/surface_embed/browser/surface_embed_host.cc`, `AttachConnector()` and `DetachConnector()` | Independent tab ownership and reparenting |
| AX and focus | SurfaceEmbed host and renderer `SetParentAccessibilityInfo()`, `SendAccessibilityInfo()`, `UpdateFocus()` | Cross-surface accessibility and focus |
| Feature state | `chrome/common/chrome_features.cc` `kWebium`; `components/surface_embed/common/features.cc` `kSurfaceEmbed` | Product default and prototype status |
| Resource graph | WebUI Browser C++ and resource `BUILD.gn` files | Generated maps, TypeScript, Mojo, and lint integration |

Also track:

- count and names of `NOTIMPLEMENTED` and `TODO(webium)` sites in `chrome/browser/ui/webui_browser`;
- disabled WebUI Browser and SurfaceEmbed tests by platform and sanitizer;
- pure virtual count in `chrome/browser/ui/browser_window.h` as an indicator of replacement cost;
- upstream additions that directly cast to `BrowserView` and omit a WebUI Browser path.

## SurfaceEmbed-first test additions

Most pinned `WebUIBrowserTest` cases enable `kAttachUnownedInnerWebContents`; only the pixel fixture explicitly enables SurfaceEmbed. Before relying on the substrate, product or upstream tests must rerun these journeys with `kWebium` and `kSurfaceEmbed` and with GuestContents proxy attachment disabled:

- startup and shutdown;
- close by closing all tabs;
- navigation and tab switching;
- DevTools target enumeration;
- browser and tab fullscreen;
- page-content focus and return focus to the shell;
- pointer and keyboard lock;
- tab close without blanking the remaining tab;
- set content size before and after shell layout;
- shell, tab, and GPU-process crash behavior;
- accessibility-tree stitching;
- IME, drag and drop, Retina and mixed-display scaling, and multi-window reparenting.

The disabled Linux compositor pixel test remains a future portability warning, not a macOS Gate 2 requirement.

## Patch growth stop conditions

Stop and reconsider the conditional hybrid if any of these occurs:

- more than the expected six-file WebUI production surface, including the shared file for the independent handler fixes while required, must be patched before feature completeness work begins;
- a patch is required in sandbox, site-isolation, navigation security, permission validation, or Safe Browsing code;
- Limitless must fork `WebUIBrowserWindow`, `WebUIBrowserUI`, or SurfaceEmbed rather than contribute a generic fix;
- the browser-window factory needs a third product-specific type;
- normal browser journeys require broad implementation inside upstream Webium files instead of product adapters or focused upstream fixes;
- patch application succeeds but watched symbols or tests reveal repeated silent behavioral drift;
- GuestContents becomes necessary for a production build;
- macOS-specific patches proliferate instead of remaining behind one narrow product boundary.

At every Chromium update, report patch count, touched-file count, additions and deletions, conflicts by symbol, disabled-test count, and whether any patch can be removed.
