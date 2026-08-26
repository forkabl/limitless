# Chromium UI surface inventory

## Scope and baseline

This inventory covers user-visible desktop surfaces in Chromium at commit
`2b4613f0a24d234b139f1246028239a2f5dde58c` that can appear outside a normal
tab strip or toolbar. Limitless v1 supports macOS only. Linux and Windows notes
are retained as deferred portability research. ChromeOS-only and Android-only
implementations are noted only when the shared desktop path has an important
branch.

The purpose is to prevent a custom Limitless shell from hiding, orphaning, or
silently dropping browser UI that is created by a page, profile service,
extension, secondary window, operating system, or security controller.

## Disposition legend

Each row has one Phase 0 disposition:

- **Retained unchanged initially:** keep Chromium's complete presentation and
  behavior. Limitless may supply a parent window or anchor but does not redesign
  the surface.
- **Restyled through an existing seam:** keep Chromium controllers and provide
  a Limitless implementation of an existing presentation interface.
- **Wrapped by Limitless UI:** Limitless owns the discoverable entry point or
  stable container while Chromium continues to render the consequential flow.
- **Replaced later:** keep the current surface reachable until a complete
  Limitless replacement is designed and tested, then replace its presentation.
- **Intentionally unsupported, product approval required:** do not ship the
  capability without an explicit product decision, migration behavior, and a
  user-visible explanation where applicable.

"Replaced later" does not authorize removal now. No current Chromium surface
may disappear merely because the main shell has no conventional toolbar anchor.
Disposition changes presentation ownership only. The model, controller, or
service named in each evidence cell retains lifecycle, policy, and security
decision ownership unless a later reviewed design explicitly replaces it.

## Core finding

There is no single "secondary UI" subsystem. Surfaces are routed through at
least six ownership paths:

```text
WebContents / document
  permissions, JavaScript dialogs, auth, print, find, payments, FedCM,
  interstitials, file chooser, translate, capture

Profile service
  downloads, extensions, media controls, notifications, identity, sync

Browser window feature
  commands, page actions, find controller, side panels, modal hosts

Global browser process
  profile picker, task manager, startup and crash recovery

Secondary BrowserWindowInterface type
  popup, app, DevTools, document picture-in-picture

Operating system
  file and print dialogs, notifications, certificate and system permissions
```

A complete shell therefore needs both a prompt host tied to a tab and a
secondary-surface router tied to window type and profile. Replacing only
`BrowserView`'s normal layout is insufficient.

