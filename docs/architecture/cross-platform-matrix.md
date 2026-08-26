# macOS v1 and deferred desktop platform matrix

## Scope and baseline

This matrix records macOS v1 requirements and preserves deferred Linux and Windows research at Chromium revision:

```text
2b4613f0a24d234b139f1246028239a2f5dde58c
```

Limitless v1 supports macOS only. Linux and Windows columns are planning evidence for a possible later expansion, not v1 build, test, CI, packaging, or release requirements. The product layout can be shared and opinionated, but native window semantics, input conventions, accessibility bridges, and system-owned dialogs cannot be flattened into one identical implementation.

The selected architecture uses Chromium's existing `WebUIBrowserWindow`, a Limitless-owned trusted frontend, and SurfaceEmbed-hosted real tab `WebContents`, not the standard `BrowserView` and `BrowserWidget` frame hierarchy. Standard Views sources below are the required parity baseline and the frame foundation for a fallback BrowserView-hosted native composition. On macOS, that fallback may use pure Views or an AppKit/SwiftUI host after a bounded comparison. These sources are not behavior the WebUI path inherits automatically.

## Window and frame behavior

| Concern | macOS | Linux | Windows | Limitless contract |
| --- | --- | --- | --- | --- |
| Standard Views frame baseline | `BrowserNativeWidgetMac::PopulateCreateWindowParams()` in `chrome/browser/ui/views/frame/browser_native_widget_mac.mm` creates a titled, closable, miniaturizable, resizable `NSWindow` with full-size content and a transparent titlebar. `UseCustomFrame()` is false for normal windows. | `BrowserNativeWidgetAuraLinux::UseCustomFrame()` in `chrome/browser/ui/views/frame/browser_native_widget_aura_linux.cc` requires client-side decoration when server decorations are unavailable; otherwise `prefs::kUseCustomChromeFrame` chooses. | `BrowserDesktopWindowTreeHostWin::ShouldUseNativeFrame()` and `GetFrameMode()` in `chrome/browser/ui/views/frame/browser_desktop_window_tree_host_win.cc` choose DWM/system or custom behavior based on window type, desktop support, and theme. | Treat these classes as the behavior baseline. A macOS fallback stays inside `BrowserView` and compares pure Views with an AppKit/SwiftUI host. |
| Selected WebUI outer window | `WebUIBrowserWindow::CreateNativeWidget()` in `chrome/browser/ui/webui_browser/webui_browser_window_mac.mm` creates `WebUIBrowserNativeWidgetMac`, with full-size content, hidden native title, command dispatch, and a fixed titlebar-height override. | Non-macOS `WebUIBrowserWindow::CreateNativeWidget()` in `chrome/browser/ui/webui_browser/webui_browser_window.cc` returns `nullptr`, so the Widget uses its default native implementation. | Same non-macOS default Widget path. | Do not assume standard `BrowserNativeWidget`, desktop-host, or frame-view behavior. Gate 2 proves macOS parity. Linux and Windows parity is deferred. |
| Caption controls | Native AppKit traffic lights on the leading side. `BrowserFrameViewMac` queries native control bounds and publishes layout exclusions. | System decorations belong to the compositor/window manager. Custom decorations use Chromium Linux frame views and the Linux navigation-button provider when available. | Chromium's `BrowserCaptionButtonContainer` and DWM/system frame rules place controls on the trailing side and expose HT-based non-client behavior. | Consume platform-provided caption exclusion rectangles. Never hardcode positions or assume controls are on the right. |
| Titlebar content | `NSWindowStyleMaskFullSizeContentView` allows content under the titlebar. `BrowserFrameViewMac::GetTopInset()` returns zero and coordinates top UI with native controls. | Depends on server-side versus client-side decorations, desktop environment theme, and Ozone backend. | Client and non-client areas change with DWM frame mode, maximized state, and window-controls overlay. | Layout against frame-provided insets and exclusions on every state change. |
| Dragging | AppKit handles native titlebar movement. Chrome also respects renderer draggable regions and top-chrome hit tests. | Standard Views uses Aura/Ozone and `BrowserDesktopWindowTreeHostLinux`. | Standard Views maps HWND non-client results through `BrowserFrameViewWin` and the desktop host. | The selected path uses `WebUIBrowserClientView::DraggableRegionsChanged()` and `NonClientHitTest()` in `chrome/browser/ui/webui_browser/webui_browser_client_view.cc`. Test subtraction of interactive controls, caption areas, pointer variants, and scale changes on each platform. |
| Resizing and minimum size | Native NSWindow resizing plus Views minimum-size calculation. | Window-manager resize semantics vary between X11 and Wayland. Client-side decoration regions must stay synchronized with scale and state. | HWND sizing, DWM insets, snap, maximize, and per-monitor DPI influence bounds. | Use Widget/frame constraints in device-independent pixels. Verify edge and corner resizing, maximize, restore, snap where applicable, and mixed-DPI movement. |
| System menu | Application commands primarily live in the macOS global menu and command dispatcher. The selected macOS WebUI native widget explicitly validates and executes Chromium commands. | Standard `BrowserDesktopWindowTreeHostLinux` may create `DbusAppmenu`; the selected generic Widget path does not show equivalent browser-specific setup. | Standard `BrowserDesktopWindowTreeHostWin` creates and updates the native system menu and handles `WM_INITMENUPOPUP` and `WM_SYSCOMMAND`; the selected generic Widget path does not show equivalent browser-specific setup. | Preserve command enablement and native macOS menu contracts for v1. Linux global-menu and Windows system-menu parity are deferred risks. |
| Fullscreen | Native fullscreen is an asynchronous AppKit transition. Standard `BrowserNativeWidgetMac`, `FullscreenToolbarController`, and `BrowserFrameViewMac` coordinate native menubar and top UI. | Standard `BrowserDesktopWindowTreeHostLinux` reports asynchronous WM state changes to `BrowserView`. | Standard Widget and exclusive-access state coordinate with HWND/DWM fullscreen and work-area changes. | The selected path uses `WebUIBrowserExclusiveAccessContext` and `WebUIBrowserWindow::ProcessFullscreen()`. Its source says immersive mode and platform toolbar preferences are not yet supported, so all native and tab fullscreen transitions are Gate 2 checks. |
| Picture-in-picture | Uses a custom frame path rather than the normal AppKit browser frame. | Uses a custom client frame. | Has specialized frame-view selection and window properties. | Do not send picture-in-picture through the initial Limitless normal-window shell. Retain Chromium implementation until separately designed. |
| Workspaces and virtual desktops | Native spaces and visibility state flow through the Widget/native window. | `initial_workspace`, activation tokens, and Ozone window properties are carried in `BrowserWindowCreateParams`; support depends on desktop environment. | `BrowserDesktopWindowTreeHostWin` integrates virtual desktop and workspace state. | Preserve create params, restored placement, and platform host calls. Do not mirror workspace state in product-only storage. |

