# Browser capability map

## Scope and baseline

This map describes desktop Chromium at commit
`2b4613f0a24d234b139f1246028239a2f5dde58c`. It identifies state and behavior
that a Limitless shell should consume rather than reproduce, and separates that
behavior from Chromium's current Views and WebUI presentation.

Limitless v1 supports macOS only. Shared capability ownership remains relevant
to future ports, but only macOS presentation and runtime behavior gate v1.

The words "authoritative" and "presentation" have narrow meanings here:

- An authoritative object owns, computes, persists, or arbitrates browser
  state. Limitless may call or observe it, but must not keep a parallel source
  of truth.
- A presentation object renders that state or collects user input. It may be
  retained, adapted, or replaced without moving ownership of the underlying
  browser behavior.
- An adapter is allowed to translate Chromium callbacks into Limitless view
  state. It must not independently decide command enablement, permission or
  security state, download danger, or tab lifecycle.

## Primary boundary finding

The useful non-visual boundary is not one class. It is a small set of scopes:

```text
Profile-scoped services
  Profile, PrefService, history, bookmarks, sessions, sync, downloads,
  identity, content settings, extensions
                |
                v
BrowserWindowInterface and BrowserWindowFeatures
  window identity, profile, activation and close lifecycle, commands
                |
                v
TabListInterface -> tabs::TabInterface -> content::WebContents
  tab collection       tab lifetime          document/navigation
                |
                v
Per-tab features
  permissions, page actions, security, find, zoom, translate, media,
  extension actions, printing
```

This split is supported directly by the pinned source:

- `chrome/browser/ui/browser.h`, `Browser`, is owned by
  `BrowserManagerService`. It owns the profile keep-alive, a
  `std::unique_ptr<BrowserWindow>`, a `TabStripModel`, and window-scoped feature
  state.
- `chrome/browser/ui/browser_window/public/browser_window_interface.h`,
  `BrowserWindowInterface`, exposes profile, session ID, activation, close
  notifications, active tab, modal-dialog hosts, and `BrowserWindowFeatures`.
  Its own warning says new tab and navigation clients should prefer
  `TabListInterface::From(BrowserWindowInterface*)` and that several legacy
  methods will be removed.
- `chrome/browser/tab_list/tab_list_interface.h`, `TabListInterface`, is the
  cross-platform tab collection API. It supports observation, activation,
  insert, close, move, pin, groups, split tabs, discard, and cross-window moves.
- `components/tabs/public/tab_interface.h`, `tabs::TabInterface`, is the public
  desktop tab lifetime API. It exposes title, URL, profile, current
  `WebContents`, activation and visibility callbacks, discard replacement,
  detach lifecycle, modal exclusion, pin/group/split state, and tab features.
- `content/public/browser/web_contents.h`, `content::WebContents`, and
  `content/public/browser/navigation_controller.h`,
  `content::NavigationController`, remain the page and navigation authorities.

The immediate implication is that a Limitless shell should bind to
`BrowserWindowInterface`, `TabListInterface`, and `TabInterface`, and descend to
`WebContents` or feature controllers only for page-specific behavior. New code
should not make `Browser*`, `BrowserView*`, or `TabStripModel*` its public
product boundary, even though some current Chromium commands still require the
legacy desktop implementation internally.

## Capability map

### Window, tabs, and page state