## Security, permission, and modal surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Site permission requests, including geolocation, notifications, camera, microphone, MIDI, clipboard, sensors, and device access | A document adds a request to its per-`WebContents` manager. Requests can queue, merge, survive a tab switch, be recreated, or be finalized on navigation, visibility change, or destruction. | `components/permissions/permission_request_manager.h`, `permissions::PermissionRequestManager`; `components/permissions/permission_prompt.h`, `permissions::PermissionPrompt`; `chrome/browser/ui/views/permissions/permission_prompt_factory.cc`, `CreatePermissionPrompt` | Views chip, bubble, quiet icon, embedded prompt, exclusive-access prompt, or macOS native notification prompt. The factory looks up `BrowserView`, `LocationBar`, fullscreen, omnibox editing state, Lens overlay, and PWA status. | **Retained unchanged initially** | Mandatory. Keep manager-owned accept-once, accept, deny, dismiss, and ignore behavior. A new shell needs a compatible anchor or a narrow `ChromePermissionsClient::CreatePrompt` host seam. Exercise loud, quiet, embedded, OS-native, tab-switch, fullscreen, and hidden-tab variants. |
| Page Info, certificate details, cookies, and per-site permission controls | User activates security identity for the active document. State must update or close on navigation and tab lifecycle changes. | `chrome/browser/ui/page_info/chrome_page_info_delegate.h`, `ChromePageInfoDelegate`; `chrome/browser/ui/views/page_info/page_info_bubble_view.h`, `PageInfoBubbleView`; `chrome/browser/ssl/chrome_security_state_util.h`, `chrome_security_state::GetVisibleSecurityState` | Views bubble anchored to the location icon, with certificate viewer and site settings links | **Retained unchanged initially** | Mandatory. Limitless must provide a visible, trustworthy origin/security entry point and an anchor. Never synthesize certificate or connection state from the displayed URL. Verify mixed content, certificate error history, localhost, file, internal pages, and incognito. |
| HTTP and proxy authentication | Network stack creates a login delegate for a challenged request. The handler completes by supplying credentials or cancelling, and aborts if destroyed first. | `chrome/browser/ui/login/login_handler.h`, `LoginHandler::Create`, `ShowLoginPrompt`, `SetAuth`, `CancelAuth`; `chrome/browser/ui/views/login_handler_views.cc`, `LoginHandlerViews` | Web-modal Views dialog associated with the challenged `WebContents`; integrated with `HttpAuthManager` and password manager | **Retained unchanged initially** | Mandatory. Preserve request cancellation, proxy versus origin realm formatting, saved credential lookup, duplicate handler coordination, focus, and tab-modal blocking. A web shell must not receive raw credentials. |
| Client certificate selection | TLS client-auth request supplies certificate identities and a delegate. Selector closes on request cancellation, tab teardown, or explicit accept/cancel. | Cross-platform entry in `chrome/browser/ssl/ssl_client_certificate_selector.h`; Views implementation `chrome/browser/ui/views/ssl_client_certificate_selector.h`, `SSLClientCertificateSelector`; `chrome/browser/ui/views/ssl_client_certificate_selector.cc` | Native-style Views certificate-selection dialog, with platform certificate handling | **Retained unchanged initially** | Mandatory. Do not copy certificate identities into product state. Verify empty, single, multiple, hardware-backed, and cancelled selections plus parent-window destruction. |
| Certificate viewer and certificate manager dialogs | Opened from Page Info, DevTools, settings, or client-auth context | `chrome/browser/certificate_viewer.h`, `ShowCertificateViewer`, `ShowCertificateViewerForClientAuth`; `chrome/browser/ui/webui/certificate_viewer/certificate_viewer_ui.h`, `CertificateViewerUI`; platform wrappers `chrome/browser/ui/views/certificate_viewer_mac_views.mm` and `chrome/browser/ui/views/certificate_viewer_win.cc` | WebUI dialog with platform wrappers and native certificate integration | **Retained unchanged initially** | Mandatory. Parent and modality differ by platform. Keep certificate bytes and trust editing inside Chromium's reviewed flow. |
| JavaScript alert, confirm, prompt, and beforeunload | Renderer requests a dialog. Manager queues and associates it with the owning contents, watches navigation and destruction, and blocks or resumes the document. | `components/javascript_dialogs/app_modal_dialog_manager.h`, `AppModalDialogManager`; `components/javascript_dialogs/app_modal_dialog_controller.h`, `AppModalDialogController`; `components/javascript_dialogs/views/app_modal_dialog_view_views.h`, `AppModalDialogViewViews` | App-modal or tab-associated native Views dialog | **Retained unchanged initially** | Mandatory. Preserve suppression, origin labelling, beforeunload semantics, queueing, close order, and anti-abuse behavior. Test a background-tab request, nested request, tab close, navigation, and window shutdown. |
| Tab-modal WebUI and native dialogs | A feature claims the tab's modal slot and hosts a dialog over contents | `components/tabs/public/tab_interface.h`, `CanShowModalUI`, `ShowModalUI`, `RegisterModalUIChanged`; `chrome/browser/ui/tabs/public/tab_dialog_manager.h`, `TabDialogManager` | Constrained WebUI or Views dialog hosted by the browser window's modal host | **Retained unchanged initially** | A replacement content host must supply `GetWebContentsModalDialogHostForTab` and window-modal hosting through `BrowserWindowInterface`. Never allow overlapping exclusive tab-modal flows. |
| External protocol launch confirmation | Navigation targets a non-web protocol and policy cannot auto-resolve it | `chrome/browser/external_protocol/external_protocol_handler.h`, `ExternalProtocolHandler`; `chrome/browser/ui/views/external_protocol_dialog.h`, `ExternalProtocolDialog` | Native Views confirmation dialog | **Retained unchanged initially** | Mandatory. Preserve user gesture, remembered decision, policy, initiating origin, sandbox, and application-name checks. Do not auto-launch from a privileged shell bridge. |
| Intent picker and link-capture choice | A URL is eligible for an installed app or external handler | `chrome/browser/ui/intent_picker_tab_helper.h`, `IntentPickerTabHelper`; `chrome/browser/ui/views/intent_picker_bubble_view.h`, `IntentPickerBubbleView`; app delegates under `chrome/browser/apps/link_capturing/` | Location-bar bubble or dialog | **Retained unchanged initially** | Provide an anchor independent of a conventional location bar. Test no-app, one-app, multiple-app, remembered choice, installed web app, and macOS link-capture behavior. |
| Fullscreen, keyboard lock, pointer lock, and escape instructions | A page or user enters exclusive access. Controllers track state and require escape affordances and, for some requests, permission UI. | `chrome/browser/ui/exclusive_access/exclusive_access_manager.h`, `ExclusiveAccessManager`; controllers under `chrome/browser/ui/exclusive_access/`; `chrome/browser/ui/exclusive_access/exclusive_access_bubble.h`; Views under `chrome/browser/ui/views/exclusive_access/` | Top-of-window bubble or exclusive-access permission prompt | **Retained unchanged initially** | Mandatory. The main shell must implement the exclusive-access context and preserve exit instructions even when its ordinary chrome is hidden. Verify native fullscreen, tab fullscreen, pointer lock, keyboard lock, escape hold, multi-display, and focus loss. |
| Browser notifications | Site or extension notification is routed through a profile service to the platform bridge | `chrome/browser/notifications/notification_display_service.h`, `NotificationDisplayService`; `chrome/browser/notifications/notification_platform_bridge.h`, `NotificationPlatformBridge`; implementations in `chrome/browser/notifications/mac/notification_platform_bridge_mac.h`, `chrome/browser/notifications/notification_platform_bridge_win.h`, and `chrome/browser/notifications/notification_platform_bridge_linux.h` | Native OS notification where available, otherwise Chromium message center | **Retained unchanged initially** | Preserve profile attribution, incognito behavior, action routing, permission revocation, and process relaunch. Limitless branding can be applied only through supported product resources. |

