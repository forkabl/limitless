# ADR-0001: Limitless desktop browser shell

## Status

Open at Gate 2B. The WebUI seam has useful browser-state evidence but is
product-UI hard red. A bounded BrowserView-hosted SwiftUI candidate now builds,
links, runs, projects authoritative tabs into a sharp vertical rail, suppresses
Chromium's horizontal tab strip, and includes a display-only Agents section
backed by the current Herdr session. The tab, presence, and shortcut work
remains a bounded product-facing evidence slice, not production-shell
selection. For the first shortcut journey, a typed per-window leader controller
now joins renderer, Views, and synthetic in-process AppKit routes. The native
candidate also operates Chromium-owned two-tab splits through a product-owned
typed adapter and sharp lazy SwiftUI controls without persisting parallel split
state. Current bounded evidence includes the parser 5 of 5, split controller 8
of 8, provider and registry 9 of 9, WebUI 12 of 12, focused native split 3 of 3,
and focused Leader sequence 3 of 3. A broad launcher run exposed an intermittent
InitialWebUI toolbar first-paint stall before fixture setup; the focused
predecessor, affected test, and successor sequence passes, so no Leader code
change is justified. External hardware input, native drag on the current Mac
arm64 system-DnD harness, IME, VoiceOver and native AX, focus, and exact-window
visual gates remain red. Retained Chromium toolbar and other top surfaces also
remain visually unacceptable. See `gate-2b-swiftui-boundary-evidence.md` and
`split-pane-model.md`.

## Baseline and decision scope

The evidence in this record is from Chromium commit `2b4613f0a24d234b139f1246028239a2f5dde58c`. Limitless v1 supports macOS only. Linux and Windows research is retained as deferred portability evidence, not as a v1 implementation, CI, test, packaging, or release commitment. Android, ChromeOS, Linux, and Windows are outside the v1 product scope.

The observed local baseline and distributed v1 target are macOS arm64 on Apple silicon. Intel is outside the v1 build, test, packaging, and release scope. The shell architecture should still avoid unnecessary CPU-specific behavior.

This record decides the ownership and process boundary for the primary Limitless browser shell. Product information architecture, visual layout, themes, tab presentation, and feature requirements are authoritative in `docs/features.md` and ordered in `docs/v1-product-roadmap.md`. Arc and Mori remain design references rather than architecture authorities; their lessons are assessed separately in `design-reference-assessment.md`.

## Decision

No production shell is selected yet. Preserve Chromium's browser-state, security, profile, page-content, prompt, and native-window authority for every candidate. Gate 2 compares:

- a Limitless-owned trusted frontend on Chromium's existing `WebUIBrowserWindow` and SurfaceEmbed substrate; and
- a product-owned SwiftUI view hosted by `views::NativeViewHost` inside the existing `BrowserView` composition.

The detailed build-route comparison selects a Limitless-owned GN action over general Chromium macOS Swift enablement for the bounded SwiftUI module. This is a compilation decision, not shell approval. The SwiftUI seam still needs a passing interactive real-app gate and pixel review.

The WebUI candidate is defined as follows:

- Chromium continues to own `Browser`, `TabStripModel`, `TabInterface`, `WebContents`, navigation, profiles, sessions, browser commands, extensions, security state, permissions, downloads, and other browser services.
- Chromium's native `views::Widget` and `WebUIBrowserNativeWidgetMac` continue to own the top-level macOS window, activation, resize, traffic-light integration, fullscreen, menus, and AppKit event routing.
- Limitless owns a trusted WebUI frontend for the visible normal-window shell, compiled from `limitless/` and exposed through narrow typed Mojo interfaces.
- Normal tab pages remain independent Chromium `WebContents`. They are visually and interactively attached to the shell with `SurfaceEmbed`, which does not expose a page scripting API to the shell.
- Existing Chromium native or WebUI surfaces are retained initially for security-sensitive prompts, dialogs, menus, file interactions, DevTools, and other completeness-critical UI.
- Only `BrowserWindowInterface::TYPE_NORMAL` uses the Limitless shell at first. App, popup, DevTools, picture-in-picture, profile-picker, and other secondary window types keep Chromium's existing implementations until separately classified and validated.