| Capability | Authoritative model or API | Observation and mutation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Browser-window identity and lifetime | `BrowserWindowInterface` in `chrome/browser/ui/browser_window/public/browser_window_interface.h`; `Browser` in `chrome/browser/ui/browser.h` | `GetProfile()`, `GetSessionID()`, `GetWindow()`, `RegisterBrowserDidClose()`, `RegisterBrowserCloseCancelled()`, activation callbacks, `GetFeatures()` | `BrowserView` and frame classes under `chrome/browser/ui/views/frame/` | Hold no raw window pointer beyond registered close callbacks. Keep product state window-scoped and let `Browser` retain profile and browser lifetime. `BrowserWindowInterface::GetWeakPtr()` is documented for short async work, not durable state. |
| Tab collection, ordering, selection, groups, splits | `TabListInterface` in `chrome/browser/tab_list/tab_list_interface.h`, backed on desktop by `TabStripModel` in `chrome/browser/ui/tabs/tab_strip_model.h` | `TabListInterfaceObserver` reports add, remove, active change, move, highlight, content replacement, destruction, and close-all; mutation methods cover open, close, move, pin, group, split, duplicate, and move-to-window | `TabStrip`, `TabContainer`, and `TabStripRegionView` under `chrome/browser/ui/views/tabs/` and `chrome/browser/ui/views/frame/tab_strip_region_view.h` | Prefer `TabListInterface::From()` over direct `TabStripModel` use. The legacy model is still the desktop authority and drives many commands, so the adapter must tolerate async close and tab drag reparenting. |
| Individual tab lifetime and state | `tabs::TabInterface` in `components/tabs/public/tab_interface.h` | Activation, visibility, selection, pin/group/split state, modal state, will-discard, will-detach, and did-insert callbacks | `Tab`, hover card, alert icons, and tab context UI under `chrome/browser/ui/views/tabs/` | Hold the `TabInterface`, then call `GetContents()` when needed. Its source explicitly warns that desktop discard can replace `WebContents`. A cached `WebContents*` is not valid across `RegisterWillDiscardContents()`. During tab reparenting, `GetBrowserWindowInterface()` can briefly be null. |
| Page title, visible URL, loading, document events | `content::WebContents` in `content/public/browser/web_contents.h` | `GetTitle()`, `GetVisibleURL()`, `GetLastCommittedURL()`, `IsLoading()` and `content::WebContentsObserver` callbacks such as `DidStartNavigation`, `DidFinishNavigation`, `TitleWasSet`, and `DidUpdateFaviconURL` | `ContentsWebView` in `chrome/browser/ui/views/frame/contents_web_view.h`, plus toolbar and tab views | Derive display state live or from observer events. Visible and last-committed URLs have different security semantics. Limitless must not label a pending visible URL as a committed origin. |
| Favicon | `favicon::ContentFaviconDriver` in `components/favicon/content/content_favicon_driver.h`; convenience helper `favicon::TabFaviconFromWebContents` in `chrome/browser/favicon/favicon_utils.h` | `FaviconDriver` observer and `GetFavicon()`; `WebContentsObserver::DidUpdateFaviconURL()` exposes candidates, not the final image | Tab, location bar, history, bookmark, and omnibox views | Use the favicon driver for the resolved image and invalidation. Do not download candidate URLs independently or retain a favicon after a tab's `WebContents` is replaced. |
| Navigation stack and navigation execution | `content::NavigationController` in `content/public/browser/navigation_controller.h`; browser-policy entry points in `chrome/browser/ui/browser_commands.h` | `CanGoBack()`, `CanGoForward()`, `GoBack()`, `GoForward()`, `LoadURLWithParams()`; prefer `chrome::GoBack`, `GoForward`, `Reload`, `Stop`, and `OpenCurrentURL` when browser policy and disposition are relevant | Navigation buttons and `LocationBarView` | Use browser commands for ordinary shell actions so restrictions, metrics, reload behavior, and disposition remain Chromium-owned. Use `NavigationController` for read-only stack state and narrowly scoped navigation that does not bypass browser policy. |

### Commands, keyboard, and menus

| Capability | Authoritative model or API | Observation and mutation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Browser command support and enablement | `chrome::BrowserCommandController` in `chrome/browser/ui/browser_command_controller.h`, stored as window feature data; facade functions in `chrome/browser/ui/browser_commands.h` | `SupportsCommand`, `IsCommandEnabled`, command observers, and `chrome::ExecuteCommand*`; controller recalculates state for tab, zoom, printing, content restrictions, fullscreen, downloads, DevTools, extension, and find changes | Toolbar buttons, app menu, keyboard handling, command-backed page actions | Limitless controls must bind enabled state to the command controller and invoke `chrome::ExecuteCommand*`. Reimplementing enablement would miss policy, modal, fullscreen, loading, and content restriction changes. |
| Keyboard accelerators | Command IDs in `chrome/app/chrome_command_ids.h`; mappings in `chrome/browser/ui/accelerator_table.h`; reserved-command policy in `BrowserCommandController::IsReservedCommandOrKey` | `GetAcceleratorList()`, `GetStandardAcceleratorForCommandId()`, platform mappings, and command execution | `BrowserView::LoadAccelerators`, `FindCommandIdForAccelerator`, and `AcceleratorPressed` in `chrome/browser/ui/views/frame/browser_view.cc` | Accelerator registration and focus dispatch are presentation responsibilities, but mapping, reservation, and command execution are not. A replacement shell must preserve pre-renderer handling, Escape behavior, platform modifiers, and extension keybindings. |
| App and tab menus | Command state plus `AppMenuModel` in `chrome/browser/ui/toolbar/app_menu_model.h` and `TabMenuModel` in `chrome/browser/ui/tabs/tab_menu_model.h` | `ui::SimpleMenuModel` delegate callbacks and command IDs | Views menu runners and toolbar app-menu button | Menu models contain important policy and enablement logic and can initially be retained even if the entry point is Limitless-owned. A redesigned menu should still use Chromium command execution and models where possible. |
| Page context menu | `RenderViewContextMenu` in `chrome/browser/renderer_context_menu/render_view_context_menu.h`, with specialized observers in the same directory | `InitMenu`, command enablement, and execution operate on the target frame and context parameters | Native Views menu presentation | Retain initially. It combines frame selection, link/image/media state, spelling, accessibility, extension items, enterprise restrictions, and content actions. Rebuilding from DOM or renderer data would be incomplete and unsafe. |

