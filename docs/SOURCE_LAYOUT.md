# Source Layout

This repository tracks a narrow first-party Limitless overlay. A full Chromium
checkout is restored outside the repository for builds.

## Repository Root

- `README.md` - project overview and entry point.
- `BUILDING.md` - canonical build, package, run, and logging workflow.
- `chromium.version` - exact Chromium revision required by integration checks.
- `config/limitless-product.json` - product, bundle, protocol, and storage
  identity.
- `assets/brand/` - authoritative full-color artwork and brand asset notes.
- `patches/chromium/` - integration changes applied to the pinned Chromium
  checkout.
- `scripts/` - integration, asset generation, release, and verification tools.
- `tests/` - source-level rebrand contract coverage.
- `docs/ARCHITECTURE.md` - runtime boundaries and data flow.
- `ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless` - tracked
  source overlay copied into a build checkout.

Build products, the Chromium checkout, and `depot_tools` are local dependencies
outside this repository. The tracked Limitless overlay is the source of truth
for app behavior.

## Limitless Overlay

Path:

```text
ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/
```

Do not edit older prototypes under `content/shell/browser/limitless`; they are not
the app that gets packaged.

There is also a local overlay guide at:

```text
ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless/README.md
```

## SwiftUI Chrome

- `LimitlessRoot.swift` - SwiftUI root, Objective-C entry points, and the shared
  app model exposed to the bridge.
- `RootView.swift`, `Sidebar.swift`, `Toolbar.swift`, `TabRow.swift`,
  `LauncherOverlay.swift` - browser chrome, tab surfaces, and command palette.
- `Components.swift`, `Glass.swift`, `Icon.swift`, `MorphingFolderIcon.swift`,
  `ToastCenter.swift`, `ToastOverlay.swift` - shared UI building blocks.
- `SettingsView.swift`, `ThemePicker.swift`, `Theme.swift`,
  `GradientTheme.swift`, `GradientEngine.swift`, `ThemePresets.swift`,
  `OKLCH.swift`, `FontRegistry.swift` - settings, tokens, and theming.

## App State

- `BrowserStore.swift` - top-level observable browser state.
- `BrowserStore+DragDrop.swift` - drag/drop behavior for sidebar organization.
- `BrowserTab.swift`, `BrowserContext.swift`, `Contexts.swift`,
  `TabFolder.swift` - tabs, spaces, and folder models.
- `BrowserSettings.swift`, `ArchiveStore.swift`, `BookmarkStore.swift`,
  `DownloadStore.swift`, `ExtensionStore.swift`, `HistoryStore.swift` -
  persisted app and browser-adjacent stores.

## Product Features

- `AIPanel.swift`, `CodexAppServerClient.swift`, `BrowserAutomation.swift` -
  AI panel and page-acting tools.
- `Boosts.swift`, `BoostStore.swift`, `Reader.swift`, `Peek.swift`,
  `SidebarPeek.swift`, `Screenshot.swift` - page customization and transient
  browsing tools.
- `AirTrafficControl.swift`, `RouteStore.swift` - host-based tab routing.
- `DownloadsPanel.swift`, `ExtensionsMenu.swift`, `LibraryPanel.swift`,
  `FindBar.swift`, `ContextMenu.swift` - browser panels and overlays.
- `MediaAgentScripts.swift`, `MediaController.swift`, `MediaPlayer.swift`,
  `MediaPolling.swift`, `PiPWindowStyler.swift` - media detection and controls.
- `PasskeyAuthenticator.swift`, `PasskeySupport.swift` - passkey support.
- `ShortcutRegistry.swift` - app command routing and reserved Chromium shortcut
  decisions.
- `TabMaintenance.swift` - tab sleep and archive timers.

## Chromium Bridge

- `limitless_bridge.h` - Swift bridging header.
- `limitless_chrome_bridge.mm` - main Swift/AppKit to Chromium integration layer.
- `limitless_browser_window.mm` and `limitless_browser_window.h` - browser window
  lifecycle and web-focused keyboard handling.
- `limitless_chrome_extensions.mm` - Chromium extension API integration.
- `limitless_permission_prompt.mm` and `limitless_permission_prompt.h` - permission UI
  integration.
- `LimitlessBrowserView.h`, `LimitlessPrivacy.h`, `limitless_chrome_hooks.h` - remaining
  bridge declarations.

## Build Wiring

The Swift files are compiled by the `limitless_ui_swift` target added by:

```text
patches/chromium/limitless-integration.patch
```

When adding, moving, or deleting Swift files, update the GN source list in that
patch in the same change.