## Download, file, find, print, and sharing surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Download progress, recent items, completion, and interruption | Profile download manager creates and updates `DownloadItem`s. Window controllers receive profile-service notifications and may outlive individual rows. | `content/public/browser/download_manager.h`, `DownloadManager`; `components/download/public/common/download_item.h`, `DownloadItem`; `chrome/browser/download/bubble/download_bubble_update_service.h`, `DownloadBubbleUpdateService`; `chrome/browser/download/bubble/download_bubble_ui_controller.h` | Toolbar button plus Views download bubble; complete history in `chrome://downloads` | **Wrapped by Limitless UI** | Limitless owns a persistent, discoverable downloads entry point and may host the existing bubble. Use update-service progress and accessible alerts. Key durable rows by download GUID, not item pointer. |
| Dangerous-download, deep-scan, password, and enterprise-review surfaces | Download state requires a user or enterprise decision before open, keep, or discard | `chrome/browser/download/download_danger_prompt.h`; danger and command state in `chrome/browser/download/download_ui_model.h`; security views under `chrome/browser/ui/views/download/bubble/` | Download bubble security subview, password prompt, or specialized dialog | **Retained unchanged initially** | Mandatory. Do not replace labels or action ordering casually. Test malware, uncommon, insecure, mixed-content, password-protected, deep-scan, enterprise block, and incognito cases. |
| Downloads manager page | User opens all downloads or a download action routes there | `chrome/browser/ui/chrome_pages.h`, `chrome::ShowDownloads`; `chrome/browser/ui/webui/downloads/downloads_ui.h`, `DownloadsUI` | `chrome://downloads` WebUI in a normal tab | **Replaced later** | Keep reachable from the Limitless downloads entry until feature parity covers history, search, pause/resume, retry, remove, show-in-folder, danger, accessibility, and policy. |
| Find in page | Browser command opens a window-scoped controller bound to the active tab; results are per tab and survive expected tab switches | `components/find_in_page/find_tab_helper.h`, `FindTabHelper`; `chrome/browser/ui/find_bar/find_bar.h`, abstract `FindBar`; `chrome/browser/ui/find_bar/find_bar_controller.h`, `FindBarController`; `chrome/browser/ui/views/find_bar_view.h` | `FindBarView`, usually inset over the contents area | **Restyled through an existing seam** | Implement `FindBar` or retain the existing view. Preserve focus restoration, screen-reader result focus, selected-text prefill, macOS global pasteboard, navigation close behavior, animation interruption, and no-match alert. |
| Print preview | User or script initiates printing. A controller associates initiator and preview `WebContents` and handles navigation, renderer exit, and teardown. | `chrome/browser/printing/print_view_manager.h`, `PrintViewManager`; `chrome/browser/printing/print_preview_dialog_controller.h`, `PrintPreviewDialogController`; `chrome/browser/ui/webui/print_preview/print_preview_ui.h`, `PrintPreviewUI` | Constrained print-preview WebUI, then optional native system print dialog | **Retained unchanged initially** | Mandatory. Preserve scripted-print restrictions, preview process, PDF path, policy, system dialog, accessibility tree, and renderer unblocking. Test print from normal page, PDF, selection, iframe, and script. |
| System print dialog | User requests basic print or enters it from preview | `PrintViewManager::BasicPrint`, `PrintForSystemDialogNow`; `chrome::BasicPrint` in `chrome/browser/ui/browser_commands.h` | Native platform print dialog | **Retained unchanged initially** | Parent native window and modality are platform-specific. Do not place printing behind a web-only path that cannot supply a correct native parent. |
| Save page and download save-as picker | Browser command invokes `WebContents::OnSavePage`; save package or download delegate creates a native chooser | `chrome/browser/ui/browser_commands.cc`, `chrome::SavePage`, `CanSavePage`; `chrome/browser/download/save_package_file_picker.h`; `chrome/browser/download/download_file_picker.h` | Native `ui::SelectFileDialog` | **Retained unchanged initially** | Preserve download restrictions, PDF save dispatch, suggested name, overwrite confirmation, cancellation, and window-close behavior. |
| Page file upload and directory picker | Renderer file chooser is mediated by the browser contents delegate and a ref-counted helper | `chrome/browser/ui/browser_web_contents_delegate/browser_web_contents_delegate.cc`, `BrowserWebContentsDelegate::RunFileChooser`; `chrome/browser/file_select_helper.h`, `FileSelectHelper`; `content/browser/web_contents/file_chooser_impl.h`, `FileChooserImpl` | Native `ui::SelectFileDialog`, with platform-specific and ChromeOS implementations | **Retained unchanged initially** | Mandatory. Preserve user activation, accept filters, directory enumeration, policy, sandboxed file access, cancellation, and selected-file grants. Never accept arbitrary paths from shell JavaScript. |
| File System Access picker and grants | Site requests handles and persistent or session permissions | `chrome/browser/file_system_access/chrome_file_system_access_permission_context.h`, `ChromeFileSystemAccessPermissionContext`; picker ultimately uses `ui::SelectFileDialog` | Native chooser plus permission and restore-grant bubbles | **Retained unchanged initially** | Keep grant lifetime and origin binding in Chromium. Test reload, tab restore, revoked permission, moved files, and off-the-record behavior. |
| Sharing hub, QR code, send-tab, and screenshot share | Browser command creates a per-contents attempt/controller | `chrome/browser/ui/browser_commands.h`, `chrome::SharingHub`, `GenerateQRCode`, `SendTabToSelf`, `ScreenshotCapture`; controllers under `chrome/browser/ui/sharing_hub/` | Anchored sharing bubble and platform share surfaces | **Wrapped by Limitless UI** | Limitless owns the share entry point but invokes Chromium's controllers. Preserve current page attribution, policy, available targets, and capture consent. |