Do not create a third full `BrowserWindow` implementation during the initial integration. Do not copy `BrowserView` or `WebUIBrowserWindow`. Do not ship the current Chromium Webium frontend as the Limitless product UI. Reuse the native and browser-process substrate, and replace or extend only the frontend resources and narrowly required bindings.

Do not use the legacy GuestContents attachment mode in production. The Chromium source explicitly says that it is not approved for production in `WebUIBrowserWindow::WebUIBrowserWindow`. The selected content attachment is `SurfaceEmbed`, subject to the Gate 2 validation below.

## Why the WebUI candidate was initially narrow

The browser model and browser window are already separated before visible UI construction:

1. `CreateBrowserWindow()` in `chrome/browser/ui/browser_window/internal/create_browser_window_non_android.cc` delegates to `Browser::Create()`.
2. `Browser::Browser()` in `chrome/browser/ui/browser.cc` constructs the tab model, initializes `BrowserWindowFeatures`, and only then calls `BrowserWindow::CreateBrowserWindow()`.
3. `BrowserWindow::CreateBrowserWindow()` in `chrome/browser/ui/views/frame/browser_window_factory.cc` already selects `WebUIBrowserWindow` for feature-enabled normal windows, otherwise `BrowserView`.
4. `BrowserWindowFeatures::InitPostWindowConstruction()` in `chrome/browser/ui/browser_window/internal/browser_window_features.cc` already has production code paths for both `BrowserView` and `WebUIBrowserWindow`, including focus, browser elements, find, fullscreen, and other controller roles.

This means Chromium has already paid much of the integration cost for a web-rendered normal-window shell. A new independent `BrowserWindow` subclass would also require a third set of feature-controller branches and would duplicate a large, changing interface. Hiding or rearranging `BrowserView` children would instead couple Limitless to Chromium's current toolbar and tab-strip layout.

The selected path must still be treated as conditional. `features::kWebium` and `surface_embed::features::kSurfaceEmbed` are disabled by default, the flag description calls Webium a prototype, and the implementation contains incomplete methods. These are validation risks, not reasons to copy the upstream implementation.

## Ownership and lifecycle

```text
CreateBrowserWindow(params)
  -> Browser::Create(params)
     -> Browser owns TabStripModel and BrowserWindowFeatures
     -> BrowserWindowFeatures owns browser controllers and service adapters
     -> Browser owns BrowserWindow through BrowserWindowDeleter
        -> WebUIBrowserWindow owns views::Widget
           -> Widget hosts the trusted Limitless shell WebContents

TabStripModel / TabInterface
  -> owns or references authoritative tab state
  -> each tab retains an independent page WebContents
     -> SurfaceEmbedConnector attaches only its visual and input surface
        to an <embed> element in the shell

Limitless shell
  -> observes typed browser APIs and Mojo Page callbacks
  -> sends typed intent messages to browser-process handlers
  -> never owns page navigation state or security identity
```

A shell renderer crash does not transfer ownership of tabs or page state: tab `WebContents` lifetimes remain controlled by Chromium. Whether the visible shell can restart and reattach without losing browser state is a Gate 2 experiment, not a source-backed guarantee. Closing the native widget follows Chromium's browser close and unload lifecycle, then tears down window-scoped features before `BrowserWindow` deletion.

## Limitless-to-Chromium interface boundary

