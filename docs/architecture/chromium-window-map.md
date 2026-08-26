# Chromium desktop window map

## Scope and baseline

This document maps normal desktop browser-window construction, visual ownership, platform framing, and destruction in the pinned Chromium checkout. Limitless v1 supports macOS only. Linux and Windows findings are retained as future portability research and are not v1 acceptance requirements. The source baseline and `limitless/chromium.version` both resolve to:

```text
2b4613f0a24d234b139f1246028239a2f5dde58c
```

The map is intentionally source-specific. Paths and symbols are the durable references. Line numbers are omitted because they are likely to move during a Chromium update.

## Finding

The narrowest credible seam is Chromium's existing `WebUIBrowserWindow` path for normal windows, selected in `BrowserWindow::CreateBrowserWindow()` at `chrome/browser/ui/views/frame/browser_window_factory.cc`.

The factory already chooses between two production `BrowserWindow` implementations:

- `BrowserView`, the standard Views shell.
- `WebUIBrowserWindow`, an experimental top-chrome WebUI shell for normal windows.

The selected conditional architecture keeps `WebUIBrowserWindow` and its native `views::Widget`, replaces its frontend resources with a Limitless-owned trusted WebUI, adds only narrow browser-process adapters, and displays the independent browser-owned tab `WebContents` through `SurfaceEmbed`. Chromium continues to own `Browser`, `BrowserManagerService`, `BrowserWindowFeatures`, `TabStripModel`, sessions, commands, and tab lifecycle.

Do not add a third `BrowserWindow` initially. `BrowserWindowFeatures::InitPostWindowConstruction()` in `chrome/browser/ui/browser_window/internal/browser_window_features.cc` already has explicit branches for `BrowserView` and `WebUIBrowserWindow`; an unrelated third implementation falls into missing or stub feature paths. The following nearby mechanisms are also not product seams:

- `BrowserWindowCreateParams::window` in `chrome/browser/ui/browser_window/public/create_browser_window.h` is explicitly test-only. `Browser::Browser()` enforces this with `CHECK_IS_TEST()` in `chrome/browser/ui/browser.cc`.
- `BrowserNativeWidgetFactory::Set()` in `chrome/browser/ui/views/frame/browser_native_widget_factory.h` replaces the native host beneath `BrowserView`; it does not transfer ownership of the visible client shell.
- Restyling `BrowserView` would leave Limitless coupled to its large child hierarchy and layout contract.

This recommendation is conditional, not a claim that WebUI Browser is production-ready. `features::kWebium` in `chrome/common/chrome_features.cc` and `surface_embed::features::kSurfaceEmbed` in `components/surface_embed/common/features.cc` are disabled by default, `chrome/browser/flag_descriptions.h` calls Webium a prototype, `chrome/browser/ui/webui_browser/webui_browser_window.cc` contains numerous `NOTIMPLEMENTED_LOG_ONCE()` paths, and `chrome/browser/ui/webui_browser/webui_browser_browsertest.cc` disables the SurfaceEmbed pixel test on Linux. Gate 2 must prove focus, accessibility, IME, frame behavior, prompts, and rendering on supported Macs. If it fails, the fallback experiment compares pure Views with AppKit/SwiftUI hosting inside `BrowserView`, not GuestContents and not a third full window implementation.

## Startup and construction trace

```text
ChromeBrowserMainParts
  -> StartupBrowserCreator
    -> StartupBrowserCreatorImpl
      -> global CreateBrowserWindow(params)
        -> Browser::Create(params)
          -> Browser::Browser(params)
            -> BrowserWindowFeatures::Init(browser)
            -> BrowserWindow::CreateBrowserWindow(browser, ...)
              -> BrowserView + BrowserWidget, or WebUIBrowserWindow
            -> BrowserWindowFeatures::InitPostWindowConstruction(browser)
          -> BrowserManagerService::AddBrowser(unique_ptr<Browser>)
```

