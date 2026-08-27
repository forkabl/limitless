# UI architecture options

## Decision summary

At Chromium revision `2b4613f0a24d234b139f1246028239a2f5dde58c`, the preferred Phase 1 direction is a conditional hybrid:

- retain Chromium's existing `WebUIBrowserWindow` as the top-level window implementation;
- replace its frontend resources with a Limitless-owned trusted WebUI;
- retain Chromium's `Browser`, `TabStripModel`, independent tab `WebContents`, command controllers, native `views::Widget`, and native or Views-owned prompts;
- embed tab surfaces with `SurfaceEmbed`, never the prototype GuestContents path in a production build;
- scope v1 to macOS and stop at Gate 2 unless focus, accessibility, IME, drag and drop, fullscreen, window controls, and prompts pass in the real macOS app.

This is not an assertion that Webium is production-ready. The pinned source calls it a prototype, keeps `kWebium` and `kSurfaceEmbed` disabled by default, contains many `NOTIMPLEMENTED_LOG_ONCE()` paths, and disables the SurfaceEmbed pixel test on Linux. The recommendation is to reuse the upstream dual-window substrate rather than create a third `BrowserWindow` implementation, then let a macOS Gate 2 decide whether it is viable. If Gate 2 fails, the fallback is a BrowserView-hosted native composition, with pure Views versus AppKit/SwiftUI decided by a bounded experiment. GuestContents and a third full `BrowserWindow` are not acceptable fallbacks. Linux and Windows are deferred beyond v1, but the comparison retains their evidence to avoid an unnecessarily irreversible design.

## Pinned-source facts that constrain the choice

1. `chrome/browser/ui/browser_window/internal/create_browser_window_non_android.cc`, `CreateBrowserWindow()`, calls `Browser::Create()`.
2. `chrome/browser/ui/browser.cc`, `Browser::Browser()`, creates the tab model and `BrowserWindowFeatures`, then obtains a `BrowserWindow` through `BrowserWindow::CreateBrowserWindow()`. It calls `BrowserWindowFeatures::InitPostWindowConstruction()` only after the window exists. `Browser` owns the returned window through `std::unique_ptr<BrowserWindow, BrowserWindowDeleter>`.
3. `chrome/browser/ui/views/frame/browser_window_factory.cc`, `BrowserWindow::CreateBrowserWindow()`, is already a two-way factory. For a normal window with `features::kWebium` enabled it constructs `WebUIBrowserWindow`; otherwise it constructs `BrowserView`, `BrowserWidget`, and the platform native widget.
4. `chrome/browser/ui/browser_window/internal/browser_window_features.cc`, `BrowserWindowFeatures::Init()` and `InitPostWindowConstruction()`, explicitly branch between `BrowserView` and `WebUIBrowserWindow`. The WebUI path already has distinct browser elements, focus, find bar, exclusive access, side panel, and zoom implementations. An unrelated third implementation falls into stub or absent feature paths.
5. `chrome/browser/ui/webui_browser/webui_browser_window.cc`, `WebUIBrowserWindow::WebUIBrowserWindow()`, owns a native `views::Widget`, hosts a `views::WebView`, loads `chrome://webui-browser`, registers native accelerators, and retains native modal-dialog and extensions containers.
6. `chrome/browser/ui/webui_browser/webui_browser_window_mac.mm` supplies a macOS native widget, command dispatcher, menu validation, shortcut execution, full-size content view, and transparent title bar. Non-macOS builds use the normal Views native widget chosen by `views::Widget`.
7. `chrome/browser/ui/webui_browser/webui_browser_client_view.cc`, `NonClientHitTest()` and `DraggableRegionsChanged()`, converts WebUI draggable regions into native `HTCAPTION` hit testing. It also reanchors permission UI when the WebUI location icon moves.
8. `chrome/browser/ui/webui_browser/webui_browser_page_handler.cc` routes navigation to the tab's `content::NavigationController`, opens page info through the existing Views bubble, creates the existing native app menu, opens the profile menu through `ProfileMenuCoordinator`, and opens DevTools through `DevToolsWindow`.
9. `components/surface_embed/README.md` identifies SurfaceEmbed as the intended mechanism for hosting tab contents inside WebUI Browser. `components/surface_embed/guest_view_vs_surface_embed.md` states that the tab remains an independent, browser-owned `WebContents`, retains tab helpers, and has no scripting or `postMessage` channel to the shell renderer.
10. `components/surface_embed/browser/surface_embed_host.cc` and `components/surface_embed/renderer/surface_embed_web_plugin.cc` implement focus handoff and accessibility-tree stitching. The host rejects invalid renderer AX node IDs. These mechanisms exist, but their product behavior still needs external accessibility testing.
11. `chrome/browser/chrome_content_browser_client_receiver_bindings.cc`, `RegisterAssociatedInterfaceBindersForRenderFrameHost()`, exposes `SurfaceEmbedHost` only when `kSurfaceEmbed` is enabled and only to a frame controlled by `WebUIBrowserUI`.
12. `chrome/browser/ui/webui_browser/webui_browser_window.cc` rejects the GuestContents proxy route in an official stable or beta build. Its check permits that route only in non-official, canary, or test builds. This makes GuestContents categorically unsuitable for Limitless production.