## Extensions and developer surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Extension action icons and pinned order | Profile model reflects installed actions, user pinning, force pinning, and policy; per-tab runner adds activeTab and blocked-script state | `chrome/browser/ui/toolbar/toolbar_actions_model.h`, `ToolbarActionsModel`; `chrome/browser/extensions/extension_action_runner.h`, `ExtensionActionRunner`; `extensions/browser/extension_registry.h`, `extensions::ExtensionRegistry` | Toolbar action buttons and extensions menu | **Wrapped by Limitless UI** | Limitless renders a discoverable extension action area from Chromium models. It must not show actions on restricted URLs, bypass force pinning, or grant activeTab without direct activation. |
| Extension popup | Action execution returns a popup path. Popup waits for its extension document to load and closes on unload, tab change, widget events, or explicit close unless DevTools or a JavaScript dialog blocks closure. | `chrome/browser/extensions/extension_view_host.h`, `ExtensionViewHost`; `chrome/browser/ui/views/extensions/extension_popup.h`, `ExtensionPopup::ShowPopup` | Views bubble around extension-owned `WebContents`, using a generic `views::BubbleAnchor` | **Retained unchanged initially** | Reuse with a Limitless anchor. Preserve min/max sizing, keyboard prehandling, file chooser, navigation delegate, focus, unload, inspection, and popup JavaScript-dialog semantics. |
| Extensions menu and site-access controls | User opens list of actions and manages per-site access | `ToolbarActionsModel`; `extensions::PermissionsManager`; `chrome/browser/ui/views/extensions/extensions_menu_view.h`, `ExtensionsMenuView` | Views bubble from toolbar | **Wrapped by Limitless UI** | A Limitless trigger may open the current menu. Redesign only after site-access, policy, pinned state, blocked actions, reload requirements, and accessibility are covered. |
| Extension install, update-permission, disable, and external-install prompts | Extension service requires explicit consent or informs the user about changed permissions or an external install | `chrome/browser/extensions/extension_install_prompt.h`, `ExtensionInstallPrompt`; `extensions/browser/install_prompt_data.h`, `extensions::InstallPromptData`; `chrome/browser/ui/extensions/extension_post_install_dialog.h`, `extensions::TriggerPostInstallDialog` | Native Views confirmation or post-install dialog | **Retained unchanged initially** | Mandatory if extensions are supported. Preserve source attribution, permission warnings, enterprise/supervised-user decisions, cancellation, and window ownership. |
| Extension options and extension-owned dialogs | Extension opens options in a tab, guest view, or dialog | `extensions/browser/guest_view/extension_options/extension_options_guest.h`, `extensions::ExtensionOptionsGuest`; `chrome/browser/extensions/extension_view_host.h`, `extensions::ExtensionViewHost` | Extension content in a tab or modal host | **Retained unchanged initially** | Keep extension process and origin boundaries. Ensure the secondary surface router handles navigation, file chooser, keyboard, and close. |
| Extension management and errors | User manages install state, permissions, site access, incognito, errors, and developer mode | `chrome/browser/ui/chrome_pages.h`, `chrome::ShowExtensions`; `chrome/browser/ui/webui/extensions/extensions_ui.h`, `ExtensionsUI` | `chrome://extensions` WebUI | **Replaced later** | Keep reachable until a replacement supports every management and policy state. Product scope must explicitly decide unpacked extensions, developer mode, and legacy apps. |
| DevTools, docked | User command or extension-popup inspection attaches DevTools to a `WebContents` or agent host | `chrome/browser/devtools/devtools_window.h`, `DevToolsWindow::OpenDevToolsWindow`, `ToggleDevToolsWindow`; `chrome/browser/ui/browser_commands.h`, `chrome::ToggleDevToolsWindow` | DevTools frontend hosted beside inspected contents | **Retained unchanged initially** | The shell layout must reserve and resize docked contents, route focus and accelerators, and handle inspected target replacement/destruction. Keep DevTools policy gating. |
| DevTools, undocked and worker | DevTools creates a separate browser window or standalone frontend for an agent host | `chrome/browser/devtools/devtools_window.h`, `DevToolsWindow::Create`, `OpenDevToolsWindowForFrame`, `OpenDevToolsWindowForWorker`; `chrome/browser/ui/browser_window/public/browser_window_interface.h`, `BrowserWindowInterface::TYPE_DEVTOOLS` | Secondary browser window with DevTools frontend | **Retained unchanged initially** | The window factory must recognize `TYPE_DEVTOOLS` and avoid forcing normal Limitless tab UI onto it. Test close order, always-on-top interactions, file chooser, certificate viewer, and reopen. |
| View source | Browser command checks controller and DevTools policy, then opens source contents | `chrome/browser/ui/browser_commands.cc`, `chrome::CanViewSource`; `content::NavigationController::CanViewSource` | View-source document in a tab or secondary contents | **Retained unchanged initially** | Keep scheme and policy handling. The Limitless tab model must display a trustworthy view-source state and support the created tab. |