Frame factories:

- macOS: `chrome/browser/ui/views/frame/browser_frame_view_factory_mac.mm`, `chrome::CreateBrowserFrameView()` returns `BrowserFrameViewMac` for normal windows.
- Linux and Windows: `chrome/browser/ui/views/frame/browser_frame_view_factory_views.cc`, `chrome::CreateBrowserFrameView()` dispatches to platform-specific creation.
- Shared native abstraction: `chrome/browser/ui/views/frame/browser_native_widget.h`, `BrowserNativeWidget`.

The WebUI path instead creates `WebUIBrowserClientView` through `WebUIBrowserWindow::WidgetDelegate::CreateClientView()`. On macOS it has the dedicated native widget above. On Linux and Windows it relies on default `views::Widget` native selection. Browser-specific system menus, workspace integration, decorations, shadows, DWM handling, and async Linux state behavior must be verified individually rather than inferred from the standard factories.

## Commands, keyboard, focus, and text input

| Concern | macOS | Linux | Windows | Limitless contract |
| --- | --- | --- | --- | --- |
| Shortcut conventions | Command-key equivalents and global menu items are authoritative. `BrowserNativeWidgetMac::PreHandleKeyboardEvent()`, `HandleKeyboardEvent()`, and command-dispatcher hooks preserve native menu behavior. | Chromium accelerator tables use Control and Alt conventions, including Linux-specific entries. Desktop environments may reserve combinations. | Chromium accelerator tables use Control and Windows conventions; native messages and system commands also participate. | Load Chromium command mappings from `chrome/browser/ui/accelerator_table.cc` and execute command IDs through Chromium. Product controls display platform-resolved labels. |
| Accelerator routing | Key equivalents are coordinated with the native main menu and `BrowserNativeWidgetMac` command validation. | `BrowserView::LoadAccelerators()`, `PreHandleKeyboardEvent()`, and Aura input routing send events to shell commands or WebContents. | Same Views routing plus HWND/native handling for system keys and menus. | Keep one command authority and one accelerator resolution path. Shell shortcuts must not drift from command enablement or steal page/IME input. |
| Focus | AppKit focus, child Widgets, sheets, and WebContents focus must agree. | Aura focus manager, Ozone, and child Widgets must agree. | HWND activation, child dialogs, Views focus, and WebContents focus must agree. | Use Views focus managers and semantic focus order. Preserve focus before modal UI, restore it after close, and test switching between shell, page, DevTools, and child dialogs. |
| Focus indication | System and Views colors/metrics vary with active state and accessibility settings. | Theme toolkit and Views may provide different focus visuals. | Forced colors and system metrics can override normal visuals. | Provide visible keyboard focus in every theme. Do not remove native focus indication without an equivalent that survives high contrast. |
| IME implementation | Native Views uses `ui/base/ime/mac/input_method_mac.mm`, `InputMethodMac`; the selected shell uses renderer text input inside the native Widget. | Native Views uses `ui/base/ime/linux/input_method_auralinux.cc`, `InputMethodAuraLinux`, or the Ozone input method; the selected shell adds a renderer boundary. | Native Views uses `ui/base/ime/win/input_method_win_tsf.cc`, `InputMethodWinTSF`; the selected shell adds a renderer boundary. | Prove DOM address input, focus handoff to SurfaceEmbed content, and return focus under the platform input method. Never implement address input from raw key events. |
| Composition and candidate UI | Native marked text and candidate windows depend on correct renderer and native bounds. | IBus/Fcitx and Wayland/X11 candidate positioning depend on the active renderer text-input client. | TSF composition, reconversion, and candidate windows depend on native caret geometry. | Test preedit, commit, cancel, candidate selection, emoji, dead keys, and caret movement in shell and page. `OmniboxViewViews` in `chrome/browser/ui/views/omnibox/omnibox_view_views.cc` is the native behavior baseline, not an implementation the WebUI shell inherits. |
| Drag and drop | Native pasteboard plus Views/WebContents routing. | Ozone/X11/Wayland data exchange varies. | OLE/native data exchange plus Views routing. | Reuse Chromium drop-data types and security filtering. `BrowserRootView` contains top-chrome/tab drag behavior and filters JavaScript URLs, which a replacement must preserve or deliberately re-express. |