### Address input, search, and autocomplete

| Capability | Authoritative model or API | Observation and mutation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Omnibox orchestration | `OmniboxController` in `chrome/browser/ui/omnibox/omnibox_controller.h` | Owns `AutocompleteController`, `OmniboxEditModel`, and `OmniboxPopupStateManager`; `StartAutocomplete()` and `StopAutocomplete()` | `LocationBarView` and `OmniboxViewViews` under `chrome/browser/ui/views/location_bar/` and `chrome/browser/ui/views/omnibox/` | Reuse the controller stack. It owns ordering and lifetimes between autocomplete providers and edit state. Its source notes that the edit model contains pointers into providers owned by the autocomplete controller. |
| Address editing and acceptance | `OmniboxEditModel` in `chrome/browser/ui/omnibox/omnibox_edit_model.h` and abstract `OmniboxView` in `chrome/browser/ui/omnibox/omnibox_view.h` | Edit-model observers cover selection, results, icons, contents, and typing; the model also calls the view and popup interfaces directly | `OmniboxViewViews`, a `views::Textfield`, and Views or WebUI popup implementations | This is an existing presentation seam, but not a clean headless service. A custom Views shell may implement `OmniboxView`; a web-rendered shell would need a narrow native binding that validates input and preserves focus, IME, clipboard, drag, and paste-and-go behavior. |
| Search and suggestion providers | `AutocompleteController` in `components/omnibox/browser/autocomplete_controller.h`; `OmniboxClient` in `components/omnibox/browser/omnibox_client.h` | Result observers, input, provider lifecycle, profile prefs, template URL service, bookmarks, and scheme classification | Omnibox popup row views and WebUI popup | Consume matches as Chromium produces them. Do not issue parallel history/search queries because doing so would diverge from provider policy, ranking, keyword mode, extension-controlled search confirmation, and enterprise settings. |

### Profile data and persistence