| Stage | Exact source and symbol | Why it matters |
| --- | --- | --- |
| Desktop startup object | `chrome/browser/chrome_browser_main.cc`, `ChromeBrowserMainParts::PreCreateThreadsImpl()` | Constructs `StartupBrowserCreator` before normal window launch. |
| Startup dispatch | `chrome/browser/chrome_browser_main.cc`, `ChromeBrowserMainParts::PreMainMessageLoopRunImpl()` | Calls `browser_creator_->Start(...)` after initial profiles are available and before entering the main run loop. |
| Command-line and profile routing | `chrome/browser/ui/startup/startup_browser_creator.cc`, `StartupBrowserCreator::Start()`, `ProcessCmdLineImpl()`, and `LaunchBrowser()` | Selects profiles, restore behavior, silent launch, app launch, and normal browser launch. A shell must not bypass this routing. |
| Restore or new window | `chrome/browser/ui/startup/startup_browser_creator_impl.cc`, `StartupBrowserCreatorImpl::Launch()`, `RestoreOrCreateBrowser()`, and `OpenTabsInBrowser()` | Restores a session or builds `BrowserWindowCreateParams`; Linux activation tokens and startup window titles are added here. |
| Public creation API | `chrome/browser/ui/browser_window/public/create_browser_window.h`, `BrowserWindowCreateParams` and `CreateBrowserWindow()` | Carries window type, profile, bounds, show state, workspace, restore state, platform activation data, and capabilities. |
| Desktop creation implementation | `chrome/browser/ui/browser_window/internal/create_browser_window_non_android.cc`, `CreateBrowserWindow()` | Validates profile suitability and delegates to `Browser::Create()` on desktop. |
| Authoritative browser owner | `chrome/browser/ui/browser.cc`, `Browser::Create()`; `chrome/browser/ui/browser_manager_service.cc`, `BrowserManagerService::AddBrowser()` | `Browser::Create()` constructs a `Browser` and transfers its `unique_ptr` to the profile-keyed manager service. |
| Model and feature construction | `chrome/browser/ui/browser.cc`, `Browser::Browser()` | Creates the tab-strip delegate and model, profile keepalive, `BrowserInitState`, and `BrowserWindowFeatures` before creating the visual window. |
| Visual shell selection | `chrome/browser/ui/views/frame/browser_window_factory.cc`, `BrowserWindow::CreateBrowserWindow()` | The exact seam that chooses `WebUIBrowserWindow` for enabled normal windows or creates the standard Views shell. |
| Post-window features | `chrome/browser/ui/browser.cc`, `Browser::Browser()` | Registers the session window and initializes features that require a completed window. A replacement must be usable before `InitPostWindowConstruction()`. |

Session restore can enter the same creation pipeline through `SessionRestore::RestoreSession()` rather than the simple new-window branch. The shell selector therefore belongs in the window factory, not only in startup code.

## Ownership model

```text
Profile
  owns BrowserManagerService
    owns unique_ptr<Browser>
      owns TabStripModel and its delegate
      owns BrowserWindowFeatures and BrowserInitState
      owns unique_ptr<BrowserWindow, BrowserWindowDeleter>
        standard path: BrowserView
          owns unique_ptr<BrowserWidget>
            Views widget hierarchy owns BrowserView as its ClientView
        WebUI path: WebUIBrowserWindow
          owns views::Widget and its trusted shell WebView
            SurfaceEmbed attaches independent tab WebContents surfaces
```

Important details:

- `BrowserManagerService` in `chrome/browser/ui/browser_manager_service.h` is the authoritative owner of the `Browser` instances for one profile. `GlobalBrowserCollection` is the cross-profile collection and activation-order view, not the owner.
- `Browser` owns its `BrowserWindow` through `BrowserWindowDeleter`, defined in `chrome/browser/ui/browser_window_deleter.*`. The deleter invokes the implementation's protected `DeleteBrowserWindow()` rather than directly calling `delete`.
- The standard Views path has intentionally unusual self-destruction. `BrowserView::DeleteBrowserWindow()` saves placement and resets `browser_widget_`; the `BrowserWidget` Views hierarchy owns the `BrowserView`, so `this` is invalid when the call returns.
- `WebUIBrowserWindow::DeleteBrowserWindow()` directly deletes the object. Its destructor tears down browser-window features and then its client-owned `views::Widget`.
- `BrowserWindowInterface` in `chrome/browser/ui/browser_window/public/browser_window_interface.h` is the small public browser-window identity and lifecycle interface. `BrowserWindow` in `chrome/browser/ui/browser_window.h` is the much broader legacy visual contract used by desktop features.
- The `BrowserWindowInterface` header explicitly steers new features toward `TabListInterface::From()`, `GetWindow()`, unowned user data, and capabilities instead of adding more methods. A Limitless adapter layer should follow that direction.