## Scaling, appearance, animation, and localization

| Concern | macOS | Linux | Windows | Limitless contract |
| --- | --- | --- | --- | --- |
| Device scale factor | `ui/display/mac/screen_mac.mm` reads `NSScreen.backingScaleFactor`; Cocoa host code reports scale changes. | `BrowserDesktopWindowTreeHostLinux::OnDeviceScaleFactorChanged()` refreshes frame hints; actual scaling is provided through Ozone and may be fractional. | `ui/views/widget/desktop_aura/desktop_window_tree_host_win.cc`, `HandleWindowScaleFactorChanged()`, and `ui/display/win/screen_win.cc` support per-monitor DPI. | Lay out in DIPs, use Chromium image/resource scaling, and handle runtime scale changes. Validate mixed-scale multi-monitor movement and fractional values. |
| Light and dark mode | Native colors and appearance are bridged by `ui/native_theme/native_theme_mac.mm` and mac color mixers. | `ui::LinuxUiTheme` can follow toolkit/system themes; Chromium theme and system-theme paths can select different frame implementations. | `ui/native_theme/native_theme_win.cc` and Windows settings providers translate system appearance. | Derive color providers from the window/profile theme source. No cached product palette may ignore a theme change callback. |
| High contrast and forced colors | macOS accessibility high-contrast appearance contributes to color mixing in `ui/color/mac/native_color_mixers_mac.mm`. | Availability and exact palette vary by desktop environment and portal/toolkit. Aura native-theme state is the shared bridge. | `ui/native_theme/os_settings_provider_win.cc` detects forced-colors/high-contrast settings; color-provider keys propagate forced-color mode. | Use semantic color IDs and test system high contrast. Web-rendered shell content must expose forced-colors behavior and meet the same state as native child Widgets. |
| Reduced motion | `ui/gfx/animation/animation_mac.mm` feeds `gfx::Animation::PrefersReducedMotion()`. | `ui/gfx/animation/animation_linux.cc` feeds the same shared query. | `ui/gfx/animation/animation_win.cc` feeds the same shared query. | All nonessential shell animation checks `gfx::Animation::PrefersReducedMotion()` or the equivalent propagated value. Also test interruption and final state. |
| RTL | AppKit frame code has explicit titlebar-direction and leading-edge handling. | Views mirroring and Linux frame implementations respond to locale direction. | Views mirroring applies while caption controls remain platform-positioned. | Mirror product layout semantically, not by hardcoded coordinate transforms. Caption exclusions stay platform-owned. Test mixed-direction URL/title strings. |
| Localization expansion | Native menu and dialog widths follow platform toolkits; shell labels use Chromium localization. | Fonts and toolkit metrics vary across desktop environments. | DPI and system font metrics affect layout. | Use `l10n_util`, flexible layouts, elision with accessible full text, and pseudolocale tests. Avoid fixed widths for translated commands and prompts. |