## Options compared

### Summary matrix

| Criterion | A. Limitless native Views inside `BrowserView` | B. WebUI chrome hosted inside `BrowserView` | C. Existing `WebUIBrowserWindow` plus Limitless WebUI and SurfaceEmbed | D. New Limitless `BrowserWindow` implementation |
| --- | --- | --- | --- | --- |
| Visual and interaction control | High, but `BrowserView` constructs current chrome eagerly | High for product chrome; native content and chrome remain separate layout trees | Very high for the main window because shell and embedded surfaces share one WebUI layout | Very high |
| Upstream patch and rebase burden | Medium to high because construction, layout, anchors, and focus are private `BrowserView` concerns | High because a new WebView host and bridge must be threaded through `BrowserView` | Low to medium because the dual implementation and handlers already exist | Small factory patch, but a large product adapter must track `BrowserWindow` |
| Browser-model and privileged-command access | Direct C++ access | Requires a new narrow Mojo bridge | Existing tab-strip APIs, toolbar APIs, `WebUIBrowserPageHandler`, and C++ controllers | Direct C++ access, but every presentation adapter must be created |
| Startup and runtime performance | Best of the four; no shell renderer | Adds one trusted shell renderer | Adds one trusted shell renderer and compositor embedding | Native version can match A; a web version matches C |
| Memory and process implications | Lowest incremental memory | One additional WebUI `WebContents` and renderer | One additional WebUI `WebContents` and renderer; tab `WebContents` stay independent | Depends on toolkit |
| Accessibility | Mature Views semantics if implemented correctly | Split native and DOM trees need stitching and focus proof | DOM shell plus explicit SurfaceEmbed AX stitching exists, but must be validated | Must be built and audited from zero |
| Keyboard, focus, drag and drop, IME | Chromium's mature native path, with product-specific focus work | Cross-boundary behavior is custom | Existing accelerator, focus, draggable-region, and SurfaceEmbed code exists; drag and drop and IME remain unknown | All behavior becomes Limitless's responsibility |
| Native title bar and controls | Preserves `BrowserWidget` and frame implementations | Preserves `BrowserWidget` and frame implementations | Native `views::Widget`; macOS has a dedicated implementation; parity is incomplete | Must reproduce or wrap platform behavior |
| Menus, prompts, popups, secondary windows | Existing Views surfaces are easiest to retain | Existing Views surfaces can remain | Existing app menu, page info, permissions anchor, modal host, profile menu, extensions container, find, and zoom paths are partly retained; many methods remain incomplete | Every surface needs an adapter or explicit fallback |
| Testability | Strong Views and browser test harnesses | Mixed WebUI and Views harnesses | Existing browser, interactive, Mojo, and pixel tests provide a base | New test harness and semantic identifiers required |
| macOS v1 fit and future portability | Strongest native starting point | Strong native frame, custom WebUI integration | Dedicated macOS native code makes it the strongest WebUI Browser platform; Windows and Linux remain future risks | Mac-specific implementation is direct, but the third-window contract has the highest rebase burden |
| Security-boundary complexity | Lowest | New trusted-renderer capability bridge | Existing restricted binder plus SurfaceEmbed's no-script design; all shell Mojo remains privileged | Native is lower risk; web is comparable to C |
| Incremental delivery | Good, but early work can couple to current `BrowserView` internals | Moderate | Best if Gate 2 succeeds because Chromium already exercises startup, navigation, fullscreen, DevTools, focus, and tab lifecycle | Poor until a large interface is implemented |