## Standard Views shell hierarchy

The normal standard path is assembled as follows:

```text
BrowserWindow::CreateBrowserWindow()
  -> new BrowserView(browser)
  -> BrowserWidget(BrowserView*)
  -> BrowserWidget::InitBrowserWidget()
     -> BrowserNativeWidgetFactory::CreateBrowserNativeWidget(...)
     -> BrowserWidget::Init(params)
        -> BrowserWidget::CreateRootView() -> BrowserRootView
        -> BrowserWidget::CreateFrameView() -> BrowserFrameView platform variant
        -> Widget delegate/client view -> BrowserView
```

| Layer | Exact source and symbol | Responsibility |
| --- | --- | --- |
| Browser visual contract | `chrome/browser/ui/browser_window.h`, `BrowserWindow` | Large legacy contract for toolbar, location bar, bubbles, downloads, keyboard handling, modal hosting, and window actions. |
| Client shell | `chrome/browser/ui/views/frame/browser_view.h`, `BrowserView` | `BrowserWindow`, `views::WidgetDelegate`, and `views::ClientView` implementation. Observes the tab model and coordinates visible browser UI. |
| Top-level widget | `chrome/browser/ui/views/frame/browser_widget.h`, `BrowserWidget` | `views::Widget` subclass that initializes the native widget, restores placement, creates the root and frame views, and coordinates frame state. |
| Root event layer | `chrome/browser/ui/views/frame/browser_root_view.h`, `BrowserRootView` | Root-level drag-and-drop and top-chrome/tab-strip event routing. It also filters unsafe dragged JavaScript URLs. |
| Non-client frame | `chrome/browser/ui/views/frame/browser_frame_view.h`, `BrowserFrameView` | Abstract frame, title, border, caption-button, hit-test, and layout-exclusion contract. |
| Client layout | `chrome/browser/ui/views/frame/layout/browser_view_layout.h`, `BrowserViewLayout` | Positions top chrome, bookmark bar, infobars, side panel, contents, overlays, and tab-modal dialog host. |
| Native host abstraction | `chrome/browser/ui/views/frame/browser_native_widget.h`, `BrowserNativeWidget` | Supplies `Widget::InitParams`, custom-frame choice, placement behavior, native system-menu behavior, and platform keyboard hooks. |
| Desktop host | `chrome/browser/ui/views/frame/browser_desktop_window_tree_host.h`, with `browser_desktop_window_tree_host_linux.cc` and `browser_desktop_window_tree_host_win.cc` | Bridges Views/Aura to the native desktop window and platform window-manager semantics. |

`BrowserView::AddedToWidget()` is the point where the element registry, toolbar, accelerators, immersive controller, app-frame UI, and `BrowserViewLayout` are wired after a native widget exists. This ordering matters because Browser features can query the window during construction, but several visual services cannot initialize until the Widget and native window exist.

## Selected WebUI shell hierarchy

The selected path does not wrap or inherit the standard `BrowserWidget`, `BrowserFrameView`, or `BrowserView` hierarchy:

```text
BrowserWindow::CreateBrowserWindow()
  -> new WebUIBrowserWindow(browser)
     -> client-owned views::Widget
        -> WidgetDelegate
           -> WebUIBrowserClientView
        -> views::WebView
           -> trusted chrome://webui-browser shell WebContents
              -> Limitless frontend resources and narrow Mojo adapters
              -> SurfaceEmbed
                 -> independent browser-owned tab WebContents
```

Exact boundaries:

- `chrome/browser/ui/webui_browser/webui_browser_window.cc`, `WebUIBrowserWindow::WebUIBrowserWindow()`, creates a client-owned `views::Widget`, a shell `views::WebView`, the modal-dialog host, extension container, native accelerators, and the trusted `chrome://webui-browser` WebContents.
- `WebUIBrowserWindow::WidgetDelegate::CreateClientView()` creates `WebUIBrowserClientView`. In `chrome/browser/ui/webui_browser/webui_browser_client_view.cc`, `DraggableRegionsChanged()` builds the shell drag region and `NonClientHitTest()` returns `HTCAPTION` inside it. The same client view reanchors permission UI when the tracked location icon moves.
- On macOS, `chrome/browser/ui/webui_browser/webui_browser_window_mac.mm`, `WebUIBrowserWindow::CreateNativeWidget()`, returns the dedicated `WebUIBrowserNativeWidgetMac`. It configures a full-size-content `NSWindow`, command dispatcher, menu validation, and keyboard command execution.
- On Linux and Windows, `WebUIBrowserWindow::CreateNativeWidget()` in `webui_browser_window.cc` returns `nullptr`, so `views::Widget` selects its default native widget. These platforms do not automatically inherit `BrowserNativeWidgetAuraLinux`, `BrowserDesktopWindowTreeHostWin`, or the standard `BrowserFrameView` variants.
- `chrome/browser/ui/browser_window/internal/browser_window_features.cc`, `BrowserWindowFeatures::InitPostWindowConstruction()`, explicitly selects WebUI-specific focus, find, exclusive-access, side-panel, and zoom implementations alongside shared browser features.
- `components/surface_embed/README.md` and `components/surface_embed/guest_view_vs_surface_embed.md` define SurfaceEmbed as a visual and input attachment for an independent `WebContents`, without a frontend scripting or document-communication API. `components/surface_embed/browser/surface_embed_host.cc` implements attachment, focus handoff, and parent accessibility forwarding.
- `chrome/browser/chrome_content_browser_client_receiver_bindings.cc`, `RegisterAssociatedInterfaceBindersForRenderFrameHost()`, binds `SurfaceEmbedHost` only when `kSurfaceEmbed` is enabled and the frame is controlled by `WebUIBrowserUI`.

The standard Views hierarchy remains the behavior and parity reference, plus the fallback architecture. It must not be described as behavior the selected WebUI path inherits automatically.

## Major visible UI ownership

This table maps Chromium's current presentation ownership. It does not prescribe the Limitless information architecture.

| Surface | Current visual owner and construction | Authoritative model or controller retained by Limitless |
| --- | --- | --- |
| Horizontal tab area | `chrome/browser/ui/views/frame/browser_view.cc`, `BrowserView::BrowserView()` calls `CreateHorizontalTabStripRegionView()`; controller code is under `chrome/browser/ui/views/tabs/` including `BrowserTabStripController` | `TabStripModel`, tab collections, groups, selection, and `Browser` tab observers |
| Optional vertical tab area | `BrowserView::BrowserView()` creates a vertical tab-strip region when selected by `VerticalTabStripStateController` | Same `TabStripModel`; vertical presentation is not a required Limitless constraint |
| Toolbar | `chrome/browser/ui/views/toolbar/toolbar_view.*`, constructed as a child of `TopContainerView` by `BrowserView::BrowserView()` | Browser commands, navigation state, command updater, toolbar feature controllers |
| Location bar and omnibox | `chrome/browser/ui/views/location_bar/location_bar_view.*` and `chrome/browser/ui/views/omnibox/omnibox_view_views.*`, owned below `ToolbarView` | Omnibox model, autocomplete controller/services, navigation controller, security state |
| App menu | `chrome/browser/ui/views/toolbar/app_menu.*` and toolbar menu button hosts | Chromium command IDs, command enablement, browser actions, platform menu routing |
| Page contents and split view | `chrome/browser/ui/views/frame/multi_contents_view.*` owns `ContentsContainerView` instances; `chrome/browser/ui/views/frame/contents_container_view.*` owns `ContentsWebView` and docked DevTools WebViews | Active tabs' `content::WebContents`, split-tab state, DevTools attachment |
| Status text | `chrome/browser/ui/views/frame/contents_web_view.*` owns `StatusBubbleViews` | Link target and loading/status information from WebContents delegates |
| Bookmark bar | `BrowserView::MaybeShowBookmarkBar()` lazily attaches `BookmarkBarView`; `chrome/browser/ui/bookmarks/bookmark_bar_controller.*` owns state | Bookmark model and `BookmarkBarController` |
| Infobars | `InfoBarContainerView` is a direct `BrowserView` child; `BrowserView::OnActiveTabChanged()` switches its manager | Per-WebContents `infobars::ContentInfoBarManager` |
| Side panel | `chrome/browser/ui/views/side_panel/side_panel.*`, created as a direct `BrowserView` child | Side-panel coordinator and registry features under `BrowserWindowFeatures` |
| Find in page | `BrowserView` provides a host placeholder; `chrome/browser/ui/views/frame/find_bar_owner_views.*` connects a separate find-bar Widget | `FindBarController` and per-WebContents `FindTabHelper` |
| Downloads | Toolbar download button and bubble use `DownloadToolbarUIController::From(browser)` and `DownloadBubbleUIController` | `DownloadManager`, download items, browser download controllers |
| Permission prompts | Views prompts and chips anchor to location-bar or contents UI; `BrowserView` updates `PermissionRequestManager` on active-tab changes | Per-WebContents `permissions::PermissionRequestManager` and content settings |
| Tab-modal dialogs | `ContentsContainerView` owns a `WebContentsModalDialogHost`; `BrowserViewLayout::GetWebContentsModalDialogHost()` exposes the active host | `WebContentsModalDialogManager` and `BrowserWindowModalDialogDelegate` |
| Window-modal and child Widgets | Created by individual browser features and parented to the browser native window | Feature-specific controllers and native modal ownership |