## Accessibility matrix

| Layer | macOS | Linux | Windows | Limitless requirement |
| --- | --- | --- | --- | --- |
| Views bridge | `ui/views/accessibility/view_ax_platform_node_delegate.cc` maps View semantics to platform nodes. | Same shared delegate. | Same shared delegate. | A native Views shell should set roles, names, states, actions, relationships, and focus on every interactive View. |
| Platform API | `ui/accessibility/platform/ax_platform_node_cocoa.mm` bridges to NSAccessibility. | `ui/accessibility/platform/ax_platform_node_auralinux.*` bridges to ATK/AT-SPI. | `ui/accessibility/platform/ax_platform_node_win.*` bridges to UI Automation and legacy accessibility where required. | Inspect with VoiceOver, Orca, and Narrator or equivalent platform inspectors. Compilation is not accessibility proof. |
| Page contents | Browser accessibility exposes the renderer accessibility tree through the native host. | Same Chromium browser accessibility pipeline. | Same Chromium browser accessibility pipeline. | Preserve the WebContents tree and create a coherent boundary between shell and page. Focus movement must not create duplicate, hidden, or unreachable roots. |
| Selected SurfaceEmbed boundary | `SurfaceEmbedHost` forwards the parent AX node and `SurfaceEmbedWebPlugin` exposes the shell container node. | Same shared SurfaceEmbed code, but the Linux render test is disabled and AT-SPI traversal is unproven. | Same shared SurfaceEmbed code; UIA traversal is unproven. | Inspect the stitched DOM-shell to tab-WebContents tree with platform screen readers. Source plumbing is necessary but not acceptance proof. |
| Current BrowserView semantics | `BrowserView::AddedToWidget()` sets the client role and registers accessible panes/focus traversal. | Same common Views behavior. | Same common Views behavior. | Recreate the semantic intent in the Limitless shell. Do not copy presentation hierarchy merely to get accessibility. |
| Native announcements | `BrowserNativeWidgetMac::NotifyAccessibilityEvent()` can post AppKit announcements. | AT-SPI events flow from platform nodes. | UIA events flow from platform nodes. | Use Chromium accessibility events and live-region patterns. Do not announce visual state through a separate inaccessible channel. |