| Concern | Authoritative owner | Limitless access |
| --- | --- | --- |
| Tab order, selection, groups, split state | `TabStripModel`, `TabInterface`, tab-strip service features | Existing typed tab-strip Mojo services and callbacks |
| Page content and lifecycle | Tab `content::WebContents` | `SurfaceEmbed` for visual/input attachment; browser-side commands for navigation |
| Back, forward, reload, stop | `NavigationController` and Chromium browser commands | Typed handler calls that re-check the target tab and command state |
| Address/search suggestions | Omnibox and searchbox services | Existing `RealboxHandler` or a narrower Limitless adapter, never a parallel suggestion engine |
| Command enablement and accelerators | `BrowserCommandController`, `BrowserActions`, platform accelerator tables | Query and execute through browser-process adapters; native pre-handle remains authoritative |
| Leader shortcut state | One product-owned controller scoped to the exact normal browser window | Renderer, root pre-target Views, and in-process AppKit adapters submit key events; the controller emits only typed allowlisted commands |
| Security identity | Chromium security state and page-info controllers | Read-only presentation data plus the existing page-info action |
| Profiles, history, bookmarks, sessions | Chromium keyed services and session helpers | Narrow service bindings or browser commands |
| Extensions | Chromium extension services and extension window controller | Existing extension container/bindings where viable; native popup surfaces initially |
| Agent presence | Current Herdr session snapshot | Product-owned off-UI provider, transient browser-process registry, and sanitized display-only SwiftUI projection. Presence grants no browser authority. |
| Prompts and dialogs | Existing Chromium controllers and modal dialog hosts | Retained native/WebUI surfaces, anchored through tracked browser elements when required |
| Native window state | `views::Widget`, native widget, fullscreen and active-state controllers | Narrow minimize, maximize, restore, close, fullscreen, and drag-region messages |

Rules for every new binding:

- Accept intent, not duplicated state mutations.
- Resolve handles in the browser process on every call.
- Reject stale, cross-profile, cross-window, or unauthorized handles.
- Re-check command enablement and security policy in Chromium.
- Keep the surface as small as the current user journey requires.
- Add browser tests for invalid messages and renderer compromise assumptions.

## Source evidence

The following pinned-source evidence is decisive:

- `chrome/browser/ui/views/frame/browser_window_factory.cc`, `BrowserWindow::CreateBrowserWindow`: existing factory selection between `WebUIBrowserWindow` and `BrowserView`, limited to normal windows for WebUI.
- `chrome/browser/ui/webui_browser/webui_browser_window.h`, `WebUIBrowserWindow`: explicitly described as a `BrowserWindow` whose primary UI is WebUI while retaining `views::Widget` for window management.
- `chrome/browser/ui/webui_browser/webui_browser_window.cc`, `WebUIBrowserWindow::WebUIBrowserWindow`: owns the widget and trusted shell `views::WebView`, registers native-window identity, loads `chrome://webui-browser/`, and warns against production GuestContents mode.
- `chrome/browser/ui/browser_window/internal/browser_window_features.cc`, `BrowserWindowFeatures::Init()` and `InitPostWindowConstruction()`: constructs browser actions and command controllers independently of the visible window and has explicit WebUI-window controller paths.
- `chrome/browser/ui/webui_browser/webui_browser_ui.cc`, `WebUIBrowserUI::BindInterface()`: binds tab-strip, drag, bookmarks, extensions, and searchbox services to the trusted shell.
- `chrome/browser/ui/webui_browser/webui_browser_page_handler.cc`, `GetGuestIdForTabId()`: resolves an opaque tab handle in the browser process. The pinned implementation originally treated UI-process-wide handle liveness as authorization; the Limitless scope patch also requires membership in the requesting browser's `TabStripModel` and reports invalid or foreign IDs as bad Mojo messages. A companion lifetime patch permanently revokes the issued `GuestHandler` command capability on tab detach, contents replacement, and browser close, and revalidates the originating browser before every operation. It does not revoke the SurfaceEmbed guest ID, whose lifetime remains owned by `GuestContentsHandle`.
- `components/surface_embed/README.md`: identifies SurfaceEmbed as the WebUI Browser content-hosting solution and states that it intentionally excludes scripting and other document communication channels.
- `components/surface_embed/guest_view_vs_surface_embed.md`: states that tab `WebContents` remain independent, keep tab helpers, and are controlled by browser-side APIs rather than page JavaScript.
- `components/surface_embed/browser/surface_embed_host.cc`, `SurfaceEmbedHost`: owns document-scoped attachment state and forwards focus and accessibility information through the connector.
- `chrome/browser/chrome_content_browser_client_receiver_bindings.cc`, `RegisterAssociatedInterfaceBindersForRenderFrameHost()`: exposes the browser-side `SurfaceEmbedHost` only when SurfaceEmbed is enabled and the frame is controlled by `WebUIBrowserUI`.
- `chrome/browser/chrome_browser_interface_binders_webui_parts_desktop.cc` and `chrome/renderer/chrome_content_renderer_client.cc`: constrain the related WebUI interfaces and renderer plugin enablement to the WebUI Browser context.
- `chrome/browser/ui/webui_browser/webui_browser_browsertest.cc`: covers startup/shutdown, tab navigation, DevTools targets, fullscreen, tab close, and surface composition paths.
- `chrome/browser/ui/webui_browser/webui_browser_interactive_uitest.cc`: exercises page focus and pointer lock with real tabs.
- `components/surface_embed/browser/surface_embed_plugin_browsertest.cc`: covers focus traversal, accessibility attachment, visibility, resize, crash, reattach, and rendering behavior.

