# ADR-0001: Limitless desktop browser shell

## Status

Proposed for Gate 1 approval, amended for a macOS-only v1. This decision is conditional on the Phase 1 integration proof and must not be treated as approval for broad product UI work.

## Baseline and decision scope

The evidence in this record is from Chromium commit `2b4613f0a24d234b139f1246028239a2f5dde58c`. Limitless v1 supports macOS only. Linux and Windows research is retained as deferred portability evidence, not as a v1 implementation, CI, test, packaging, or release commitment. Android, ChromeOS, Linux, and Windows are outside the v1 product scope.

The observed local baseline is macOS arm64. Whether the distributed v1 is Apple-silicon-only or also ships an Intel build is a release-foundation decision that must be made before packaging work. The shell architecture must not depend on CPU-specific behavior.

This record decides the ownership and process boundary for the primary Limitless browser shell. It does not decide the final information architecture, visual layout, theme, tab presentation, or product feature order. Arc and Mori are design references, not implicit product requirements or architecture authorities; their candidate lessons are assessed separately in `design-reference-assessment.md`.

## Decision

Use a hybrid shell built on Chromium's existing `WebUIBrowserWindow` substrate:

- Chromium continues to own `Browser`, `TabStripModel`, `TabInterface`, `WebContents`, navigation, profiles, sessions, browser commands, extensions, security state, permissions, downloads, and other browser services.
- Chromium's native `views::Widget` and `WebUIBrowserNativeWidgetMac` continue to own the top-level macOS window, activation, resize, traffic-light integration, fullscreen, menus, and AppKit event routing.
- Limitless owns a trusted WebUI frontend for the visible normal-window shell, compiled from `limitless/` and exposed through narrow typed Mojo interfaces.
- Normal tab pages remain independent Chromium `WebContents`. They are visually and interactively attached to the shell with `SurfaceEmbed`, which does not expose a page scripting API to the shell.
- Existing Chromium native or WebUI surfaces are retained initially for security-sensitive prompts, dialogs, menus, file interactions, DevTools, and other completeness-critical UI.
- Only `BrowserWindowInterface::TYPE_NORMAL` uses the Limitless shell at first. App, popup, DevTools, picture-in-picture, profile-picker, and other secondary window types keep Chromium's existing implementations until separately classified and validated.

Do not create a third full `BrowserWindow` implementation during the initial integration. Do not copy `BrowserView` or `WebUIBrowserWindow`. Do not ship the current Chromium Webium frontend as the Limitless product UI. Reuse the native and browser-process substrate, and replace or extend only the frontend resources and narrowly required bindings.

Do not use the legacy GuestContents attachment mode in production. The Chromium source explicitly says that it is not approved for production in `WebUIBrowserWindow::WebUIBrowserWindow`. The selected content attachment is `SurfaceEmbed`, subject to the Gate 2 validation below.

## Why this is the narrowest durable seam

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
| Security identity | Chromium security state and page-info controllers | Read-only presentation data plus the existing page-info action |
| Profiles, history, bookmarks, sessions | Chromium keyed services and session helpers | Narrow service bindings or browser commands |
| Extensions | Chromium extension services and extension window controller | Existing extension container/bindings where viable; native popup surfaces initially |
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
- `chrome/browser/ui/webui_browser/webui_browser_page_handler.cc`, `GetGuestIdForTabId()`: resolves an opaque tab handle in the browser process and reports an invalid ID as a bad Mojo message.
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

A SwiftUI/AppKit shell is not selected over the existing WebUI Browser substrate. If the Gate 2 hybrid fails, a bounded fallback experiment may host SwiftUI through `NativeViewHost` inside a simplified `BrowserView` composition and compare it with a pure Views composition. Neither fallback may create a third full `BrowserWindow` or duplicate Chromium's authoritative browser state.

### Separate shell process embedding Chromium content

This would cross the browser process boundary and require reconstructing secure browser APIs, lifecycle, prompts, drag, focus, accessibility, and session behavior. It is rejected because it risks rebuilding browser machinery.

## Expected initial patch surface

The exact patch manifest is defined in `patch-surface.md`. Gate 2 validation is limited to two upstream files:

1. `chrome/browser/ui/webui_browser/BUILD.gn` accepts an external Limitless root for the validation build and depends on the Limitless resource and test targets.
2. `chrome/browser/ui/webui_browser/webui_browser_ui.cc` conditionally serves the Limitless resource map and HTML entry point.

The validation launcher supplies `--enable-features=Webium,SurfaceEmbed`. Gate 2 does not patch the browser-window factory, install production feature defaults, add a new binder, or add production branding.

Only after Gate 2 passes, the expected production foundation may expand to five upstream files: central external-root plumbing, the product startup dependency, early product feature defaults, and conditional resource selection. A Limitless-specific typed binder may be proposed separately only if existing browser APIs cannot express an approved user journey. Production branding is also a separate decision. Either addition must update the quantified patch manifest and be reviewed against the patch-growth stop conditions.

No initial patch should modify `Browser`, `TabStripModel`, `NavigationController`, network code, security-state computation, permission policy, sandboxing, site isolation, or page process ownership.

The patch series must fail clearly if the window factory branch, WebUI controller type, resource target, or browser-interface registration changes upstream.

## Security consequences

The shell is trusted browser UI and therefore a high-value renderer. Its privileges must be smaller than its visual reach:

- SurfaceEmbed supplies pixels and input routing only. It must remain impossible for the shell to script page content or acquire a page DOM reference.
- Browser-process handlers validate every handle and enforce the active profile/window relationship.
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
- Apple silicon is the proven development baseline. Intel distribution remains undecided and must be validated through a real x86_64 build and run if included in v1.

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
| macOS release viability | Run on supported macOS versions, Apple silicon, multiple displays, Retina scaling, and both native and tab fullscreen; run x86_64 only if Intel is included | Browser-grade behavior without CPU-specific shell logic | Requires replacing native window behavior or leaves a supported Mac architecture unreliable |

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

- a browser test proving the selected window class, real `WebContents` navigation, model-derived state, stale-handle rejection, tab lifecycle, and session restore;
- an interactive UI test for focus, accelerators, tab traversal, window controls, fullscreen, and prompt focus restoration;
- a SurfaceEmbed render test at two sizes and scale factors;
- accessibility assertions for roles, names, focus, actions, and shell/page order;
- an outside-in smoke test launching the actual Limitless binary with a temporary profile;
- execution on the supported macOS versions and Apple silicon before Gate 2 approval, plus an Intel build and execution only if Intel is included in the v1 support matrix.

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
4. Approve a rethink to a BrowserView-hosted native shell if SurfaceEmbed or trusted WebUI cannot meet those checks without broad upstream changes, with Views versus AppKit/SwiftUI decided by a bounded fallback experiment.

## Conditions that revisit this decision

Reopen this ADR if any of the following occurs:

- SurfaceEmbed cannot be approved for production use or cannot preserve tab behavior on supported Macs.
- The shell needs direct page scripting or duplicates authoritative browser state.
- Basic accessibility, IME, keyboard, prompt, or native-window behavior cannot be fixed behind narrow interfaces.
- Any browser-window factory patch is required, or the thin slice exceeds the two-file resource integration described above.
- WebUI Browser gaps force Limitless to copy or continuously patch large Chromium implementations.
- A Chromium update repeatedly requires rediscovering ownership or editing product code across upstream UI directories.
- A supported Mac architecture or macOS version needs a materially different shell architecture.