## Profile, startup, internal page, and utility surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Profile picker and profile creation | Startup, profile menu, locked/unusable profile, background mode, or explicit add/manage action opens a singleton picker window that can be reactivated or replaced for new parameters. | `chrome/browser/ui/profiles/profile_picker.h`, `ProfilePicker::Show`; `chrome/browser/ui/views/profiles/profile_picker_view.h`, `ProfilePickerView`; `chrome/browser/ui/webui/signin/profile_picker_ui.h`, `ProfilePickerUI` | Separate Views window hosting profile-picker WebUI | **Replaced later** | Keep unchanged until a replacement covers loaded/unloaded profiles, lock, force sign-in, guest, deletion, creation, reauthentication, policy, callback-on-close, and startup target URL. The secondary router must not assume a `Browser` or normal profile exists. |
| First-run experience | Profile-keyed service decides whether policy and account state require first run, opens the profile picker flow, and resumes or aborts the interrupted startup task on exit. | `chrome/browser/ui/startup/first_run_service.h`, `FirstRunService::OpenFirstRunIfNeeded`; `ProfilePicker::Params::ForFirstRun` | Profile-picker WebUI flow | **Replaced later** | Keep unchanged until onboarding, sync consent, policy skip, profile naming, quit/abandon behavior, and resume callbacks have parity. First run is not just a marketing page. |
| Crash restore prompt | Startup detects an unclean exit and asks whether to restore, with profile restrictions and optional metrics choice. | `chrome/browser/ui/session_crashed_bubble.h`, `SessionCrashedBubble::ShowIfNotOffTheRecordProfile`; `chrome/browser/ui/views/session_crashed_bubble_view.h`, `SessionCrashedBubbleView`; startup call in `chrome/browser/ui/startup/infobar_utils.cc` | Views bubble associated with a browser window | **Retained unchanged initially** | Mandatory. Supply a stable anchor or centered fallback before removing conventional chrome. Verify normal, guest, incognito, no-tab, policy, multi-window, and repeated-crash paths. |
| Automatic session restore and restored windows | Startup/session service recreates browser windows and tabs, including normal, popup, app, pinned, split, and WebUI states. | `chrome/browser/sessions/session_restore.h`, `SessionRestore`; `chrome/browser/sessions/session_service.h`, `SessionService`; broad coverage in `chrome/browser/sessions/session_restore_browsertest.cc` | Recreated browser windows plus optional crash UI | **Retained unchanged initially** | The Limitless window factory must handle every restored supported window type. Product workspace metadata must reconcile Chromium restore rather than create duplicate tabs or windows. |
| Settings | User command or feature opens settings or a subpage for a profile | `chrome/browser/ui/chrome_pages.h`, `chrome::ShowSettings`, `ShowSettingsSubPage`; `chrome/browser/ui/webui/settings/settings_ui.h`, `SettingsUI` | `chrome://settings` WebUI in a tab | **Replaced later** | Keep reachable. A replacement must preserve managed settings, deep links, search, accessibility, guest/incognito routing, system settings links, certificates, permissions, passwords, privacy, and reset flows. |
| History and synced/recent tabs | User opens history, recent tabs, or restore actions | `chrome/browser/ui/chrome_pages.h`, `chrome::ShowHistory`; `chrome/browser/ui/webui/history/history_ui.h`, `HistoryUI`; `TabRestoreService` and history service | `chrome://history` WebUI and menus | **Replaced later** | Keep reachable until local history, deletion, sync, grouped visits, recently closed windows, foreign sessions, incognito exclusion, policy, and pagination have parity. |
| Bookmarks manager | User opens or edits the full bookmark tree | `BookmarkModel` and `BookmarkModelFactory`; `chrome/browser/ui/webui/bookmarks/bookmarks_ui.h`, `BookmarksUI` | `chrome://bookmarks` WebUI, plus bookmark-edit bubbles | **Replaced later** | Keep manager and current edit bubble until tree mutation, undo, drag/drop, managed nodes, search, import/export, and accessibility are covered. |
| Task manager | User command opens or reactivates one global utility window backed by process/task providers. | `chrome/browser/task_manager/task_manager_interface.h`, `TaskManagerInterface`; `chrome/browser/ui/views/task_manager_view.h`, `TaskManagerView::Show`; command `chrome::OpenTaskManager` | Standalone Views dialog/window | **Retained unchanged initially** | Keep utility window outside the Limitless normal-shell factory. Preserve end-process policy, process grouping, search, columns, platform metrics, keyboard, and activation of represented tabs. |
| App menu and native application/system menus | User opens browser menu or OS application/window menu | `chrome/browser/ui/toolbar/app_menu_model.h`, `AppMenuModel`; accelerator and platform menu code under `chrome/browser/ui/` and Views frame | Views menu on Linux/Windows; native application menu conventions on macOS | **Wrapped by Limitless UI** | Limitless may present an opinionated menu, but every supported command and managed/disabled state needs a route. Keep native macOS application-menu behavior and Windows system menu/window commands. |
| Page and tab context menus | Right click or keyboard menu key on page or tab | `chrome/browser/renderer_context_menu/render_view_context_menu.h`, `RenderViewContextMenu`; `chrome/browser/ui/tabs/tab_menu_model.h`, `TabMenuModel` | Native menus | **Retained unchanged initially** | Page menu combines frame target, media, spelling, accessibility, extensions, enterprise, save, print, translate, and inspect. Retain models and presentation until feature parity is demonstrated. |
| Infobars and startup notices | Browser or profile features attach informational or confirmation UI to a tab or window | `components/infobars/core/infobar_manager.h`, `InfoBarManager`; Chrome delegates under `chrome/browser/ui/startup/` and feature directories | Bar above page contents, sometimes global confirmation UI | **Retained unchanged initially** | The content layout must reserve space and not cover the page. Inventory delegates during implementation because this is an open-ended extension point for policy, automation, default browser, obsolete system, recovery, and feature notices. |
| Default-browser, update, sign-in, and policy startup prompts | Startup or profile services request a bubble, modal dialog, infobar, or settings handoff | `chrome/browser/ui/startup/default_browser_prompt/`; `chrome::OpenUpdateChromeDialog`; sign-in controllers under `chrome/browser/ui/signin/` | Several Views and WebUI surfaces with OS-settings links | **Retained unchanged initially** | Keep until branding, update ownership, default-browser integration, policy, and platform settings behavior are explicitly decided. Do not leave a hidden prompt blocking startup. |

## Password, autofill, payment, identity, and translation surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Password save, update, manage, leak warning, generation, and account chooser | Password manager controller reacts to form state, navigation, credential selection, generation, or compromise and associates UI with the current contents | `chrome/browser/ui/views/passwords/password_bubble_view_base.h`, `PasswordBubbleViewBase::ShowBubble`; password views and controllers under `chrome/browser/ui/views/passwords/`; core services under `components/password_manager/` | Location-bar bubble, popup, or tab-modal dialog | **Wrapped by Limitless UI** | Limitless must expose the Chromium page action and provide an anchor, while retaining the complete flow. Never copy passwords into shell state. Test navigation, save/update, passkey interaction, generation, leak dialog, incognito, and enterprise policy. |
| Autofill address, card, IBAN, offer, virtual-card, and mandatory-reauth prompts | Per-contents controllers show save/manage/offers or authentication UI after Autofill events | Controllers under `chrome/browser/ui/autofill/`, including `SaveCardBubbleControllerImpl`, `SaveAddressBubbleController`, and `IbanBubbleController`; Views through `chrome/browser/ui/views/autofill/autofill_bubble_handler_impl.h` and related files | Location-bar bubbles, dialogs, and platform authentication | **Wrapped by Limitless UI** | Render page-action entry state from Chromium and retain prompt contents. Preserve reauth, legal messages, server/local distinction, strike databases, off-the-record restrictions, and policy. |
| Payment Request | Page invokes the Payment Request API and a content manager arbitrates one active request for the contents | `components/payments/content/payment_request.h`, `PaymentRequest`; `components/payments/content/payment_request_display_manager.h`, `PaymentRequestDisplayManager`; `chrome/browser/ui/views/payments/payment_request_dialog_view.h`, `PaymentRequestDialogView` | Tab-modal Views dialog with multiple sheets and payment-handler WebContents | **Retained unchanged initially** | Mandatory. Do not bridge payment credentials through shell UI. Preserve merchant origin, secure-context restrictions, one-request-at-a-time, retry/update events, handler navigation, cancellation, and native authentication. |
| FedCM account chooser and identity dialogs | Site calls identity credentials API. Identity controller owns the request and desktop view is additionally scoped to tab lifetime and visibility. | `chrome/browser/ui/webid/identity_dialog_controller.h`; `chrome/browser/ui/webid/account_selection_view.h`; `chrome/browser/ui/views/webid/fedcm_account_selection_view_desktop.h`, `webid::FedCmAccountSelectionView` | Bubble, modal, page action, or popup depending on RP mode and state | **Retained unchanged initially** | Mandatory. The source explicitly scopes the view to both identity request and `TabInterface`, closes on tab loss, and can hide for background or undersized contents. Preserve relying-party and identity-provider attribution and anti-clickjacking layout. |
| Translate | Language detection and translate manager decide availability, current step, languages, policy, and errors for a document | `chrome/browser/translate/chrome_translate_client.h`, `ChromeTranslateClient`; `components/translate/core/browser/translate_manager.h`; Views under `chrome/browser/ui/views/translate/` | Page action plus `TranslateBubbleView` | **Wrapped by Limitless UI** | Limitless hosts the page action and invokes existing bubble/manager. Verify detected, translating, translated, error, never-translate, always-translate, language selection, navigation, and incognito. |