## Alternatives considered

### Limitless-owned Chromium Views shell

This provides native performance, mature accessibility primitives, and direct access to Views-based prompts. It is not selected as the first architecture because `BrowserView` eagerly constructs the current toolbar, tab, content, side-panel, and prompt-host hierarchy. A clean Limitless composition inside the existing `BrowserView` would first need a generic construction and layout seam rather than visibility patches. A separate fresh `BrowserWindow` is not the fallback because it would also need to implement the broad legacy window contract and add a third feature-controller path.

Revisit this option if WebUI shell startup, accessibility, IME, or content attachment cannot meet the Gate 2 bar.

### Standalone WebUI browser window implemented by Limitless

This offers full visual control but repeats the integration already present in `WebUIBrowserWindow`. It would duplicate native widget creation, focus, modal-host, fullscreen, extension, find, and lifecycle code and would increase rebase work. It is rejected.

### WebUI frontend with GuestContents or iframe attachment

This is close to the older Webium prototype, but Chromium's own source blocks production use of the required unowned-inner-WebContents feature. It also creates a broader and more coupled frame relationship than the selected surface-only attachment. It is rejected for production.

### BrowserView with an overlaid or hidden custom shell

This could make a quick prototype but preserves the upstream layout as hidden state, duplicates focus and command surfaces, and creates fragile sizing and accessibility behavior. It is rejected.

### macOS SwiftUI/AppKit shell

The macOS-only v1 makes a native shell worth explicit consideration. Chromium's Apple toolchain includes `swift_source_set`, and `views::NativeViewHost` can host an `NSView` with macOS attachment and accessibility-parent plumbing. However, the pinned desktop browser has no checked-in SwiftUI or `NSHostingView` shell precedent. The checked-in Swift targets are iOS-oriented.

Mori proves prototype feasibility but uses a third `BrowserWindow`, direct native tab-view reparenting, global single-window state, parallel session/tab state, and many inert window-interface methods. Those choices conflict with this ADR's ownership and patch rules.

`docs/features.md` now explicitly requires native SwiftUI for the Mac shell, so the earlier deferral no longer governs product planning. Gate 2 must compare the current WebUI candidate with a bounded AppKit/SwiftUI host inside Chromium's existing `BrowserView` composition using the same navigation, tab, extension, prompt, focus, IME, accessibility, native-window, crash, performance, and pixel-quality journey. The experiment may use `NativeViewHost`, but it must not create a third full `BrowserWindow`, directly reparent tab native views, or duplicate Chromium's authoritative browser state. Broad shell work waits for this evidence-backed selection.

