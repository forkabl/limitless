# Limitless Architecture

Limitless is a native macOS browser chrome embedded in the Chromium `chrome` target.
The boundary is intentionally simple: Chromium owns browser primitives, while
Limitless owns the native chrome and the bridge layer that asks Chromium to act.

## Runtime Layers

```text
SwiftUI chrome
  RootView, Sidebar, Toolbar, LauncherOverlay, panels, themes, stores

Objective-C++ bridge
  limitless_chrome_bridge.mm, limitless_browser_window.mm, extension and permission glue

Chromium browser stack
  Browser, WebContents, tabs, navigation, renderers, extensions, downloads
```

## Ownership Rules

- Swift files under `chrome/browser/ui/limitless` own visible Limitless UI, local app
  state, shortcut routing, settings, and feature models.
- Objective-C++ files under the same directory translate Limitless actions into
  Chromium browser commands and forward Chromium state back into Swift.
- Chromium continues to own page loading, renderer lifecycle, permissions,
  downloads, extensions, session primitives, and web text input.
- The bridge should stay thin. If logic is UI state or product behavior, keep it
  in Swift. If logic requires Chromium browser objects, keep it in Objective-C++.

## Main Data Flow

1. `limitless_browser_window.mm` installs Limitless into the Chromium browser window.
2. `LimitlessRoot.swift` creates the SwiftUI root view and the shared `BrowserStore`.
3. SwiftUI views mutate `BrowserStore` in response to user actions.
4. `limitless_chrome_bridge.mm` converts those actions into Chromium operations.
5. Bridge callbacks update Swift stores so the chrome reflects Chromium state.

## Keyboard Flow

Keyboard handling is shared between AppKit and Chromium web-view pre-handling:

- `ShortcutRegistry.swift` defines the single command routing table.
- `LimitlessRoot.swift` exposes Objective-C entry points for shortcut handling.
- `limitless_browser_window.mm` forwards web-focused key events before Chromium
  consumes them.
- Accepted shortcuts force a SwiftUI redraw pulse so state changes become
  visible immediately under Chromium's macOS message pump.

## Feature Boundaries

- Tabs, contexts, folders, sleep, and archive state: `BrowserStore.swift`,
  `BrowserTab.swift`, `Contexts.swift`, `TabFolder.swift`,
  `TabMaintenance.swift`, and `ArchiveStore.swift`.
- Command palette and shortcuts: `LauncherOverlay.swift` and
  `ShortcutRegistry.swift`.
- AI panel and page automation: `AIPanel.swift`, `CodexAppServerClient.swift`,
  and `BrowserAutomation.swift`.
- Per-site customization: `Boosts.swift`, `BoostStore.swift`, `Reader.swift`,
  `Peek.swift`, `AirTrafficControl.swift`, and `RouteStore.swift`.
- Chromium integration: `limitless_chrome_bridge.mm`, `limitless_browser_window.mm`,
  `limitless_chrome_extensions.mm`, `limitless_permission_prompt.mm`, and bridge headers.

## Structural Notes

The Limitless source directory is currently flat because Chromium's GN target lists
Swift sources explicitly in `chrome/browser/ui/BUILD.gn`. Moving files into
subdirectories is possible, but should be done as a separate build-verified
change that updates the GN source list at the same time.