## Media, casting, capture, and picture-in-picture surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Tab audio, capture, device, VR, and PiP indicators | Per-tab feature observes current `WebContents` and media capture service, then emits prioritized alert changes across discard | `chrome/browser/ui/tabs/alert/tab_alert_controller.h`, `tabs::TabAlertController`; `components/tabs/public/tab_alert.h`, `tabs::TabAlert` | Tab icon, hover card, mini toolbar, accessibility labels | **Restyled through an existing seam** | Limitless renders these states from `TabAlertController`, including accessible text. Never hide recording or capture because a tab is not selected. Rebind automatically after discard. |
| Global media controls | Profile service aggregates media sessions and casts; user controls playback and devices | `chrome/browser/ui/global_media_controls/media_notification_service.h`, `MediaNotificationService`; `chrome/browser/ui/views/global_media_controls/media_dialog_view.h`, `MediaDialogView` | Toolbar bubble or centered Views dialog | **Wrapped by Limitless UI** | Provide a discoverable entry point and retain dialog initially. Test local, remote, multiple sessions, PiP, browser restart, and no-toolbar anchor fallback. |
| Casting and presentation routes | Presentation request or user command opens route picker and observes sink/route state | `components/media_router/browser/media_router_dialog_controller.h`, `MediaRouterDialogController`; `chrome/browser/ui/views/media_router/media_router_dialog_controller_views.h`, `MediaRouterDialogControllerViews`; `chrome/browser/ui/views/media_router/cast_dialog_coordinator.h`, `CastDialogCoordinator` | Anchored or centered cast dialog, sometimes integrated with global media controls | **Wrapped by Limitless UI** | Preserve source selection, device discovery, route start/stop, policy, error state, and profile isolation. Provide a centered fallback if no toolbar action exists. |
| Screen, window, and tab capture chooser | `getDisplayMedia`, casting, or browser share flow requests sources and owns a callback until selection or cancel | `chrome/browser/media/webrtc/desktop_media_picker.h`, `DesktopMediaPicker`; controller and Views under `chrome/browser/ui/views/desktop_capture/`; platform implementations in media capture code | Native/Views source picker and OS screen-recording permission flows | **Retained unchanged initially** | Mandatory. Preserve source thumbnails, audio options, self-capture warnings, OS permission, cancellation, and user activation. A shell bridge must not choose a source silently. |
| Active capture indicators | Capture service tracks audio, video, tab, window, and display capture and notifies observers | `chrome/browser/media/webrtc/media_stream_capture_indicator.h`, `MediaStreamCaptureIndicator`; `chrome/browser/media/webrtc/media_capture_devices_dispatcher.h`, `MediaCaptureDevicesDispatcher` | Tab/window indicators and platform status | **Restyled through an existing seam** | Limitless may render indicators from authoritative callbacks, but OS and Chromium indicators remain until parity is proven. Test background tabs, multiple simultaneous captures, device loss, stop sharing, and window close. |
| Web Share and send-to-device dialog | Site Web Share or browser sharing action requests an OS or Chromium share target | `chrome/browser/webshare/share_service_impl.h`, `ShareServiceImpl`; `components/sharing_message/sharing_dialog.h`; `chrome/browser/ui/views/sharing/sharing_dialog_view.h` | Native OS share UI or Views dialog | **Retained unchanged initially** | Preserve user activation, file validation, cancellation, DLP/policy, and native parent window. |
| Video picture-in-picture | Media element enters PiP and a window controller owns playback window lifecycle | `content/public/browser/video_picture_in_picture_window_controller.h`; Views implementation under `chrome/browser/ui/views/picture_in_picture/` | Always-on-top secondary native widget | **Retained unchanged initially** | Window routing must preserve controls, occlusion, always-on-top, accessibility, and close/return-to-tab behavior. |
| Document picture-in-picture | Document creates a restricted PiP `WebContents` with special navigation and opener rules | `content/public/browser/document_picture_in_picture_window_controller.h`; `BrowserWindowInterface::TYPE_PICTURE_IN_PICTURE`; implementations under `content/browser/picture_in_picture/` and `chrome/browser/ui/views/picture_in_picture/` | Special secondary browser/native window with custom frame | **Retained unchanged initially** | Do not apply the normal Limitless shell. Preserve navigation restrictions, opener relation, child dialogs, tucking/occlusion behavior, file dialogs, and platform frame. |

