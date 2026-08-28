import AppKit
import QuartzCore

/// Holds the live Chromium viewport at stable geometry during native chrome
/// transitions and commits its final size once the transition settles.
final class WebContentsResizeCoordinator {
    private var activeTransitionID: UInt64?
    private var anchor: ChromeTransitionAnchor = .center
    private var frozenSize: CGSize = .zero

    var isHoldingGeometry: Bool { activeTransitionID != nil }

    /// Returns true when a transition just ended and visible WebContents should
    /// receive their final frames.
    @discardableResult
    func update(
        transition: ActiveChromeTransition?,
        viewportHost: NSView,
        containerBounds: CGRect
    ) -> Bool {
        if let transition {
            if activeTransitionID != transition.id {
                begin(
                    transition, viewportHost: viewportHost,
                    containerBounds: containerBounds)
            }
            updateTransform(viewportHost: viewportHost, containerBounds: containerBounds)
            return false
        }

        guard activeTransitionID != nil else { return false }
        finish(viewportHost: viewportHost, containerBounds: containerBounds)
        return true
    }

    func containerDidResize(viewportHost: NSView, containerBounds: CGRect) {
        guard isHoldingGeometry else { return }
        updateTransform(viewportHost: viewportHost, containerBounds: containerBounds)
    }

    private func begin(
        _ transition: ActiveChromeTransition,
        viewportHost: NSView,
        containerBounds: CGRect
    ) {
        activeTransitionID = transition.id
        anchor = transition.anchor

        if viewportHost.bounds.width > 0, viewportHost.bounds.height > 0 {
            frozenSize = viewportHost.bounds.size
        } else {
            viewportHost.frame = containerBounds
            frozenSize = containerBounds.size
        }
        viewportHost.wantsLayer = true
    }

    private func updateTransform(viewportHost: NSView, containerBounds: CGRect) {
        guard frozenSize.width > 0, frozenSize.height > 0,
            let layer = viewportHost.layer
        else { return }

        let scaleX = containerBounds.width / frozenSize.width
        let scaleY = containerBounds.height / frozenSize.height
        let anchorX: CGFloat
        switch anchor {
        case .leading: anchorX = 0
        case .trailing: anchorX = frozenSize.width
        case .center: anchorX = frozenSize.width / 2
        }
        let anchorY = frozenSize.height / 2

        var transform = CATransform3DIdentity
        transform = CATransform3DTranslate(transform, anchorX, anchorY, 0)
        transform = CATransform3DScale(transform, scaleX, scaleY, 1)
        transform = CATransform3DTranslate(transform, -anchorX, -anchorY, 0)

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer.sublayerTransform = transform
        CATransaction.commit()
    }

    private func finish(viewportHost: NSView, containerBounds: CGRect) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        viewportHost.layer?.sublayerTransform = CATransform3DIdentity
        viewportHost.frame = containerBounds
        CATransaction.commit()

        activeTransitionID = nil
        frozenSize = .zero
    }
}