The bounded experiment now proves full Chrome linking, one hosted `NSView` in
the existing `NSWindow`, leading-layout reservation, Views focus and
accessibility-pane ordering, multi-window attach and detach lifecycle, popup
exclusion, and a zero-effect disabled path. Its leader slice adds a typed
per-window controller shared by renderer, Views, and synthetic in-process
AppKit routes. The product-owned compiler action is narrower and better
evidenced than the isolated three-file Chromium toolchain alternative. The
external hardware-input, IME, VoiceOver and native AX, exact-window PNG, and
pixel-review gates remain required before this candidate can be selected. See
`gate-2b-swiftui-boundary-evidence.md`.

### Separate shell process embedding Chromium content

This would cross the browser process boundary and require reconstructing secure browser APIs, lifecycle, prompts, drag, focus, accessibility, and session behavior. It is rejected because it risks rebuilding browser machinery.

## Candidate patch surfaces

The exact patch manifest is defined in `patch-surface.md`. The WebUI validation changes two upstream files:

1. `chrome/browser/ui/webui_browser/BUILD.gn` accepts an external Limitless root for the validation build and depends on the Limitless resource and test targets.
2. `chrome/browser/ui/webui_browser/webui_browser_ui.cc` conditionally serves the Limitless resource map and HTML entry point.

The validation launcher supplies `--enable-features=Webium,SurfaceEmbed`. Gate 2 does not patch the browser-window factory, install production feature defaults, add a new binder, or add production branding.

The bounded SwiftUI candidate adds a second experimental patch touching seven upstream files. It adds one shared external-root argument, one conditional dependency, and the smallest BrowserView construction and layout hooks needed to host the product-owned view. It does not modify the browser-window factory, Chromium's Apple toolchain, tab ownership, navigation, security state, or native page-view parenting. This patch remains experimental until the interactive Gate 2B run passes.

A third one-file patch closes an independent authorization gap in `GetGuestIdForTabId()`. It requires the resolved tab to belong to the requesting browser before guest creation. Browser tests prove that an inactive same-window tab remains valid, another same-profile window is rejected, both regular/incognito directions are rejected, a closed stale handle is rejected, and rejection creates no `GuestContentsHandle`. The check is browser membership, not a separate profile comparison.

A fourth patch overlaps the same page-handler file but has a separate capability-lifetime purpose. `WebUIBrowserGuestHandler` is permanently revoked when its tab moves, closes, replaces its `WebContents`, or its browser closes. Every operation rechecks the originating browser, profile, session, tab membership, and exact guest-bound contents. Tests prove that a stale post-move command cannot mutate the destination tab, close revocation is clean, and a new destination-window handler works. The normal SurfaceEmbed background-discard path remains red because Chromium rejects discard of the embedded performance-manager page node; this is Gate 2 candidate evidence, not a claimed pass.

A fifth patch overlaps the one upstream `browser_view.cc` file from the bounded
SwiftUI seam. It replaces the placeholder construction call with the
authoritative tab rail and hardens the required BrowserView lifecycle seams:
construction and teardown, tab-strip selection and mode switching, widget
initialization, accessible panes and window title, and per-tab accessibility
updates. Product-owned code observes exact tab handles, order, title, active
state, move, replacement, and lifecycle events. It routes activation and close
through window-scoped `TabListInterface` authorization. Browser tests prove
exact reorder projection and unchanged surviving `WebContents` identities.
The same checkpoint suppresses both upstream tab strips for native-rail
windows. This is behavior evidence, not visual approval or native input proof;
Chromium toolbar and other top surfaces remain.

The follow-on Agents slice adds no upstream patch. Its Herdr provider,
browser-process presence registry, SwiftUI rows, Objective-C++ projection, and
tests remain in `limitless/` and reuse the host seam already accounted for by
patches `0002` and `0005`. It displays every agent returned by the current Herdr
session across that session's workspaces, but it does not aggregate named
sessions or prove that an agent is using Limitless. Same-user presence is not
authentication or authorization. The registry exposes no tab, profile,
settings, page, command, revoke, kill, or audit capability, and private windows
do not subscribe.

