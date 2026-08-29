import SwiftUI
import AppKit

/// Bridge object the ObjC++ AppDelegate calls to build and own the SwiftUI
/// chrome. Holds the single shared BrowserStore for the window.
@objc(LimitlessRoot)
final class LimitlessRoot: NSObject {
    /// Retained for the app lifetime so the store/tabs aren't deallocated.
    private static var shared: LimitlessRoot?

    let store = BrowserStore()

    @objc static func makeRootViewController() -> NSViewController {
        let root = LimitlessRoot()
        shared = root

        let hosting = NSHostingController(rootView: RootView(store: root.store))
        hosting.view.frame = NSRect(x: 0, y: 0, width: 1280, height: 820)
        return hosting
    }

    @MainActor
    @objc static func prepareForTermination() {
        shared?.store.prepareForTermination()
    }

    @objc static func shouldAutoFocusWebContent() -> Bool {
        shared?.store.shouldAutoFocusWebContent ?? false
    }

    @objc static func canHandleShortcutEvent(_ event: NSEvent) -> Bool {
        guard let store = shared?.store else { return false }
        return LimitlessCommands.canHandle(event, store: store)
    }

    @objc static func performShortcutEvent(_ event: NSEvent) {
        guard let store = shared?.store else { return }
        _ = LimitlessCommands.handle(event, store: store)
    }

    @objc(isReservedShortcutKeyEquivalent:modifierMask:)
    static func isReservedShortcut(keyEquivalent: String, modifierMask: UInt) -> Bool {
        LimitlessCommands.reservesChromiumShortcut(keyEquivalent: keyEquivalent,
                                              modifierMask: modifierMask)
    }

    @objc(handleShortcutWithKeyCode:charactersIgnoringModifiers:modifierMask:)
    static func handleShortcut(keyCode: UInt16,
                               charactersIgnoringModifiers: String?,
                               modifierMask: UInt) -> Bool {
        handleShortcut(keyCode: keyCode,
                       charactersIgnoringModifiers: charactersIgnoringModifiers,
                       modifierMask: modifierMask,
                       isRepeat: false)
    }

    @objc(handleShortcutWithKeyCode:charactersIgnoringModifiers:modifierMask:isRepeat:)
    static func handleShortcut(keyCode: UInt16,
                               charactersIgnoringModifiers: String?,
                               modifierMask: UInt,
                               isRepeat: Bool) -> Bool {
        guard Thread.isMainThread else {
            var handled = false
            DispatchQueue.main.sync {
                handled = handleShortcut(keyCode: keyCode,
                                         charactersIgnoringModifiers: charactersIgnoringModifiers,
                                         modifierMask: modifierMask,
                                         isRepeat: isRepeat)
            }
            return handled
        }
        guard let store = shared?.store else { return false }
        return LimitlessCommands.handle(keyCode: keyCode,
                                        charactersIgnoringModifiers: charactersIgnoringModifiers,
                                        modifierMask: modifierMask,
                                        isRepeat: isRepeat,
                                        store: store)
    }

    // Menu-driven actions (called from the AppKit menu bar).
    // ⌘T / File ▸ New Tab toggles the launcher (command palette) rather than
    // silently spawning a blank tab.
    @objc static func newTab() { shared?.store.toggleLauncher() }
    @objc static func dismissLauncherIfVisible() -> Bool {
        guard let store = shared?.store, store.launcherVisible else { return false }
        store.dismissLauncher()
        return true
    }
    @objc(openNewTabWithURL:)
    static func openNewTab(url: String) {
        shared?.store.newTab(url: url.isEmpty ? "about:blank" : url, select: true)
    }
    @objc static func closeCurrentTab() {
        if let id = shared?.store.selectedTabID { shared?.store.closeTab(id) }
    }
    @objc static func reopenClosedTab() { shared?.store.reopenClosedTab() }
    @objc static func reload() { shared?.store.reload() }
    @objc static func forceReload() { shared?.store.reloadIgnoringCache() }
    @objc static func stop() { shared?.store.stop() }
    @objc static func goBack() { shared?.store.goBack() }
    @objc static func goForward() { shared?.store.goForward() }
    @objc static func goHome() { shared?.store.goHome() }
    @objc static func toggleSidebar() { shared?.store.toggleSidebar() }
    @objc static func toggleAIPanel() { shared?.store.toggleAIPanel() }
    @objc static func openSettings() { shared?.store.settingsVisible = true }
    @objc static func focusOmnibox() { shared?.store.presentLauncherForCurrentTab() }
    @objc static func zoomIn() { shared?.store.zoomIn() }
    @objc static func zoomOut() { shared?.store.zoomOut() }
    @objc static func resetZoom() { shared?.store.resetZoom() }
    @objc static func toggleFindBar() { shared?.store.toggleFindBar() }
    @objc static func findNext() { shared?.store.findNext(forward: true) }
    @objc static func findPrevious() { shared?.store.findNext(forward: false) }
    @objc static func toggleDevTools() { shared?.store.toggleDevTools() }
    @objc static func printPage() { shared?.store.printPage() }
    @objc static func shareCurrentPage() { shared?.store.share() }
    @objc static func showQRCodeForCurrentPage() { shared?.store.showQRCode() }
    @objc static func translateCurrentPage() { shared?.store.translate() }
    @objc static func selectNextTab() { shared?.store.selectNextTab() }
    @objc static func selectPreviousTab() { shared?.store.selectPreviousTab() }
    @objc(toggleBookmarkForURL:title:)
    static func toggleBookmark(url: String, title: String) {
        shared?.store.toggleBookmark(url: url, title: title)
    }
    @objc(shareURL:title:)
    static func share(url: String, title: String) {
        shared?.store.share(url: url, title: title)
    }
    @objc(showQRCodeForURL:title:)
    static func showQRCode(url: String, title: String) {
        shared?.store.showQRCode(url: url, title: title)
    }
    @objc(translateURL:)
    static func translate(url: String) {
        shared?.store.translate(url: url)
    }
    @objc(translateText:)
    static func translate(text: String) {
        shared?.store.translate(text: text)
    }
    @objc(showNativeNotice:icon:)
    static func showNativeNotice(message: String, icon: String) {
        ToastCenter.shared.show(message, icon: icon, style: .info)
    }
    @objc static func showTabSearch() {
        shared?.store.presentLauncher()
    }

}