The main implementation risk is not rendering a tab or toolbar. It is satisfying the behavioral and anchoring assumptions currently spread across the large `BrowserWindow` interface. Limitless should adapt Chromium feature controllers to explicit shell-owned anchor, command, focus, and modal-host interfaces rather than copy Views implementations.

## Frame, non-client, and layout boundaries

On the standard Views path, `BrowserWidget::InitBrowserWidget()` in `chrome/browser/ui/views/frame/browser_widget.cc` asks `BrowserNativeWidgetFactory::CreateBrowserNativeWidget()` for the platform native widget and then initializes Views. `BrowserWidget::CreateFrameView()` calls `chrome::CreateBrowserFrameView()`. These sources define the native-behavior baseline and the fallback architecture.

Window hit testing is split:

- `BrowserView::NonClientHitTest()` handles client chrome and WebContents draggable regions, then delegates to layout and frame logic.
- `BrowserFrameView` variants own caption-button and non-client hit-test geometry.
- Desktop window-tree hosts translate the result into native resize, move, caption, and system-menu behavior.
- Web applications can supply window-controls-overlay geometry through the active `WebContents`; the browser shell must not make those areas clickable or draggable incorrectly.

The selected WebUI path uses `WebUIBrowserClientView` and a dedicated macOS native widget for v1. Gate 2 must compare traffic lights, drag and resize hit tests, application and window menus, placement, native and tab fullscreen, Spaces behavior, scale changes, and accessibility with the standard macOS sources above. If the WebUI substrate fails that comparison, a product-owned native composition within `BrowserView` retains Chromium's standard frame chain; a bounded experiment chooses Views or an AppKit/SwiftUI host. Default Views native widgets on Linux and Windows remain future portability concerns only.

## Close and destruction lifecycle

The user-visible close path is asynchronous because beforeunload handlers, policy, downloads, and tab shutdown may defer or cancel it.

```text
native close request
  -> BrowserView::OnWindowCloseRequested()
     or WebUIBrowserWindow::OnWindowCloseRequested()
  -> UnloadController::HandleBeforeClose()
     -> cancel, wait for unload handlers, or permit
  -> UnloadController::OnWindowClosing()
     -> session and tab-restore notifications
     -> close all tabs and WebContents
  -> UnloadController::OnWindowCloseComplete()
     -> mark deletion scheduled
     -> notify browser-did-close observers
     -> post BrowserManagerService::SynchronouslyDestroyBrowser()
  -> BrowserManagerService::DeleteBrowser()
     -> remove owned Browser from collections
     -> Browser::~Browser()
        -> window_.reset()
           -> BrowserWindowDeleter
              -> implementation DeleteBrowserWindow()
```