| Capability | Authoritative model or API | Access and observation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Profiles and browser contexts | `Profile` in `chrome/browser/profiles/profile.h`; `ProfileManager` in `chrome/browser/profiles/profile_manager.h` | `BrowserWindowInterface::GetProfile()`, `Profile::FromBrowserContext()`, manager load and attribute APIs | Avatar menu, profile picker, first-run WebUI | A window's profile never changes. Respect original versus off-the-record profile boundaries and keep profile services keyed to the profile supplied by Chromium. |
| Preferences and policy-backed values | `PrefService` in `components/prefs/pref_service.h`; browser profile exposes `GetPrefs()` | Typed getters/setters, managed-state checks, and `PrefChangeRegistrar` | Settings WebUI, checkboxes, menus, toolbar state | Use registered prefs and policy-aware APIs. A Limitless settings layer must not write generated preference stores directly or assume a user value overrides enterprise policy. |
| History | `history::HistoryService` in `components/history/core/browser/history_service.h`; factory in `chrome/browser/history/history_service_factory.h` | Query and observer APIs; factory access requires an explicit service access type | `HistoryUI` in `chrome/browser/ui/webui/history/history_ui.h` | Use the service for product history surfaces. Keep deletions, sync state, and incognito exclusion in Chromium. Retaining `chrome://history` initially is lower risk. |
| Bookmarks | `bookmarks::BookmarkModel` in `components/bookmarks/browser/bookmark_model.h`; `BookmarkModelFactory::GetForBrowserContext` in `chrome/browser/bookmarks/bookmark_model_factory.h` | Model observer, node mutation, load lifecycle; browser commands for bookmark-current/all actions | Bookmark bar, bookmark bubble, manager WebUI | Do not mirror the tree. Observe load and model changes. Use commands for browser-context actions so edit dialogs and policy remain coherent. |
| Current session | `SessionService` in `chrome/browser/sessions/session_service.h`; `SessionServiceFactory` in `chrome/browser/sessions/session_service_factory.h`; `SessionRestore` in `chrome/browser/sessions/session_restore.h` | Browser and tab changes are persisted by the existing browser integration; restore APIs recreate browsers and tabs | Startup flow, crash bubble, and restored Chromium windows | The shell should participate through normal `Browser` and tab lifecycle rather than serializing its own Chromium tabs. Product-only workspace metadata may be separate, but must reference stable browser/session identifiers and reconcile disappearance. |
| Recently closed tabs and windows | `sessions::TabRestoreService` in `components/sessions/core/tab_restore_service.h`; `TabRestoreServiceFactory` in `chrome/browser/sessions/tab_restore_service_factory.h` | Observer and restore APIs; `BrowserCommandController` observes it for command enablement | Recent tabs menu and history UI | Use service state and `chrome::RestoreTab`. Do not infer recently closed items from Limitless UI events because unload cancellation and session filtering alter outcomes. |
| Sync | `syncer::SyncService` in `components/sync/service/sync_service.h`; `SyncServiceFactory::GetForProfile` in `chrome/browser/sync/sync_service_factory.h` | `SyncServiceObserver`, transport and data-type state | Settings, avatar, history, bookmarks, tab sync UI | Limitless should display state from the service but leave account, encryption, consent, policy, and data-type control to Chromium services. Sync is not a generic storage API for product-specific state without a dedicated data type. |
| Identity | `signin::IdentityManager` in `components/signin/public/identity_manager/identity_manager.h`; `IdentityManagerFactory` in `chrome/browser/signin/identity_manager_factory.h` | Profile-keyed observer and account/token state APIs | Avatar menu, sign-in and sync WebUIs, profile picker | Keep token and consent handling in Chromium. Limitless may observe high-level account state, but must not expose refresh tokens or conflate browser identity, web FedCM identity, and site login state. |

### Downloads, permissions, security, and page actions

| Capability | Authoritative model or API | Access and observation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Download lifecycle | `content::DownloadManager` in `content/public/browser/download_manager.h`; `download::DownloadItem` in `components/download/public/common/download_item.h`; profile `DownloadCoreService` in `chrome/browser/download/download_core_service.h` | Manager and item observers; state, GUID, URLs, target path, danger, interruption, progress, pause/cancel/resume | Download bubble Views and `chrome://downloads` WebUI | Use Chromium's manager and item operations. Never infer completion from bytes alone, skip danger checks, or open files independently. Item pointers are lifecycle-bound, so adapters should key durable UI rows by GUID and remove them on destruction. |
| Download display aggregation | `DownloadBubbleUpdateService` in `chrome/browser/download/bubble/download_bubble_update_service.h`; `DownloadUIModel` in `chrome/browser/download/download_ui_model.h` | Profile-keyed cache, display info, progress, accessible alerts, and sorted models | `DownloadBubbleUIController` plus views under `chrome/browser/ui/views/download/bubble/` | This service is a practical initial shell boundary because it already combines normal downloads and offline items and produces accessible alerts. Danger decisions still flow through the item/delegate, not through a custom row model. |
| Permission requests | Per-tab `permissions::PermissionRequestManager` in `components/permissions/permission_request_manager.h`; platform-independent `permissions::PermissionPrompt` in `components/permissions/permission_prompt.h` | Request manager owns and queues requests and implements accept-once, accept, deny, dismiss, ignore, navigation, visibility, and prompt recreation; observers report prompt lifecycle | `CreatePermissionPrompt` in `chrome/browser/ui/views/permissions/permission_prompt_factory.cc`, then chips, bubbles, embedded prompts, exclusive-access UI, or a macOS notification prompt | Preserve manager and delegate decisions. Presentation can be replaced through `PermissionPrompt`, but the current desktop factory is coupled to `BrowserView`, `LocationBar`, omnibox editing, fullscreen, Lens overlay, and PWA behavior. This is a required adapter or narrow factory seam, not a surface that can be omitted. |
| Persistent content settings | `HostContentSettingsMap` in `components/content_settings/core/browser/host_content_settings_map.h` | Policy and provider-aware get/set APIs plus observers | Settings WebUI and page-info permission controls | Use it only through the profile and content-setting abstractions. Do not store a Limitless permission database. Incognito, provider precedence, expiration, embargo, and system-level blocks must remain intact. |
| Per-page access and blocked state | `PageSpecificContentSettings` in `components/content_settings/browser/page_specific_content_settings.h` | Per-document data, site-data observers, permission-usage observers, microphone/camera state, and a delegate that notifies UI | Location-bar content-setting icons and Page Info | This is document-scoped. Reset UI on primary-page change and do not carry an allowed or blocked indicator to another document in the same tab. Its current delegate says `UpdateLocationBar`, so a shell adapter is needed to translate that into Limitless invalidation. |
| Security identity and certificate state | `chrome_security_state::GetVisibleSecurityState` in `chrome/browser/ssl/chrome_security_state_util.h`; `security_state::VisibleSecurityState` in `components/security_state/core/security_state.h`; `WebContentsObserver::DidChangeVisibleSecurityState` | Compute from the current `WebContents` when notified | Location icon and `PageInfoBubbleView` under `chrome/browser/ui/views/page_info/`; certificate viewer WebUI/native wrappers | Never cache, approximate, or derive TLS identity from the displayed URL. Use the computed visible state and retain certificate and Page Info flows until a reviewed presentation adapter covers every state. |
| Page actions | Per-tab `page_actions::PageActionController::From(tabs::TabInterface*)` in `chrome/browser/ui/page_action/page_action_controller.h` | Per-action model observers, show/hide, suggestion chips, anchored messages, activity, text/image/accessibility overrides, and controller destruction callback | Page-action views inside `LocationBarView` | This is an explicit controller-to-view boundary, but actions can carry security or lifecycle significance. A Limitless page-action host must register all required action IDs and preserve action activation, accessibility names, active state, and anchored-message behavior. |