The structural proof uses 40 real Chromium tabs and 12 agents. Transparent
native probes occupy the real bounds proposed by SwiftUI `GeometryReader`
instances for both scroll viewports and their content. Post-layout `NSView`
frames prove the regions have positive area, remain inside the rail, do not
overlap, preserve the Workspace minimum and Agents cap, and contain overflowing
content. The probes do not draw, accept hits, or enter accessibility, so this
is layout evidence rather than pixel, input, or external AX acceptance.

A sixth patch adds the bounded leader-shortcut routing seam. Product-owned code
keeps one typed controller, one two-second timer, one exact command map, and one
native monitor per eligible normal browser window. It accepts Control+B
followed by `n`, `p`, `1` through `9`, Shift+X, or `b`; Escape, timeout, unknown
input, window resignation, and teardown cancel the state. The CLI policy can
disable the feature and restore Chromium routing unchanged. Renderer and root
pre-target Views events share the same controller with the in-process AppKit
CommandDispatcher/local-monitor adapter. Enabled Leader precedes Chromium's
high-priority accelerator substrate, while popup and disabled windows keep the
stock delegate. Identical retained native events are deduplicated without
incrementing the arm generation or moving the timer deadline.

Patch `0006-browser-view-leader-shortcuts.patch` touches exactly two upstream
symbols: `BrowserView::PreHandleKeyboardEvent` in
`chrome/browser/ui/views/frame/browser_view.cc` and
`BrowserNativeWidgetMac::OnWindowInitialized` in
`chrome/browser/ui/views/frame/browser_native_widget_mac.mm`. The first file
already belongs to the rail patch, so the ordered six-patch series now touches
10 unique upstream files. The shortcut controller, SwiftUI status, typed
commands, native delegate subclass, and tests remain product-owned. The
`APPLICATION` browser tests synthesize `NSEvent` input through in-process
AppKit; they do not establish external hardware input, IME, VoiceOver or native
AX, or pixel acceptance.

Only after Gate 2 passes, the expected production foundation may expand to five upstream files: central external-root plumbing, the product startup dependency, early product feature defaults, and conditional resource selection. The WebUI production surface is six unique files while the two page-handler patches remain unupstreamed and share one file. A Limitless-specific typed binder may be proposed separately only if existing browser APIs cannot express an approved user journey. Production branding is also a separate decision. Either addition must update the quantified patch manifest and be reviewed against the patch-growth stop conditions.

No product-foundation patch should modify `Browser`, `NavigationController`,
network code, security-state computation, permission policy, sandboxing, site
isolation, or page process ownership. The later split milestone's separately
accounted editability-lifecycle patch modifies `TabStripModel` only to expose a
generic observer wakeup after real desktop Views drag state changes; its scope,
tests, and removal condition are recorded in the patch manifest.

The patch series must fail clearly if the window factory branch, WebUI controller type, resource target, or browser-interface registration changes upstream.

## Security consequences

The shell is trusted browser UI and therefore a high-value renderer. Its privileges must be smaller than its visual reach:

- SurfaceEmbed supplies pixels and input routing only. It must remain impossible for the shell to script page content or acquire a page DOM reference.
- Browser-process handlers validate every handle and enforce membership in the requesting browser, which also rejects handles from another profile.
- Issued `GuestHandler` command capabilities are tied to the originating browser and exact guest-bound contents, permanently revoked across tab movement or close, and revalidated before every operation. This does not claim that the separate SurfaceEmbed guest ID is revoked.
- Herdr presence remains display-only. The CLI process, current-session
  snapshot, pane identities, and focused field do not grant or imply browser
  authority.