Exact evidence:

- `chrome/browser/ui/views/frame/browser_view.cc`, `BrowserView::OnWindowCloseRequested()`, hides a permitted-closing frame while tabs finish closing and delegates unload state to `UnloadController`.
- `chrome/browser/ui/webui_browser/webui_browser_window.cc`, `WebUIBrowserWindow::OnWindowCloseRequested()`, follows the same unload route and rearms the synchronous close callback if closing is deferred.
- `chrome/browser/ui/unload_controller.cc`, `UnloadController::OnWindowCloseComplete()`, posts manager destruction to return control to the run loop before deleting the browser.
- `chrome/browser/ui/browser_manager_service.cc`, `BrowserManagerService::DeleteBrowser()`, extracts the owner entry before destruction to avoid reentrant use-after-free.
- `chrome/browser/ui/views/frame/browser_view.cc`, `BrowserView::OnWidgetDestroying()`, deletes remaining `WebContents` while BrowserView layouts and helpers still exist.
- `chrome/browser/ui/views/frame/browser_widget.cc`, `BrowserWidget::OnNativeWidgetDestroyed()`, has a forced synchronous destruction path for native teardown.

Any Limitless shell must preserve the unload controller and manager-owned deletion sequence. Directly destroying the window, Widget, or `Browser` on a close event would bypass user prompts and can violate reentrancy assumptions.

## Platform frame dispatch

The following table is the standard `BrowserView` baseline:

| Platform | Native widget and host | Frame view | Key behavior |
| --- | --- | --- | --- |
| macOS | `chrome/browser/ui/views/frame/browser_native_widget_factory_mac.mm` creates `BrowserNativeWidgetMac`; implementation is in `chrome/browser/ui/views/frame/browser_native_widget_mac.mm` | `chrome/browser/ui/views/frame/browser_frame_view_factory_mac.mm` creates `BrowserFrameViewMac` | Normal windows use an `NSWindow` with titled, closable, miniaturizable, resizable, and full-size-content style masks. AppKit owns traffic lights and native fullscreen transitions; Chrome reserves their geometry and coordinates overlay top chrome. Custom frame is used for picture-in-picture, not normal browser windows. |
| Linux | `chrome/browser/ui/views/frame/browser_native_widget_aura_linux.cc` implements the factory and `BrowserNativeWidgetAuraLinux`; `BrowserDesktopWindowTreeHostLinux` in `browser_desktop_window_tree_host_linux.cc` bridges Ozone | `chrome/browser/ui/views/frame/browser_frame_view_factory_views.cc` selects Linux native-theme or opaque frame variants | Custom frame is mandatory if server decorations are unsupported. Otherwise the profile pref can choose system or Chromium frame. The host updates client shadow, decoration, input, and opaque regions and can expose a D-Bus global menu. Fullscreen and window state are asynchronous under the window manager. |
| Windows | `chrome/browser/ui/views/frame/browser_native_widget_factory_aura.cc` creates the Aura native widget; `BrowserDesktopWindowTreeHostWin` is in `browser_desktop_window_tree_host_win.cc` | `chrome/browser/ui/views/frame/browser_frame_view_factory_views.cc` selects `BrowserFrameViewWin` for native/DWM mode or `OpaqueBrowserFrameView` for custom mode | HWND/DWM integration controls frame mode, system menu, virtual desktops, workspaces, icon, insets, and native messages. Chromium caption controls and non-client hit testing vary with system versus custom frame selection. |

The selected `WebUIBrowserWindow` differs:

| Platform | Selected WebUI path | Consequence |
| --- | --- | --- |
| macOS | `WebUIBrowserNativeWidgetMac` in `chrome/browser/ui/webui_browser/webui_browser_window_mac.mm` | Explicit AppKit window style, titlebar, command-dispatch, and shortcut behavior exists, but parity with normal `BrowserNativeWidgetMac` fullscreen and traffic-light layout still needs E2E proof. |
| Linux | `WebUIBrowserWindow::CreateNativeWidget()` returns `nullptr`; generic `views::Widget` chooses the native widget. `WebUIBrowserClientView` supplies WebUI draggable hit testing. | Deferred beyond v1. Chromium's Linux browser-specific decoration preference, client-frame shadow, D-Bus menu, activation token, and async state code is not automatically inherited. |
| Windows | Same generic `views::Widget` selection plus `WebUIBrowserClientView`. | Deferred beyond v1. Chromium's `BrowserDesktopWindowTreeHostWin` system-menu, virtual-desktop, profile-icon, and browser-specific DWM behavior is not automatically inherited. |