### A. Limitless-owned native Views shell inside `BrowserView`

This option keeps `BrowserView`, `BrowserWidget`, native frame implementations, direct `ContentsWebView` hosting, and all existing Chromium presentation adapters. Limitless would supply product Views and a dedicated layout strategy.

Advantages:

- native accessibility, IME, focus, keyboard, drag and drop, menus, and platform window behavior start from Chromium's mature path;
- no extra shell renderer or privileged browser-to-renderer API;
- active tab content remains hosted exactly as normal Chromium content;
- retained bubbles and dialogs already know how to find `BrowserView` anchors.

Costs and risks:

- `BrowserView::BrowserView()` in `chrome/browser/ui/views/frame/browser_view.cc` eagerly creates the toolbar, content region, infobar container, side panel, horizontal or vertical tab strip, shadows, scrims, and platform helpers;
- the relevant state is private and many feature controllers obtain concrete `BrowserView` children;
- a clean product shell would require a new generic composition and layout seam, not a pile of visibility changes;
- that seam would touch more upstream ownership boundaries than reusing WebUI Browser and would be sensitive to ongoing top-chrome refactors.

This is one native fallback candidate if the WebUI Browser substrate fails Gate 2. It is preferable to a new full window implementation because it preserves platform and security-sensitive Views behavior. For macOS v1 it must be compared with the AppKit/SwiftUI hosting variant in E.

### B. WebUI chrome hosted inside `BrowserView`

This option adds a trusted shell `views::WebView` to the existing `BrowserView`, but continues to host the active tab in native `ContentsWebView` children. It avoids SurfaceEmbed for tab content.

Advantages:

- product chrome can use HTML, CSS, TypeScript, and Chromium WebUI tooling;
- tab rendering stays on the normal direct path;
- the existing `BrowserWidget` and frame implementations remain authoritative.

Costs and risks:

- the window is split across two layout and accessibility technologies;
- shell chrome cannot naturally wrap or interleave multiple page surfaces without additional native layout coordination;
- a new privileged Mojo surface, focus bridge, drag bridge, semantic automation bridge, and anchor mapping would be required;
- `BrowserView` construction and layout still need upstream seams, so this combines much of A's patch cost with much of C's renderer cost.

This option has no decisive advantage over C at the pinned revision because Chromium has already built the dual-window feature path that C reuses.

### C. Existing `WebUIBrowserWindow` substrate with Limitless frontend

This option keeps the upstream C++ substrate and replaces the `chrome://webui-browser` resource bundle with Limitless-owned resources and only the additional Mojo methods that Limitless needs. Native Views continue to own the outer widget and initially retained security-sensitive or secondary surfaces. SurfaceEmbed displays browser-owned tab `WebContents` inside the shell without exposing a scripting channel.

Advantages:

- no third window type is introduced into `BrowserWindowFeatures`;
- `Browser`, tab ownership, session behavior, command enablement, and tab helpers remain authoritative;
- existing code handles window creation, close lifecycle, accelerators, page info, permission anchoring, app and profile menus, modal dialogs, extensions, find, zoom, DevTools, fullscreen, and native macOS command dispatch to varying degrees;
- the product frontend can control the full main-window composition, including vertical navigation and future split panes;
- SurfaceEmbed keeps tabs independently owned and reparentable, which is compatible with tab dragging, multiple windows, and split-pane composition in principle.

Risks that prevent unconditional approval:

- `features::kWebium` in `chrome/common/chrome_features.cc` and `surface_embed::features::kSurfaceEmbed` in `components/surface_embed/common/features.cc` are disabled by default;
- `chrome/browser/flag_descriptions.h` describes Webium as a prototype;
- `WebUIBrowserWindow` still has incomplete visibility, workspace, loading, toolbar security, downloads, translate, drag, profile, eye-dropper, caret browsing, incognito, and other methods;
- `WebUIStubLocationBar` is intentionally incomplete;
- most WebUI Browser functional tests use `kAttachUnownedInnerWebContents`, not SurfaceEmbed;
- `WebUIBrowserSurfaceEmbedPixelTest.SurfaceEmbedRendersRedRect` is disabled on Linux;
- SurfaceEmbed has explicit focus and AX plumbing, but no end-user screen-reader, IME, drag, mixed-DPI, or multi-window proof in this checkout;
- the trusted shell renderer and every Mojo method become privileged attack surface. Requests must be capability-minimal and must validate browser, profile, tab ownership, URL, and state on the browser side.