## Error, warning, and network surfaces

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Network error pages | Navigation commits an internal error document; helper updates diagnostics, reload, and recovery behavior | Renderer support under `chrome/renderer/net/`; `chrome/browser/net/net_error_tab_helper.h`, `NetErrorTabHelper`; shared error-page components under `components/error_page/` | Error document in the affected tab | **Retained unchanged initially** | Mandatory. Do not replace with shell error state. Preserve failed URL handling, offline/captive actions, reload, diagnostics, DNS probes, and security origin display. |
| SSL, HTTPS-only, bad-clock, MITM, insecure-form, and captive-portal interstitials | Navigation throttle or SSL handler attaches a blocking page to the target `WebContents`; controller owns proceed/back actions and metrics | `components/security_interstitials/content/security_interstitial_page.h`, `SecurityInterstitialPage`; `components/security_interstitials/content/security_interstitial_tab_helper.h`, `SecurityInterstitialTabHelper` | Full-page trusted interstitial in the affected tab | **Retained unchanged initially** | Mandatory and not restylable in the shell phase. Never cover, bypass, auto-proceed, or reproduce these pages. Test history/back, reload, cert changes, clock correction, captive portal, and tab restore. |
| Safe Browsing malware, phishing, unwanted-software, and billing warnings | Safe Browsing blocking-page factory creates a security interstitial for unsafe resources or navigation | `components/safe_browsing/content/browser/safe_browsing_blocking_page.h`, `SafeBrowsingBlockingPage`; `chrome/browser/safe_browsing/chrome_safe_browsing_blocking_page_factory.h`, `ChromeSafeBrowsingBlockingPageFactory` | Full-page or subresource interstitial | **Retained unchanged initially** | Mandatory. Keep reporting, proceed restrictions, policy, enhanced-protection messaging, and subresource behavior. No shell navigation affordance may obscure the warning. |
| Captive portal login and detection | Portal detector or SSL error flow routes user to login and may maintain original tab state | `components/security_interstitials/content/captive_portal_blocking_page.h`, `CaptivePortalBlockingPage`; captive portal helpers in the same component | Blocking page, login tab, or platform network UI | **Retained unchanged initially** | Preserve network-change observation, original navigation recovery, and platform routing. Test portal detected before and during navigation. |
| Browser or renderer crash contents | Renderer process termination leaves a sad-tab or crash presentation, while browser crash is handled by restart and session recovery | `chrome/browser/ui/sad_tab_helper.h`, `SadTabHelper`; `chrome/browser/ui/sad_tab_controller.h`, `SadTabController`; `chrome/browser/ui/views/sad_tab_view.h`, `SadTabView`; browser crash recovery via session services | Per-tab crash view or next-start recovery prompt | **Retained unchanged initially** | Limitless must not show stale page state over a dead renderer. Verify reload, repeated crash, OOM, process kill, crash during modal UI, and accessibility. |

## Secondary browser and application windows

`BrowserWindowInterface::Type` in
`chrome/browser/ui/browser_window/public/browser_window_interface.h` enumerates
normal, popup, app, DevTools, app-popup, and document picture-in-picture browser
types. The shell construction seam must dispatch explicitly on type.

| Surface | Trigger and lifecycle | Source evidence at the pinned revision | Current presentation and anchor | Disposition | Requirements and concerns |
| --- | --- | --- | --- | --- | --- |
| Script and user popup windows | `window.open`, link disposition, or browser command creates a `TYPE_POPUP` browser with feature-constrained chrome | `chrome/browser/ui/browser_window/public/browser_window_interface.h`, `BrowserWindowInterface::TYPE_POPUP`; creation and policy through browser commands and browser window creation | Smaller browser window, often without tab strip and with limited location display | **Retained unchanged initially** | Keep Chromium popup policy and a trustworthy origin display. Test requested bounds, opener close, blocked popup recovery, fullscreen, auth, permissions, downloads, DevTools, and multiple monitors. |
| Installed web app windows | App launch creates `TYPE_APP` with an `AppBrowserController` or `WebAppBrowserController`, app identity, special toolbar, and optional window-controls overlay | `chrome/browser/ui/web_applications/app_browser_controller.h`, `AppBrowserController`; `chrome/browser/ui/web_applications/web_app_browser_controller.h`, `WebAppBrowserController` | App-specific native frame and toolbar | **Retained unchanged initially** | Do not apply the normal shell until installed-app product scope is decided. Preserve app identity, scope, navigation affordance, link capture, window-controls overlay, permissions, install/update, and session restore. |
| App popup windows | Installed app opens a popup that behaves like an app but has popup appearance | `chrome/browser/ui/browser_window/public/browser_window_interface.h`, `BrowserWindowInterface::TYPE_APP_POPUP`; app browser controllers | Restricted app secondary window | **Retained unchanged initially** | Route separately from normal popup and normal app windows. Verify app identity and opener lifecycle. |
| Legacy packaged Chrome Apps and their windows | Extension platform app launches extension-owned windows | `extensions/browser/app_window/app_window.h`, `extensions::AppWindow`; platform app launch infrastructure under `chrome/browser/apps/platform_apps/` | Extension app window with platform-app APIs | **Intentionally unsupported, product approval required** | This is a proposed scope decision, not current approval. If unsupported, startup, restore, extension install, policy-installed apps, and user messaging need explicit behavior. If supported, keep Chromium app-window machinery unchanged. |
| Secondary settings, sign-in, reauth, and utility windows | Profile and enterprise features create standalone WebUI or Views windows that may not have a normal tab model | Controllers under `chrome/browser/ui/signin/`, `chrome/browser/ui/views/profiles/`, and feature directories | Standalone native window hosting WebUI or Views | **Retained unchanged initially** | The window router needs a safe fallback for unknown non-normal window types. It must not assert that every surface has a visible tab strip, omnibox, or active tab. |

## Anchoring and hosting risks found in source

### Direct `BrowserView` and `LocationBar` dependencies

The permission prompt factory is the clearest hard dependency. In
`chrome/browser/ui/views/permissions/permission_prompt_factory.cc`:

- `GetBrowser()` maps `WebContents` through `tabs::TabInterface` or WebUI
  embedding context.
- `IsFullScreenMode()` obtains `BrowserView` and `LocationBar`.
- `ShouldIgnorePermissionRequest()` checks whether the location bar is editing
  or empty and queries a Lens overlay controller through the active tab.
- quiet prompt selection tests whether the location bar is drawn.
- microphone prompt creation calls `LocationBar::SetPermissionPromptShowing`
  to protect omnibox focus behavior.

This means "retain permissions" is not achieved by keeping
`PermissionRequestManager` alone. The shell must either retain a compatible
native location-bar/prompt host or add a narrow prompt-host seam at
`ChromePermissionsClient::CreatePrompt` while preserving those behavioral
checks.

Other anchored surfaces such as Page Info, password, translate, Autofill,
intent picker, downloads, extensions, media, and crash restore commonly expect
a toolbar or location-bar anchor. Several accept generic Views anchors or can
be centered, but each must be verified. The shell needs a shared policy:

```text
preferred Limitless anchor exists -> anchor Chromium surface there
no meaningful anchor              -> use reviewed centered/window fallback
security identity surface         -> keep a persistent trusted entry point
fullscreen                        -> use surface-specific exclusive-access path
```