### Find, zoom, print, save, share, translate, and media

| Capability | Authoritative model or API | Access and observation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Find in page | Per-tab `find_in_page::FindTabHelper` in `components/find_in_page/find_tab_helper.h`; window `FindBarController` and abstract `FindBar` in `chrome/browser/ui/find_bar/` | Helper starts/stops find and reports results; controller switches active contents and owns a platform presentation implementing `FindBar` | `FindBarView` in `chrome/browser/ui/views/find_bar_view.h` | This is a strong existing restyling seam. Supply a Limitless `FindBar` implementation or retain `FindBarView`, while keeping helper/controller session, selection, result, accessibility-focus, navigation, and macOS global-pasteboard behavior. |
| Page zoom | `zoom::ZoomController` in `components/zoom/zoom_controller.h`; `chrome::Zoom` and `CanZoom*` in `chrome/browser/ui/browser_commands.h` | Per-contents controller, zoom observer, zoom mode, default/relative/current state | Toolbar/menu controls and zoom bubble | Invoke browser commands and observe `ZoomController`. Extension manual/disabled modes and per-origin versus per-tab behavior make an independent zoom percentage unsafe. |
| Printing | Per-contents `printing::PrintViewManager` in `chrome/browser/printing/print_view_manager.h`; singleton `PrintPreviewDialogController` in `chrome/browser/printing/print_preview_dialog_controller.h` | `chrome::Print`, `BasicPrint`, print restrictions and manager state; controller owns initiator-to-preview-dialog relationships | `PrintPreviewUI` in `chrome/browser/ui/webui/print_preview/print_preview_ui.h` plus system print dialog | Retain the full flow. Scripted printing, policy, renderer blocking, preview WebContents, PDF handling, system dialogs, and accessibility trees are browser machinery, not shell state. |
| Save page and file selection | `chrome::SavePage` and `CanSavePage` declared in `chrome/browser/ui/browser_commands.h` and implemented in `chrome/browser/ui/browser_commands.cc`; `content::WebContents::OnSavePage`; `FileSelectHelper` in `chrome/browser/file_select_helper.h`; `SavePackageFilePicker` in `chrome/browser/download/save_package_file_picker.h`; `DownloadFilePicker` in `chrome/browser/download/download_file_picker.h` | Commands honor download restrictions and PDF-specific dispatch; helpers own asynchronous native dialog and listener lifecycle | Native `ui::SelectFileDialog` implementations and platform bridges | Invoke the command or helper and keep native dialogs. Do not accept paths through a privileged web shell and call browser APIs without the existing gesture, policy, ownership, and cancellation checks. |
| Sharing | `chrome::SharingHub`, `sharing_hub::SharingHubBubbleController`, `share::ShareAttempt`, and `components/sharing_message` | Per-contents controller and browser command | Sharing bubble, OS share integration, QR and send-tab surfaces | Retain initially or wrap the command entry point. The available targets and policies are dynamic. A Limitless button should invoke Chromium rather than enumerate share targets itself. |
| Translation | Per-contents `ChromeTranslateClient` in `chrome/browser/translate/chrome_translate_client.h`, owning `translate::TranslateManager`; commands in `chrome/browser/ui/browser_commands.h` | Language state, driver, manager, prefs, observers, translate and undo operations | `TranslateBubbleView` and translate page action under `chrome/browser/ui/views/translate/` | Observe and invoke the client/manager, but retain the current UI first. Translation is document-scoped and async, and automatic/manual policy plus errors must be represented. |
| Tab media and device indicators | `tabs::TabAlertController` in `chrome/browser/ui/tabs/alert/tab_alert_controller.h`; enum in `components/tabs/public/tab_alert.h` | `From(TabInterface*)`, active alerts, priority alert, and change callbacks cover audio, capture, PiP, devices, VR, and other tab alerts | Tab alert icons, hover card, mini toolbar | Use the tab feature rather than sampling media elements. It already merges `WebContents` capabilities and `MediaStreamCaptureIndicator` state and provides accessible text. |
| Capture state | `MediaStreamCaptureIndicator` and `MediaCaptureDevicesDispatcher` under `chrome/browser/media/webrtc/` | Observer callbacks and per-contents checks for audio, video, tab, window, and display capture | Toolbar/tab indicators, desktop capture chooser, platform indicators | Never hide active capture. Keep OS chooser and system permission flow. The shell must continue displaying an unambiguous indicator even when the originating tab is backgrounded. |
| Global media and casting | `MediaNotificationService` in `chrome/browser/ui/global_media_controls/media_notification_service.h`; `media_router::MediaRouterDialogController` in `components/media_router/browser/media_router_dialog_controller.h` | Profile service and route controllers | `MediaDialogView` and `MediaRouterDialogControllerViews` under `chrome/browser/ui/views/` | Retain first. Session actions, device discovery, route state, presentation requests, and policy are not equivalent to per-tab play state. |