SurfaceEmbed evidence is in `components/surface_embed/browser/surface_embed_host.cc`, especially `SetParentAccessibilityInfo()` and `ForwardParentAccessibilityInfo()`, and `components/surface_embed/renderer/surface_embed_web_plugin.cc`, especially `SendAccessibilityInfo()`. The current code contains an accessibility TODO around obtaining the container AX node ID, so platform screen-reader validation is mandatory.

Minimum accessibility acceptance on every supported platform, which means macOS for v1:

- meaningful window, navigation, content, and dialog landmarks;
- names, roles, values, states, and actions for every interactive control;
- deterministic reading and keyboard focus order;
- full keyboard operation without focus traps;
- focus restoration after menus, bubbles, modals, and tab changes;
- sufficient focus and state contrast in normal and high-contrast modes;
- correct zoom and text scaling without clipping;
- reduced-motion behavior;
- correct boundary and focus transfer between shell and page WebContents.

## Multiple windows, profiles, and lifecycle

The ownership model is shared across all three platforms:

- `BrowserManagerService` in `chrome/browser/ui/browser_manager_service.*` owns each profile's `Browser` objects.
- `GlobalBrowserCollection` provides cross-profile iteration and activation ordering.
- `BrowserWindowInterface` documents that all tabs in a desktop browser window share the window profile.
- `BrowserWindowCreateParams` carries bounds, show state, workspace, session-restore, and platform activation data.

Platform differences:

| Scenario | macOS | Linux | Windows | Required validation |
| --- | --- | --- | --- | --- |
| Last window closed | The application can remain alive through AppController and the global menu. | Usually exits when no keepalive remains, subject to background services and desktop launch behavior. | Usually exits when no keepalive remains, subject to background services. | Close last window, reopen, quit explicitly, and verify session state. Do not infer application lifetime from window visibility. |
| Window activation | AppKit may report activation after returning to the run loop; current implementations compensate when showing. | WM activation is asynchronous and can be denied or token-gated. | HWND activation is generally immediate but foreground activation policy applies. | Test new window, background window, focus stealing prevention, and profile switching. |
| Multiple profiles | Separate windows share one application menu but have profile-specific state. | WM class/role/app ID and profile resources affect grouping. | App user-model, icon, virtual desktop, and profile properties can affect grouping. | Open two normal profiles and one off-the-record window. Verify command target, theme, session, notifications, and close ownership. |

## System-owned and security-sensitive surfaces