### D. A new `BrowserWindow` implementation selected by the factory

The existing factory is technically a narrow construction seam, so Limitless could add a third implementation. This looks attractive if patch size is considered alone, but it is not the narrowest behavioral seam.

`chrome/browser/ui/browser_window.h` currently has 72 pure virtual browser-specific methods in addition to `ui::BaseWindow`. More importantly, `BrowserWindowFeatures::InitPostWindowConstruction()` recognizes concrete `BrowserView` and `WebUIBrowserWindow` objects. A third implementation receives stub focus behavior and misses the feature-specific setup unless upstream feature construction is expanded.

Consequences:

- Limitless would own close lifecycle, native widget behavior, commands, focus, modal hosting, permissions, security state, downloads, translate, extensions, fullscreen, workspaces, and many secondary surfaces;
- compiler errors would reveal interface additions, but behavioral drift would still be easy to miss;
- sharing `BrowserView` implementation would require copying or refactoring it, both of which violate the minimal-patch goal;
- this option is only justified if both existing implementations fail and Chromium first exposes a substantially smaller public window-shell interface.

### E. A macOS SwiftUI/AppKit shell

The macOS-only v1 scope makes a native SwiftUI/AppKit shell a credible alternative to evaluate, but it does not make Mori's third-window implementation a durable seam.

Pinned-source evidence:

- `build/config/apple/swift_source_set.gni` and the Apple toolchain can compile Swift, but every checked-in `swift_source_set` consumer in this revision is under iOS or iOS-specific components. There is no checked-in desktop Chromium SwiftUI browser-shell precedent.
- The pinned desktop source has no `NSHostingView` integration.
- `views::NativeViewHost` can attach a native `NSView`, keep its bounds synchronized, and project an accessibility parent. `ui/views/controls/native/native_view_host_mac_interactive_uitest.mm` and `native_view_host_mac_unittest.mm` cover event dispatch, attachment, focus-related hosting, and accessibility parenting.
- Mori demonstrates that SwiftUI can drive Chromium-backed browser chrome, but its implementation creates a third `BrowserWindow`, reparents tab native views directly, uses global single-window state, and supplies many inert browser-window methods.

Possible variants:

1. A new SwiftUI-owned `BrowserWindow`, as in Mori, is rejected for the same reasons as D.
2. Replacing the trusted WebUI view inside `WebUIBrowserWindow` with SwiftUI would fork or broadly patch the existing WebUI-specific feature and handler path, losing the main reason to select that substrate.
3. Hosting an `NSHostingView` through `NativeViewHost` inside a deliberately simplified `BrowserView` composition is technically plausible. It still requires a generic BrowserView construction/layout seam and must prove focus, accessibility, menus, prompts, multiple windows, and authoritative Chromium state.

Option E does not displace C because Chromium has already built a WebUI-specific normal-window feature path, restricted binders, macOS native widget, and SurfaceEmbed lifecycle. If C fails Gate 2, compare a native Views composition and an AppKit/SwiftUI host inside `BrowserView` with the same thin journey before selecting the fallback. Do not use a third full `BrowserWindow` for either experiment.

## Recommended ownership boundary

```text
Browser process
  Browser and BrowserWindowFeatures
    TabStripModel and tab WebContents
    command, session, profile, permission, download, extension services
    WebUIBrowserWindow
      native views::Widget and platform window
      native retained prompts, menus, modal hosts, and secondary UI
      trusted Limitless WebUI WebContents
        Limitless layout, navigation UI, command palette, pane layout
        narrow Mojo requests and browser-to-shell state updates
        SurfaceEmbed elements
          independent browser-owned tab WebContents surfaces
```

The shell must never create parallel authoritative tab, navigation, command-enablement, permission, or security state. Frontend state is a projection of Chromium state. The browser process owns validation and mutation. Page content has no scripting path into the shell.

Initially the factory should continue to create Chromium Views windows for non-normal types such as popups, apps, app popups, picture-in-picture, and DevTools. This matches the pinned factory, which selects WebUI Browser only for `TYPE_NORMAL`.

## Expensive or irreversible commitments