Platform-specific behavior is expanded in `cross-platform-matrix.md`.

## Recommended integration boundary

The proposed boundary is:

```text
Chromium-owned                              Limitless-owned
-----------------------------------------   -------------------------------
BrowserManagerService and Browser           trusted WebUI frontend resources
WebUIBrowserWindow and views::Widget         product layout and styling
BrowserWindowFeatures                       narrow C++/Mojo adapters
TabStripModel and tab WebContents            projected shell state
SurfaceEmbed attachment and input routing   semantic shell accessibility
profiles, sessions, commands, services      product interaction composition
security-sensitive controllers/prompts      shell anchors where required
```

Initial implementation direction:

1. Keep `Browser::Create()` and manager ownership unchanged.
2. Use the existing `features::kWebium` normal-window selection and existing `WebUIBrowserWindow`. Do not fork it or add a third implementation.
3. Supply Limitless-owned resources for the trusted `chrome://webui-browser` frontend and add only capability-minimal browser-process adapters.
4. Attach real browser-owned tab `WebContents` through `SurfaceEmbed`. Do not use the development-only GuestContents proxy route.
5. Project browser models and command enablement into frontend state. Do not create parallel tab, navigation, permission, security, or session authority.
6. Retain Chromium-owned prompts, menus, secondary windows, and security-sensitive controllers until each is deliberately adapted and tested.

The existing selector applies only to `TYPE_NORMAL`. `TYPE_APP`, `TYPE_APP_POPUP`, `TYPE_POPUP`, `TYPE_DEVTOOLS`, and `TYPE_PICTURE_IN_PICTURE` should remain on the standard implementation until their requirements are separately mapped and approved.

## Stability and expected patch surface

Likely stable across rebases:

- `Browser` as the browser-window model and tab delegate.
- profile-keyed manager ownership of browser windows.
- the existing dual `BrowserView` and `WebUIBrowserWindow` implementation boundary.
- `views::Widget`, WebUI controller/resources, SurfaceEmbed, and platform-native layers.
- `WebContents`, tab models, command IDs, and feature controllers as authoritative state.

Expected narrow upstream touch:

- Chromium GN dependency and resource-registration edges that let the existing `chrome://webui-browser` controller serve Limitless-owned frontend resources.
- Capability-minimal additions to the existing WebUI browser page-handler or Mojo interface only where current browser-side adapters cannot express required product actions.
- A product feature-default or early configuration hook after Gate 2, so production `TYPE_NORMAL` windows select WebUI Browser and SurfaceEmbed without optional launch flags.
- Small anchor or capability adapters for retained Chromium UI only when the existing WebUI browser implementation lacks them.

Rebase warning signals:

- changes to `BrowserWindow::CreateBrowserWindow()` or the `WebUIBrowserWindow` selection;
- changes to `BrowserWindowFeatures` branches for `WebUIBrowserWindow`;
- additions to the legacy `BrowserWindow` virtual interface;
- changes to `WebUIBrowserUI`, its page-handler Mojo API, or resource registration;
- changes to SurfaceEmbed attachment, focus, accessibility, or binder restrictions;
- changes to `Browser` construction ordering or `BrowserManagerService` destruction;
- changes to WebUI native-widget initialization, client-view hit testing, or modal-dialog host lookup.

## Source-backed unknowns and validation experiments