### Extensions and developer tools

| Capability | Authoritative model or API | Access and observation | Current presentation | Limitless boundary and caveats |
| --- | --- | --- | --- | --- |
| Installed extensions and toolbar actions | `extensions::ExtensionRegistry`, `extensions::ExtensionActionManager`, and profile `ToolbarActionsModel` in `chrome/browser/ui/toolbar/toolbar_actions_model.h` | Registry and model observers, action IDs, pinned order, names, policy-restricted URLs, force pinning, permissions | `ExtensionsMenuView`, toolbar action views, settings WebUI | Use the registry and model. Do not expose an action on a restricted URL or invent pin state. `ToolbarActionsModel` distinguishes profile-global ordering from per-window layout owned by the current `ExtensionsContainer`. |
| Per-tab extension action permission and execution | `extensions::ExtensionActionRunner` in `chrome/browser/extensions/extension_action_runner.h` | `GetForWebContents`, `RunAction`, withheld permission checks, activeTab grants, reload prompt, blocked-action state | Extension action controller, reload dialog, toolbar icon state | Only run actions following a direct user activation and through this controller. Its `RunAction` result determines whether a popup, click event, side panel, or no visible action follows. |
| Extension popup content | `extensions::ExtensionViewHost` in `chrome/browser/extensions/extension_view_host.h` owns privileged extension `WebContents` and delegates navigation, keyboard, file chooser, eye dropper, resize, and window lookup | Host and registry lifecycle, popup close events, DevTools attachment | `ExtensionPopup` in `chrome/browser/ui/views/extensions/extension_popup.h`, a Views bubble accepting a generic `views::BubbleAnchor` | The host is authoritative; the bubble is replaceable presentation. Initially reuse `ExtensionPopup::ShowPopup` with a Limitless anchor. Preserve focus, max/min size, delayed first-load show, unload close, JavaScript-dialog blocking, and popup DevTools. |
| Extension options and management | Extension guest view under `extensions/browser/guest_view/extension_options/`; extension service/registry; `chrome://extensions` | Extension-owned content and management APIs | Extension options dialog/tab and `ExtensionsUI` | Retain initially. Extension permissions, incognito access, errors, update/install/uninstall, and policy need complete treatment before product restyling. |
| DevTools | `DevToolsWindow` in `chrome/browser/devtools/devtools_window.h`; command entry in `chrome::ToggleDevToolsWindow` | Open/toggle for inspected `WebContents` or agent host; docked and undocked lifecycle | DevTools frontend WebContents, docked contents, or browser window of `TYPE_DEVTOOLS` | Call the existing entry point. Do not embed a second DevTools protocol client. The shell seam must support docked layout and separate DevTools windows, popup inspection, file chooser, focus, and close ordering. |
| View source | `content::NavigationController::CanViewSource` and `chrome::CanViewSource`; command ID routing | Existing command creates/navigates the view-source contents | A tab or secondary browser contents using the view-source URL scheme | Retain. It is policy-gated with DevTools availability and should use the standard command path. |