| Surface | macOS | Linux | Windows | Initial Limitless policy |
| --- | --- | --- | --- | --- |
| File chooser | `ui/shell_dialogs/select_file_dialog_mac.mm` uses native macOS dialog plumbing. | `select_file_dialog_linux_portal.cc` supports the desktop portal, with Linux toolkit fallback in `select_file_dialog_linux.cc`. | `select_file_dialog_win.cc` uses Windows native dialogs. | Retain Chromium's `SelectFileDialog::Create()` route and parent to the active native window. Test open, save, folder, multiple selection, cancellation, and modality. |
| Notifications | `chrome/browser/notifications/mac/notification_platform_bridge_mac.cc` | `chrome/browser/notifications/notification_platform_bridge_linux.cc` | `chrome/browser/notifications/notification_platform_bridge_win.cc` | Retain the platform bridge, profile attribution, permission checks, activation, and deep-link routing. Limitless may restyle only in-browser notification settings. |
| HTTP authentication | Common `chrome/browser/ui/login/http_auth_coordinator.*` and `login_handler.*`; desktop Views presentation uses `chrome/browser/ui/views/login_handler_views.cc` and a WebContents modal host. | Same common/controller path with Linux native parenting. | Same common/controller path with Windows native parenting. | Retain the coordinator and supply a correct modal-dialog host. Test cancel, wrong credentials, navigation, tab close, and window close. |
| Certificate viewer | `chrome/browser/ui/views/certificate_viewer_mac_views.mm` can invoke OS viewer plumbing. | Generic constrained WebUI certificate viewer is available under `chrome/browser/ui/webui/certificate_viewer/`. | `chrome/browser/ui/views/certificate_viewer_win.cc` uses Windows certificate UI. | Retain native or Chromium viewer selected by platform. Never replace validation or trust decisions with product-only state. |
| Permissions | Some OS capabilities have additional macOS authorization. Browser prompts still rely on Chromium permission and content-setting controllers. | Browser prompts plus desktop portals or OS capability policy where relevant. | Browser prompts plus Windows OS capability policy where relevant. | Keep `PermissionRequestManager` authoritative and preserve OS prompts. Provide trusted shell anchors without suppressing origin, scope, or persistence information. |
| Print and save | Native print/save plumbing and Chromium print preview may open separate dialogs or WebContents. | Backend and portal/toolkit behavior varies. | Native print/save integration and Chromium preview. | Retain initial Chromium flows and secondary windows. Parent them correctly and keep profile/window lifecycle intact. |
| Password, autofill, translate, identity, payment | Many surfaces are Views bubbles anchored to current toolbar or contents geometry. | Same controllers with platform parent differences. | Same controllers with platform parent differences. | Retain controllers and adapt presentation only after explicit anchor, focus, accessibility, and security-review contracts exist. |
| DevTools and view source | May create docked contents or separate browser-window types. | Same model with platform host differences. | Same model with platform host differences. | Preserve the standard secondary-window implementation initially. Prove docked DevTools can attach to a Limitless contents container. |

## GPU and software fallback

Chromium's shared compositor and GPU process are the platform contract:

- `content/browser/gpu/gpu_data_manager_impl.*`, `GpuDataManagerImpl`, owns GPU feature status and fallback transitions.
- `content/browser/gpu/gpu_data_manager_impl_private.h` records fallback modes.
- `content/browser/gpu/compositor_util.cc` reports compositing capabilities and problems.
- `gpu/config/software_rendering_list.json` describes known driver/device workarounds.
- `ui/compositor/layer.h` and the Views compositor support software compositing paths.

Limitless must remain inside Chromium's Views/WebContents compositor rather than require a second native rendering stack. The shell must function when GPU compositing is disabled, animations are reduced, transparency is unavailable, or effects such as blur cannot be rendered.

Required checks on each platform:

- normal GPU path and `--disable-gpu` software path;
- resize, scroll, menus, overlays, modal dimming, and video in both paths;
- no information encoded only in blur, transparency, or animation;
- correct device-scale and color behavior after GPU restart or fallback;
- useful diagnostics without weakening blocklist or sandbox behavior.

## Platform implementation rule

Keep platform calls in Chromium's browser process and project only capability-minimal state into the trusted frontend:

```text
Limitless trusted WebUI frontend
  -> WebUIBrowserWindow and existing page-handler adapters
     -> macOS: WebUIBrowserNativeWidgetMac
     -> future Linux/Windows: default views::Widget native path, subject to revalidation
     -> Chromium commands, models, prompts, and system dialogs
  -> SurfaceEmbed
     -> independent browser-owned tab WebContents
```

Add narrow browser-side adapters only for demonstrated macOS gaps. Shared product components receive validated state and geometry, while Chromium owns native calls, commands, tab mutations, and system dialogs. Do not spread AppKit assumptions through the TypeScript frontend. If a later Linux or Windows effort requires broad copies of browser-specific native hosts, stop and re-evaluate the architecture before adding that platform.

## Validation matrix

The macOS column is required for v1. Linux and Windows scenarios are retained as a post-v1 checklist and do not block Gate 2:

| Journey | macOS v1 | Linux after v1 | Windows after v1 |
| --- | --- | --- | --- |
| Launch, restore, second window, second profile, last-window close | Required | Deferred: Wayland and X11 where supported | Deferred |
| SurfaceEmbed render, switch, resize, crash, detach, and cross-window move | Required | Deferred: Wayland and X11; replace the disabled upstream pixel test | Deferred |
| Move, all-edge resize, maximize/restore, minimize, native close | Required | Deferred: server and client decorations | Deferred: include snap |
| Browser fullscreen, tab fullscreen, exit by keyboard and UI | Native fullscreen transition | WM fullscreen transition | DWM/Widget fullscreen transition |
| Caption controls, drag regions, interactive controls near frame | Traffic lights and RTL title direction | Desktop-environment controls and both frame modes | DWM/custom controls and window-controls overlay |
| Keyboard-only navigation and command menu | Command-key/global menu | Control/Alt and desktop-reserved keys | Control/system-menu keys |
| IME and composed address input | macOS input sources, dead keys, emoji | IBus/Fcitx on Wayland and X11 | TSF IMEs, dead keys, emoji |
| Accessibility | VoiceOver and Accessibility Inspector | Orca and Accerciser or AT-SPI inspector | Narrator and Accessibility Insights |
| Appearance | light, dark, increased contrast, reduced motion | system/Chromium theme, high contrast, reduced motion | light, dark, forced colors, reduced motion |
| Scaling | Retina/non-Retina transition if hardware permits | 100%, 125%, 150%, mixed monitors | per-monitor 100%, 125%, 150%, mixed monitors |
| System surfaces | file chooser, notification, auth, certificate, permission | portal and toolkit variants where available | native dialog and notification variants |
| Rendering fallback | GPU and software | GPU and software on Wayland/X11 | GPU and software, including GPU restart |

Success means behavioral and visual inspection, not only a passing build. Capture screenshots for stable Limitless surfaces, but inspect native behavior interactively where screenshots cannot prove focus, modality, resize hit targets, or accessibility actions.

## Known unknowns

- Linux behavior must be validated separately on Wayland and X11. The source deliberately routes through Ozone and window-manager capabilities, so one Linux session is not representative.
- `WebUIBrowserWindow` uses default native Widgets on Linux and Windows rather than the browser-specific native hosts. System-menu, decoration, workspace, DWM, activation-token, and async state parity is unknown until E2E validation.
- `WebUIBrowserNativeWidgetMac` hardcodes a 40 DIP titlebar-height override and is only loosely based on `BrowserNativeWidgetMac`; traffic-light layout, native fullscreen, and display-change behavior need visual and behavioral proof.
- A WebUI shell's cross-process focus, accessibility-tree boundary, asynchronous layout, drag regions, and IME behavior remain higher risk than an all-Views shell. `WebUIBrowserWindow` demonstrates feasibility but not completeness.
- SurfaceEmbed's Linux render test is disabled in `chrome/browser/ui/webui_browser/webui_browser_browsertest.cc`; rendering, focus, accessibility, drag, and mixed-DPI parity cannot be inferred from the shared implementation.
- Some browser surfaces assume a `BrowserView` anchor or a toolbar element. Each reached call site must be classified before removing standard Views chrome.
- Native menu completeness for an alternate shell needs end-to-end command validation, especially macOS key equivalents and dynamically enabled items.
- Linux fractional scaling, global menu, portal dialogs, and server-decoration behavior depend on runtime desktop capabilities and cannot be proven from source alone.
- Forced-colors and increased-contrast behavior for the hybrid native/WebUI shell needs a combined macOS accessibility and visual experiment in Gate 2. Equivalent Linux and Windows experiments are deferred until those platforms are proposed.
- GPU software fallback needs a real built shell because product effects may introduce requirements that the current architecture docs cannot predict.

The macOS unknowns are Gate 2 items and should be resolved with the smallest real-browser vertical slice before broad product UI work begins. Linux and Windows unknowns are deferred until either platform is proposed for support.