### Modal host completeness

`BrowserWindowInterface` exposes separate WebContents-modal hosts for the
window and a tab. `tabs::TabInterface` also arbitrates tab-modal UI. A new shell
must implement both layout hosting and lifetime coordination before payment,
FedCM, authentication, JavaScript dialogs, print preview, extension dialogs,
and other constrained surfaces can be considered retained.

### Window-type dispatch

Normal, popup, app, DevTools, app-popup, and document-PiP windows have different
chrome, navigation, and frame requirements. A replacement factory that returns
one normal Limitless view for every `BrowserWindowInterface::Type` would break
security identity, app behavior, DevTools, and PiP restrictions. Unknown or
unsupported types must fail clearly or fall back to Chromium's existing window
implementation.

## Extension-specific requirements

If extensions remain in scope, the shell must provide all of the following
before hiding Chromium's extension UI:

1. A discoverable action list sourced from `ToolbarActionsModel`.
2. Pinned, force-pinned, policy-disabled, incognito, and restricted-URL state.
3. Direct-user-gesture invocation through `ExtensionActionRunner`.
4. Popup hosting through `ExtensionViewHost`, including keyboard, file chooser,
   resize, navigation, unload, JavaScript dialog, and DevTools behavior.
5. Site-access and withheld-permission flows, including page reload prompts.
6. Install, update-permission, external-install, uninstall, disabled, and
   supervised-user prompts.
7. Options pages, extension management, command shortcuts, context-menu items,
   side panels, and DevTools inspection.
8. Secondary extension/app windows or an explicitly approved unsupported
   policy.

An extension toolbar button alone is not extension support.

## Platform-specific obligations

- **macOS:** retain application menus, traffic-light/native-fullscreen
  relationships, notification attribution for web apps, global find pasteboard,
  native file and print sheets, system permission prompts, and correct parent
  window behavior.
- **Windows after v1:** retain system/window menu commands, native file and certificate
  dialogs, notification bridge, task-manager platform metrics, authentication,
  and mixed-DPI parent-window placement.
- **Linux after v1:** retain native or portal-backed file dialogs, desktop notification
  behavior, window-manager conventions, certificate/auth dialogs, fractional
  scaling, and software-rendering fallback.

For v1 macOS, test keyboard-only operation, VoiceOver naming and reading order,
IME and composed input, RTL, localization expansion, increased contrast,
reduced motion, minimum size, focus restoration, and multi-monitor placement.
Equivalent tests are deferred for Linux and Windows.

## Product decisions still required

1. Whether legacy packaged Chrome Apps are supported. This inventory proposes
   unsupported status only if the user approves it.
2. Whether installed web app windows keep Chromium's app chrome indefinitely
   or later receive a separate Limitless app-window design.
3. Whether the long-term settings, history, bookmarks, downloads, extensions,
   profile picker, and first-run experiences are Limitless-owned or intentionally
   remain Chromium WebUI.
4. Whether extension side panels, extension apps, developer mode, unpacked
   extensions, and enterprise-forced extensions are in the first supported
   release.
5. Whether native OS notifications and share sheets carry Limitless or
   Chromium product branding, subject to build and platform mechanisms.

## Known unknowns and required validation

| Unknown | Smallest conclusive validation |
| --- | --- |
| Which retained bubbles can anchor to an arbitrary Limitless Views control? | In a prototype normal window, open Page Info, permissions, password save, Autofill save, translate, intent picker, downloads, extensions, sharing, media, and crash restore from explicit Limitless anchors. Record any direct toolbar/location-bar lookup. |
| Can every tab-modal flow host correctly in the replacement contents layout? | Exercise JavaScript dialog, HTTP auth, Payment Request, FedCM modal, print preview, extension dialog, and client certificate selection while switching tabs, resizing, entering fullscreen, and closing the window. |
| Does the permission factory need an upstream hook? | Trigger loud geolocation, quiet notification, camera, microphone while editing the omnibox, embedded prompt, PWA notification on macOS, fullscreen, hidden tab, and Lens overlay. Compare decisions and lifecycle with stock Chromium. |
| Are all secondary browser types routed? | Instrument browser-window creation, then launch normal, popup, installed app, app popup, DevTools, document PiP, profile picker, and restored variants. Assert an explicit route or reviewed fallback for each. |
| Do extension popups work without Chromium's toolbar container? | Invoke click-only and popup actions, withheld activeTab, reload-required script, file chooser, popup resize, popup DevTools, unload, tab switch, and window close from a Limitless action control. |
| Can Chromium WebUI pages run inside the normal Limitless tab host? | Open settings, history, downloads, bookmarks, extensions, view source, and internal error pages, then verify navigation, focus, accelerators, modal dialogs, file chooser, print, and accessibility. |
| Are capture and security indicators impossible to overlook? | Start audio, video, tab, window, and display capture in active and background tabs, enter PiP/fullscreen, move tabs between windows, discard a tab, and inspect visual and accessibility output. |
| Are native dialogs parented correctly on macOS? | Open upload, save page, download save-as, system print, client certificate, HTTP auth, screen capture, and System Settings handoffs on supported macOS versions, including multi-monitor and Retina cases. Linux and Windows validation is deferred. |

## Gate 1 implication

The surface inventory supports a Limitless-owned main shell only if the
architecture deliberately retains a Chromium prompt host and secondary-window
fallback. The expected early product split is:

- Limitless owns normal-window layout, navigation affordances, tab/workspace
  presentation, command entry points, and selected stable controllers such as
  the abstract find bar and tab alert presentation.
- Chromium remains responsible for permission, authentication, certificate,
  download danger, print, payment, FedCM, interstitial, extension, capture,
  system-dialog, DevTools, app, popup, and recovery flows until each is replaced
  through a reviewed seam.
- Existing internal WebUI pages remain reachable during incremental delivery.
- Unknown and unsupported window types use an explicit Chromium fallback rather
  than silently receiving an incomplete normal shell.

If the chosen window seam cannot provide these hosts and fallbacks without a
broad patch series, the architecture should be reconsidered before Phase 1.