## APIs that are useful but not sufficient as a product boundary

### `BrowserWindowInterface`

It is the correct window identity and lifecycle boundary, but not a single
complete shell API. The pinned header explicitly discourages adding features to
it and marks legacy tab methods for removal. Limitless should retrieve scoped
controllers from `BrowserWindowFeatures`, use `TabListInterface`, and keep
small adapters per capability.

### `TabStripModel`

It is the authoritative desktop implementation and remains required by many
Chromium internals. It is also a broad legacy surface with async close,
selection, grouping, split, drag, and window ownership details. Limitless
should use it only behind a desktop adapter where `TabListInterface` lacks a
needed operation, and should record each such use as expected rebase debt.

### `OmniboxController`

It is reusable and should be reused, but it is not fully view-neutral.
`OmniboxEditModel` calls `OmniboxView` and popup presentation directly in
addition to observer notifications. Replacing its presentation is feasible,
but is a substantial native UI adapter. A web shell would require an explicit
privileged bridge and a detailed focus, IME, clipboard, drag, accessibility,
and spoofing review.

### `PageActionController`

It supplies an explicit model/controller seam but represents many independent
features. Merely rendering actions that happen to be visible is insufficient.
The shell must create the expected action host, preserve callbacks and
anchored-message behavior, and test every security or privacy significant
action.

## Lifecycle rules for a Limitless adapter

1. Scope a window adapter to `BrowserWindowInterface::RegisterBrowserDidClose`
   and unregister every observer before window destruction.
2. Scope tab-list observation to `TabListInterfaceObserver::OnTabListDestroyed`.
3. Scope tab adapters to `tabs::TabInterface::RegisterWillDetach` and handle
   temporary loss of the browser window during reparenting.
4. Never cache a tab's `WebContents` across
   `tabs::TabInterface::RegisterWillDiscardContents`.
5. Rebind every `WebContentsObserver` and per-contents helper after discard.
6. Treat navigation, permission, content setting, security, translate, find,
   and many media states as document-scoped. Clear or recompute on primary-page
   change.
7. Use Chromium command observers for enabled state. UI clickability is a view
   of command state, not an independent product rule.
8. Keep profile-keyed services separated between normal, guest, and
   off-the-record profiles. Never merge their UI caches.
9. Let tab-modal surfaces acquire `tabs::ScopedTabModalUI` or use Chromium's
   existing manager. Do not place two mutually exclusive prompts over one tab.
10. Preserve visible origin, capture, permission, download danger, certificate,
    and interstitial states even when Limitless changes layout or hides
    conventional toolbar regions.

## Required adapter interfaces

The capability evidence supports the following narrow Limitless-owned
interfaces. These are design descriptions, not Phase 0 implementation:

```text
LimitlessWindowAdapter
  BrowserWindowInterface lifetime, profile, activation, commands
  TabListAdapter
    TabListInterface observation and mutations
    LimitlessTabAdapter per tabs::TabInterface
      current WebContents binding
      navigation, title, URL, favicon, loading, security, alerts
  OmniboxAdapter
    OmniboxController plus a native OmniboxView implementation or retained view
  PromptHost
    permissions, tab-modal dialogs, anchored bubbles, page actions
  SecondarySurfaceRouter
    DevTools, popups, apps, profile picker, task manager, system dialogs
```

`PromptHost` and `SecondarySurfaceRouter` are mandatory. A shell that can only
render one normal tab and execute navigation commands is not a browser-shell
boundary.

