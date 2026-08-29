import SwiftUI
import AppKit

/// Hosts the live CEF browser views. All realized tabs stay mounted (so they
/// keep running like real background tabs); only the selected one is visible.
struct WebContainerView: NSViewRepresentable {
    @ObservedObject var store: BrowserStore
    @ObservedObject var activeTab: BrowserTab
    @ObservedObject var chromeTransitions: ChromeTransitionDriver
    /// Corner radius applied to the container's layer so the live CEF content is
    /// clipped to the rounded "card" - SwiftUI `.clipShape` can't clip a hosted
    /// AppKit view, so the rounding has to happen on the layer itself.
    var cornerRadius: CGFloat = 0

    func makeNSView(context: Context) -> ContainerView {
        let view = ContainerView()
        view.applyCornerRadius(cornerRadius)
        return view
    }

    func updateNSView(_ nsView: ContainerView, context: Context) {
        let activeLoadFailed = activeTab.didFail
        nsView.applyCornerRadius(cornerRadius)

        // Geometry-changing chrome holds Chromium at a stable presented size.
        // The final native geometry is revealed only after Chromium confirms
        // successful presentation for every visible pane.
        nsView.updateGeometryTransition(
            chromeTransitions.activeTransition,
            manualResize: store.isResizingSidebar)

        // Settings - and an agent thread - render as a full page over the web
        // card, so hide Chromium while either is up. The launcher is hosted in
        // an AppKit overlay above the web view and leaves the page visible
        // behind its scrim.
        let agentForeground = store.selectedTab?.kind == .agent
        LimitlessBrowserView.setWebContentSuppressed(store.settingsVisible || agentForeground)

        // Realize the selected *web* tab only - an agent tab has no CEF view and
        // must never be forced to create one.
        store.selectedWebTab?.realize()

        let realizedTabs = store.tabs.filter { $0.hasRealized }
        let liveViews = realizedTabs.map { $0.browserView }

        // Remove views whose tabs are gone.
        for sub in nsView.browserSubviews where !(liveViews.contains { $0 === sub }) {
            sub.removeFromSuperview()
        }

        // Split view: the selected tab and the split tab render side-by-side.
        let splitID = store.splitTabID
        nsView.splitLayout = nil
        if let splitID, let splitTab = realizedTabs.first(where: { $0.id == splitID }),
           let primary = realizedTabs.first(where: { $0.id == store.selectedTabID }),
           splitTab.id != primary.id {
            let left = store.splitSide == .left ? splitTab.browserView
                                                : primary.browserView
            let right = store.splitSide == .left ? primary.browserView
                                                 : splitTab.browserView
            nsView.splitLayout = (left: left, right: right)
        }

        // Add, position, and set visibility for current tabs.
        for tab in realizedTabs {
            let view = tab.browserView
            if view.superview !== nsView.viewportHost {
                view.removeFromSuperview()
                nsView.addBrowserSubview(view)
            }
            let visible = tab.id == store.selectedTabID || (splitID != nil && tab.id == splitID)
            let hidden = !visible || tab.didFail
            view.autoresizingMask = []
            view.isHidden = hidden
            view.setWebWindowVisible(!hidden)
            // Drive Chromium's page-visibility so backgrounded tabs throttle and
            // (when enabled) auto-enter Picture-in-Picture on tab switch. While an
            // agent thread is foreground its target web tabs are off-screen but
            // must stay unthrottled so the agent's JS actions run reliably.
            view.setPageHidden(hidden && !agentForeground)
        }

        nsView.synchronizeBrowserGeometry()

        // Keep the active browser keyboard-focused.
        if let active = store.selectedTab, active.hasRealized {
            active.browserView.isHidden = activeLoadFailed
            active.browserView.setWebWindowVisible(!activeLoadFailed)
            if store.shouldAutoFocusWebContent,
               !activeLoadFailed,
               !Self.windowHasTextInputFocus(nsView.window) {
                DispatchQueue.main.async { [weak browserView = active.browserView,
                                            weak store,
                                            weak nsView] in
                    guard let browserView,
                          let store,
                          store.shouldAutoFocusWebContent,
                          !browserView.isHidden,
                          !Self.windowHasTextInputFocus(nsView?.window)
                    else { return }
                    browserView.focusBrowser()
                }
            }
        }
    }