- Security identity, permission state, certificate state, interstitials, and dangerous-download decisions stay authoritative in Chromium.
- The shell origin must have a restrictive CSP. Binder registration must be scoped to the exact trusted WebUI controller, not all WebUI pages.
- Page drag data, clipboard, file paths, custom schemes, extension actions, and command-palette execution need explicit validation and threat-model coverage.
- A compromised shell renderer must not be able to navigate arbitrary background tabs, suppress warnings, or invoke disabled commands merely by sending a message.

This architecture adds a privileged renderer and compositor attachment path. That cost is acceptable only if the thin slice passes a focused security review of binder scope, handle validation, CSP, origin checks, and SurfaceEmbed lifecycle.

## Accessibility consequences

The WebUI shell can expose semantic HTML roles, names, states, and actions, while Chromium's page `WebContents` retains its normal accessibility tree. SurfaceEmbed contains explicit parent accessibility forwarding and browser tests, but source evidence alone does not prove a coherent desktop screen-reader experience.

Gate 2 must verify:

- one ordered accessibility tree across shell and page boundaries;
- forward and reverse keyboard traversal without traps;
- focus restoration after tab changes, dialogs, menus, and shell renderer restart;
- meaningful roles, names, values, and actions for all shell controls;
- VoiceOver and the macOS Accessibility Inspector on every supported macOS version;
- high contrast, forced colors, reduced motion, text scaling, localization expansion, and RTL.

Failure to produce coherent focus and reading order is a rethink trigger, not deferred polish.

## Platform consequences

- macOS is the sole v1 support and release target. Chromium's dedicated WebUI Browser native widget supplies Cocoa integration for traffic lights, app menus, keyboard conventions, fullscreen, and native services. Immersive fullscreen remains a known Webium gap to test.
- Gate 2 is a rigorous macOS runtime gate. It does not require Linux or Windows builds or source-backed viability claims.
- Linux and Windows are deferred. Their Phase 0 source findings remain useful warnings for a future expansion decision, but failures or missing parity there do not block v1.
- Shared product code must still avoid unnecessary AppKit assumptions. Mac-specific behavior stays behind a narrow native capability interface or compile-selected implementation so future platform work can replace it cleanly.
- Apple silicon is the development and v1 distribution target. Intel is outside the v1 support matrix.

## Known unknowns and validation experiments

| Risk | Smallest useful experiment | Pass condition | Rethink signal |
| --- | --- | --- | --- |
| SurfaceEmbed maturity | Run the real shell with two tabs through open, switch, close, crash, reattach, and resize on macOS | No blank frames, stale input, lost state, or attachment leak | Requires GuestContents, page recreation, or Chromium model duplication |
| Focus and IME | Automate shell-to-page traversal, address entry, composed text, dead keys, and reverse traversal | Focus owner and text composition remain correct through navigation and tab switches | Focus traps, lost composition, or coordinate workarounds |
| Accessibility | Inspect the combined tree and perform core navigation with screen readers | Stable reading order, names, focus, and actions across the embed boundary | Split or unusable trees, missing focus restoration |
| Native frame parity | Exercise drag, resize, caption controls, system menu, fullscreen, minimum size, and mixed DPI | Native behavior matches platform conventions | Emulated controls or platform branches throughout shared code |
| Prompt anchoring | Trigger permission, page-info, password, download, extension, and auth surfaces | Correct ownership, anchor, modality, focus return, and trusted-origin communication | Prompts hidden, mis-anchored, or dependent on absent BrowserView children |
| Command authority | Race tab close/switch against back, reload, close-tab, and command-palette actions | Browser process rejects stale targets and obeys command enablement | Shell must cache or independently infer command state |
| Shell startup and crash | Measure first usable paint and kill the shell renderer | No long blank chrome and full recovery without losing tabs | Startup regression or window-level data loss |
| External GN integration | Generate, build, and run from a fresh checkout with only the pin and patch manifest | No manual source copies, hidden local excludes, or generated-file edits | Product code must be spread through Chromium directories |
| macOS release viability | Run on supported macOS versions, Apple silicon, multiple displays, Retina scaling, and both native and tab fullscreen | Browser-grade behavior without CPU-specific shell logic | Requires replacing native window behavior or leaves a supported Mac architecture unreliable |