## Stability and rebase assessment

| Boundary | Expected stability | Reason |
| --- | --- | --- |
| `content::WebContents`, `NavigationController`, download and printing content APIs | High | Public embedder APIs with broad use and tests. |
| Profile-keyed services and component models | High to medium | Service factories and observer APIs are established, but feature availability and policy details change. |
| `TabListInterface`, `tabs::TabInterface`, `BrowserWindowInterface` | Medium and improving | They are explicit public interfaces, but the source is actively migrating away from legacy desktop classes. Rebase review must follow their comments and removals. |
| `chrome::BrowserCommandController` and `chrome::browser_commands` | Medium | Central and well tested, but some entry points still use desktop-only implementation details and command IDs change. |
| `FindBar`, `PermissionPrompt`, page-action controller | Medium | Explicit presentation seams exist, but each has current toolbar, location-bar, or window assumptions. |
| `OmniboxController` and `OmniboxView` | Medium to low for a custom implementation | Reusable but internally coupled and changing rapidly, including multiple popup technologies. |
| Direct `BrowserView`, `LocationBarView`, tab Views, and bubble subclass access | Low as a product API | Presentation implementation details with frequent layout and feature churn. Retain whole surfaces or isolate use behind one adapter. |

## Security invariants

- Security identity comes from `chrome_security_state::GetVisibleSecurityState`
  for the current `WebContents` and is refreshed on
  `DidChangeVisibleSecurityState`.
- Permission decisions remain in `PermissionRequestManager` and
  `HostContentSettingsMap`. A shell only renders and returns an explicit
  delegate decision.
- Download danger, interruption, target selection, and open behavior remain in
  `DownloadItem`, `ChromeDownloadManagerDelegate`, and their UI models.
- Navigation input is accepted through `OmniboxEditModel` and Chromium's
  command/navigation path. A web-rendered shell must not expose arbitrary
  privileged `LoadURL` without validating disposition, initiator, and user
  activation.
- Extension activeTab grants and blocked script execution remain in
  `ExtensionActionRunner` and require direct user action.
- Interstitial, authentication, certificate, payment, FedCM, capture, and
  external-protocol surfaces may not be hidden or replaced with incomplete
  approximations.

## Known unknowns and validation experiments

1. **Tab API completeness:** audit the thin slice against
   `TabListInterface`. If any action requires direct `TabStripModel`, list the
   exact operation and isolate it in the desktop tab adapter.
2. **Omnibox ownership:** construct the real `OmniboxController` with a minimal
   Limitless `OmniboxView` and prove typed navigation, suggestions, tab switch,
   IME composition, paste-and-go, accessibility, and extension-controlled
   search confirmation.
3. **Permission prompt factory:** create a geolocation, notification, camera,
   microphone, and quiet prompt in the new shell. The source-backed risk is the
   current factory's direct `BrowserView` and `LocationBar` lookup. Determine
   whether a retained anchor works or a narrow `ChromePermissionsClient` prompt
   host hook is required.
4. **Page action registration:** enumerate action IDs installed by `TabFeatures`
   at the pinned revision and verify the Limitless host observes all actions,
   including chips and anchored messages.
5. **Accelerator routing:** verify reserved browser commands before the
   renderer, editable-field behavior, extension commands, Escape, fullscreen,
   and macOS-specific mappings. Linux and Windows mappings are deferred until
   either platform is proposed for support.
6. **Discard and reparent:** force a background tab discard and drag a tab to a
   new window. Confirm title, URL, favicon, security, media, permissions, and
   observers rebind without duplicate state.
7. **Download danger and accessibility:** exercise safe, interrupted, dangerous,
   deep-scan, and save-as downloads. Confirm accessible alerts from
   `DownloadBubbleUpdateService` and Chromium-owned decisions survive custom
   presentation.
8. **DevTools and extension popups:** prove docked and undocked DevTools plus an
   inspected extension popup. These flows stress focus, secondary windows,
   popup lifetime, and `WebContents` ownership.

## Conclusion

Chromium already exposes most browser behavior below the current visual shell,
but the boundary is layered. The durable route is to keep `Browser` and its
profile, tab, command, and feature machinery authoritative, build the product
shell against the newer window and tab interfaces, and retain or adapt the
security-sensitive secondary surfaces. Directly reproducing state from Views
classes or a parallel web model would violate both lifecycle and security
invariants.