    private static func windowHasTextInputFocus(_ window: NSWindow?) -> Bool {
        guard let responder = window?.firstResponder else { return false }
        return responder is NSTextView || responder is NSTextField
    }

    /// Flipped container so child frames use top-left origin.
    final class ContainerView: NSView {
        override var isFlipped: Bool { true }

        final class ViewportHost: NSView {
            override var isFlipped: Bool { true }
        }

        let viewportHost = ViewportHost()
        private let resizeCoordinator = WebContentsResizeCoordinator()

        var holdsGeometry: Bool { resizeCoordinator.isHoldingGeometry }

        var browserSubviews: [NSView] { viewportHost.subviews }

        /// Non-nil while a vertical split is active: the two panes, each laid
        /// out as half of the container with a thin gap between them.
        var splitLayout: (left: NSView, right: NSView)? {
            didSet { needsLayout = true }
        }

        static let splitGap: CGFloat = 8

        override init(frame frameRect: NSRect) {
            super.init(frame: frameRect)
            configureViewportHost()
        }

        required init?(coder: NSCoder) {
            super.init(coder: coder)
            configureViewportHost()
        }

        private func configureViewportHost() {
            viewportHost.frame = bounds
            viewportHost.autoresizingMask = []
            viewportHost.autoresizesSubviews = false
            addSubview(viewportHost)
        }

        func addBrowserSubview(_ view: NSView) {
            viewportHost.addSubview(view)
        }

        func updateGeometryTransition(_ transition: ActiveChromeTransition?,
                                      manualResize: Bool) {
            resizeCoordinator.updateChromeActivity(
                transition: transition,
                manualResize: manualResize,
                viewportHost: viewportHost,
                containerBounds: bounds)
        }

        func frameForSubview(_ view: NSView) -> NSRect {
            let targetBounds = NSRect(origin: .zero, size: bounds.size)
            guard let split = splitLayout else { return targetBounds }
            let half = (targetBounds.width - Self.splitGap) / 2
            if view === split.left {
                return NSRect(x: 0, y: 0, width: half, height: targetBounds.height)
            }
            if view === split.right {
                return NSRect(x: half + Self.splitGap, y: 0,
                              width: half, height: targetBounds.height)
            }
            return targetBounds
        }

        func synchronizeBrowserGeometry() {
            let targets = browserSubviews.compactMap { subview ->
                WebContentsResizeCoordinator.Target? in
                guard let browserView = subview as? LimitlessBrowserView,
                      !browserView.isHidden
                else { return nil }
                return WebContentsResizeCoordinator.Target(
                    view: browserView,
                    frame: frameForSubview(browserView))
            }
            resizeCoordinator.updateTargets(
                targets,
                viewportHost: viewportHost,
                containerBounds: bounds)
        }

        /// Round (and clip to) the layer so the hosted CEF subviews are masked
        /// to the card shape. `.continuous` matches SwiftUI's squircle corners.
        func applyCornerRadius(_ radius: CGFloat) {
            wantsLayer = true
            guard let layer else { return }
            if layer.cornerRadius != radius { layer.cornerRadius = radius }
            layer.cornerCurve = .continuous
            layer.masksToBounds = radius > 0
        }

        override func layout() {
            super.layout()
            layoutViewport()
        }

        override func setFrameSize(_ newSize: NSSize) {
            super.setFrameSize(newSize)
            layoutViewport()
        }

        override func viewWillStartLiveResize() {
            super.viewWillStartLiveResize()
            resizeCoordinator.liveResizeDidStart(
                viewportHost: viewportHost, containerBounds: bounds)
            synchronizeBrowserGeometry()
        }

        override func viewDidEndLiveResize() {
            super.viewDidEndLiveResize()
            resizeCoordinator.liveResizeDidEnd()
            synchronizeBrowserGeometry()
        }

        private func layoutViewport() {
            if resizeCoordinator.isHoldingGeometry {
                resizeCoordinator.containerDidResize(
                    viewportHost: viewportHost, containerBounds: bounds)
            } else {
                viewportHost.frame = bounds
                viewportHost.bounds = NSRect(origin: .zero, size: bounds.size)
            }
            synchronizeBrowserGeometry()
        }
    }
}