## Thin vertical slice proposal

The Phase 1 seam proof should contain only enough product UI to validate the architecture:

1. Launch a Limitless-branded binary with a fresh temporary profile and construct one normal window through the WebUI window factory path.
2. Render a minimal Limitless shell containing a destination field, one tab item, back, forward, reload/stop, new-tab, close-tab, and native window drag regions.
3. Attach the real active tab `WebContents` with SurfaceEmbed and navigate it to a local HTTPS test page.
4. Derive title, URL, favicon, loading, active tab, and command availability from Chromium services.
5. Open and close a second real tab, switch both directions, and verify page state is retained.
6. Traverse shell controls and page content entirely by keyboard, including reverse traversal and address focus.
7. Exercise native move, resize, minimize, maximize or zoom, fullscreen, and close behavior.
8. Open DevTools for the page, trigger one permission prompt, and inspect security identity and accessibility state.
9. Close and relaunch to prove real session restore.
10. Kill and recover the shell renderer without losing tabs or navigation state.

Required automated coverage:

- browser tests proving the selected window class, real `WebContents` navigation, model-derived state, inactive same-window acceptance, stale-handle rejection, cross-window and cross-profile rejection, tab lifecycle, and session restore;
- an interactive UI test for focus, accelerators, tab traversal, window controls, fullscreen, and prompt focus restoration;
- a SurfaceEmbed render test at two sizes and scale factors;
- accessibility assertions for roles, names, focus, actions, and shell/page order;
- an outside-in smoke test launching the actual Limitless binary with a temporary profile;
- execution on the supported macOS versions and Apple silicon before Gate 2 approval.

## Migration and rebase strategy

- Keep frontend resources, typed interfaces, tests, scripts, and product documentation in `limitless/`.
- Keep upstream changes as an ordered patch series with one purpose per patch.
- Record touched symbols and GN labels, not only line numbers.
- Make patch application and source-pin checks fail before GN generation when the baseline drifts.
- Prefer existing browser API Mojo services. Add Limitless bindings by user journey, then upstream generic capabilities where appropriate.
- Keep Chromium's normal `BrowserView` path buildable as a diagnostic fallback during early phases, not as hidden runtime UI.
- On every Chromium update, run the vertical slice before resolving cosmetic conflicts and track patch count, files touched, and semantic conflict causes.
- Remove Limitless hooks when upstream WebUI Browser exposes a stable equivalent.

## Gate 1 approval questions

1. Approve the macOS-only v1 conditional hybrid architecture: native Chromium windowing, a Limitless-owned trusted WebUI shell, and SurfaceEmbed-hosted real tabs.
2. Approve retaining Chromium-owned prompts, dialogs, menus, DevTools, and secondary window types initially rather than restyling them before the core shell is proven.
3. Approve the rule that no broad product UI begins until the Gate 2 slice passes focus, accessibility, security, native-window, and real-browser checks on macOS.
4. Compare the WebUI candidate with a BrowserView-hosted AppKit/SwiftUI shell in Gate 2 because native SwiftUI is now an explicit product requirement. Select from equivalent evidence rather than waiting for the WebUI candidate to fail.

## Conditions that revisit this decision

Reopen this ADR if any of the following occurs:

- SurfaceEmbed cannot be approved for production use or cannot preserve tab behavior on supported Macs.
- The shell needs direct page scripting or duplicates authoritative browser state.
- Basic accessibility, IME, keyboard, prompt, or native-window behavior cannot be fixed behind narrow interfaces.
- Any browser-window factory patch is required, the resource integration expands beyond its two files, or the independent authorization fix expands beyond the one page-handler file.
- WebUI Browser gaps force Limitless to copy or continuously patch large Chromium implementations.
- A Chromium update repeatedly requires rediscovering ownership or editing product code across upstream UI directories.
- A supported Mac architecture or macOS version needs a materially different shell architecture.