| Unknown | Source evidence for uncertainty | Smallest useful experiment |
| --- | --- | --- |
| Is existing `WebUIBrowserWindow` complete enough for the thin slice? | The class satisfies the broad `BrowserWindow` interface but still logs numerous methods as unimplemented, and `BrowserWindowFeatures` has WebUI-specific branches only for some roles. | Replace only frontend resources, then run launch, navigation, second-tab, close/reopen, DevTools, download, permission, find, zoom, and HTTP-auth journeys with unimplemented-call logging. |
| Does SurfaceEmbed preserve real tab behavior? | It keeps an independent browser-owned `WebContents` and implements focus and AX forwarding, but end-user IME, drag, multi-window, Retina and mixed-display behavior is unproven. | Attach two real tabs, switch, resize, crash, move one to another window, traverse focus and VoiceOver order, and repeat across supported macOS versions and display configurations. |
| Do Linux and Windows retain browser-grade frame behavior later? | Non-macOS `WebUIBrowserWindow::CreateNativeWidget()` returns `nullptr`, so the standard browser native-widget and desktop-host classes are not automatically used. | Deferred beyond v1. Re-run the normal Views versus WebUI comparison before adding either platform to the product roadmap. |
| Which non-visual features still assume `BrowserView`? | `BrowserWindow::FromBrowser()` recognizes both production subclasses, while individual features can still call BrowserView-specific helpers. | Audit and instrument `BrowserView::GetBrowserViewForBrowser()` and `AsBrowserView()` call sites reached by the thin slice; adapt only the required capability or retain the surface. |
| Can all required prompts anchor without the standard location bar and layout? | `BrowserViewLayout`, `ContentsContainerView`, and toolbar Views currently supply modal and bubble hosts. | Provide explicit Limitless anchor and `WebContentsModalDialogHost` implementations, then exercise permissions, downloads, autofill, translate, and HTTP auth end to end. |
| Is the trusted WebUI boundary acceptable? | The shell adds a privileged renderer. SurfaceEmbed is deliberately scriptless, while any added page-handler Mojo API executes in the browser process. | Security-review binder scope, CSP, resource origins, tab/profile ownership checks, URL validation, and every new Mojo method. Reject generic command, scripting, or arbitrary navigation APIs. |
| Does selecting only `TYPE_NORMAL` preserve all opening behaviors? | Creation params define app, popup, DevTools, and picture-in-picture types, and the existing WebUI branch selects only normal windows. | Run popup, DevTools, installed-app, picture-in-picture, profile, and session-restore journeys while the normal shell is enabled. |
| Is close ordering correct under native destruction, beforeunload, and last-window shutdown? | Views and WebUI implementations contain distinct native close callbacks, while final Browser deletion is posted asynchronously. | E2E test close cancellation, accepted beforeunload, pending downloads, native OS close, quit, and last-window close with two profiles. |
| Is the fallback cleaner than continuing the hybrid? | Standard `BrowserView` preserves mature native behavior but eagerly constructs current chrome and exposes many concrete child assumptions. Chromium can compile Swift and `NativeViewHost` can attach an NSView, but there is no desktop SwiftUI shell precedent in the pinned tree. | If a Gate 2 failure is fundamental, compare pure Views and AppKit/SwiftUI hosting inside a simplified `BrowserView`, and enumerate required construction/layout seams. Do not create a third `BrowserWindow`. |

## Relevant test harnesses

Useful existing coverage includes:

- `chrome/browser/ui/views/frame/browser_view_browsertest.cc`
- `chrome/browser/ui/views/frame/browser_view_focus_interactive_uitest.cc`
- `chrome/browser/ui/views/frame/browser_native_widget_aura_linux_interactive_uitest.cc`
- `chrome/browser/ui/views/frame/browser_frame_view_win_interactive_uitest.cc`
- `chrome/browser/ui/views/frame/immersive_mode_controller_mac_interactive_uitest.mm`
- platform-specific `browser_frame_view_*_browsertest.cc` and unit tests under `chrome/browser/ui/views/frame/`
- tests under `chrome/browser/ui/webui_browser/` for the alternate shell

The architecture gate should not pass on source inspection alone. A thin Limitless resource replacement using the existing WebUI window and SurfaceEmbed must launch the real macOS app and prove navigation, focus, native close, resize, fullscreen, VoiceOver and Accessibility Inspector behavior, and at least one browser-owned modal surface. Linux and Windows do not block v1 and require their own architecture revalidation before future support is claimed.