- Building product state around GuestContents would couple tab lifetime to a deprecated embedding model and cannot ship in official stable or beta builds.
- Exposing broad script execution, arbitrary URL loading, or generic command invocation to the trusted shell would create a durable privileged API that is difficult to secure later.
- Forking `WebUIBrowserWindow` or copying `BrowserView` would turn upstream bug fixes into manual merges.
- Treating current Webium frontend APIs as stable public APIs would create rebase risk. Limitless should wrap them behind product-owned TypeScript and C++ adapters.
- Replacing native prompts before the substrate is complete would expand the security and accessibility scope prematurely.
- Depending on exact DOM or generated resource identifiers from Chromium's prototype frontend would make frontend replacement fragile.

## Gate 2 validation experiments

The recommendation is approved for a production shell only if a thin Limitless resource replacement passes all of these on real built binaries:

1. Launch and lifecycle: create, show, resize, minimize, maximize, restore, close, cancel close with `beforeunload`, relaunch, and restore a session.
2. Tab surface: navigate one real tab through SurfaceEmbed, switch tabs, close one tab, move a tab to another window, and verify the original `WebContents` and tab helpers remain authoritative.
3. Keyboard and focus: move focus shell to page to shell in both directions, exercise platform accelerators, test reserved and unreserved shortcuts, and verify no key reaches the wrong security context.
4. Text input: test dead keys, composed text, at least one CJK IME, RTL text, selection, clipboard, and accessibility text editing in the shell and page.
5. Accessibility: inspect the stitched AX tree with VoiceOver and Accessibility Inspector, then traverse shell controls into page content and back using only a screen reader and keyboard.
6. Native window behavior: drag from declared regions, exclude interactive controls from drag, resize from every edge, cross mixed-DPI monitors, and enter browser and content fullscreen.
7. Security UI: exercise page info, permissions, HTTP authentication, certificate errors, Safe Browsing interstitials, downloads, file chooser, and pending-download close confirmation. Anchors and trusted boundaries must be unambiguous.
8. Browser integrations: open DevTools, extension action popup, print preview, find, zoom, profile menu, app menu, and a popup window.
9. Rendering resilience: test GPU and software rendering, shell renderer crash and recovery, tab renderer crash, zoom, fractional scale, dark mode, high contrast, and forced colors.
10. Mac coverage: run the same journey on supported macOS versions, Apple silicon, Retina and mixed-display configurations, and native and tab fullscreen. Add a real x86_64 build and run only if Intel is included in v1. Linux and Windows require a new support decision and architecture revalidation after v1.

Failure of focus, AX stitching, stable SurfaceEmbed rendering, security surfaces, or supported Mac behavior is a rethink trigger, not a backlog item. The fallback experiment compares Views and AppKit/SwiftUI composition inside `BrowserView`, not GuestContents and not a third full `BrowserWindow`.

## Known unknowns

- Whether the Linux SurfaceEmbed pixel-test failure is environmental, compositor-specific, or architectural remains a deferred portability question, not a v1 blocker.
- Whether SurfaceEmbed correctly handles all IME, drag and drop, pointer capture, touch, accessibility, and mixed-DPI transitions.
- Whether macOS native fullscreen and traffic-light placement remain correct with a materially different shell height and draggable-region topology.
- The pinned WebUI Browser page handler did not verify requesting-browser ownership: a live handle from another window was accepted. Patch `0003-webui-browser-tab-handle-scope.patch` now requires membership in the requesting browser's `TabStripModel`; focused tests cover inactive same-window acceptance, same-profile cross-window rejection, both regular/incognito directions, stale-handle rejection, and the absence of a `GuestContentsHandle` side effect on rejection.
- The pinned guest handler kept its original window and `WebContents` after issuance. Patch `0004-webui-browser-guest-handler-lifetime.patch` ties the `GuestHandler` command capability to the originating browser and guest-bound contents, revokes it on detach, replacement, or browser close, and revalidates every operation. Tests prove clean close revocation and post-transfer stale-command rejection. This does not revoke the separate SurfaceEmbed guest ID. The real SurfaceEmbed background-discard path remains incompatible with Chromium's primary-page-node requirement and is a Gate 2 failure, not covered by a synthetic pass.
- Which `NOTIMPLEMENTED` methods are reached by ordinary browsing journeys and which are dormant for retained native surfaces.
- How quickly the upstream Webium and SurfaceEmbed APIs will change. The 2025 and 2026 copyright dates and prototype flags indicate active development rather than a stable embedding contract.
