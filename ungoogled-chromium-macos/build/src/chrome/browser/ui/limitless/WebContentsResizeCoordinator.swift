import AppKit
import QuartzCore

/// Separates desired AppKit geometry from renderer-committed and successfully
/// presented Chromium geometry. The coordinator never forces layout in a
/// Chromium subtree.
final class WebContentsResizeCoordinator {
    struct Target {
        let view: LimitlessBrowserView
        let frame: CGRect
    }

    private var chromeActivityIsActive = false
    private var liveResizeIsActive = false
    private var presentationIsHeld = false
    private var needsTerminalRequest = false
    private var nextGeneration: UInt64 = 0
    private var awaitingGeneration: UInt64?
    private var remainingTerminalViews: Set<ObjectIdentifier> = []
    private var latestTargets: [Target] = []
    private var terminalTargets: [Target] = []
    private var terminalContainerBounds: CGRect = .zero
    private var lastNonterminalSizes: [ObjectIdentifier: CGSize] = [:]
    private var frozenViewportSize: CGSize = .zero
    private var latestContainerBounds: CGRect = .zero
    private weak var viewportHost: NSView?

    var isHoldingGeometry: Bool { presentationIsHeld }

    deinit {
        for target in latestTargets {
            target.view.bounds = CGRect(origin: .zero, size: target.view.frame.size)
            target.view.setViewportPresentationTransformActive(false)
            target.view.cancelViewportResize()
        }
    }

    func updateChromeActivity(
        transition: ActiveChromeTransition?,
        manualResize: Bool,
        viewportHost: NSView,
        containerBounds: CGRect
    ) {
        let isActive = transition != nil || manualResize
        if isActive && !chromeActivityIsActive {
            beginPresentationHold(
                viewportHost: viewportHost, containerBounds: containerBounds)
        } else if !isActive && chromeActivityIsActive {
            needsTerminalRequest = true
        }
        chromeActivityIsActive = isActive
        latestContainerBounds = containerBounds
        updatePresentationGeometry(
            viewportHost: viewportHost, containerBounds: containerBounds)
    }

    func liveResizeDidStart(viewportHost: NSView, containerBounds: CGRect) {
        liveResizeIsActive = true
        beginPresentationHold(
            viewportHost: viewportHost, containerBounds: containerBounds)
    }

    func liveResizeDidEnd() {
        guard liveResizeIsActive else { return }
        liveResizeIsActive = false
        needsTerminalRequest = true
    }

    func containerDidResize(viewportHost: NSView, containerBounds: CGRect) {
        latestContainerBounds = containerBounds
        updatePresentationGeometry(
            viewportHost: viewportHost, containerBounds: containerBounds)
    }

    func updateTargets(
        _ targets: [Target],
        viewportHost: NSView,
        containerBounds: CGRect
    ) {
        let oldViews = Dictionary(
            uniqueKeysWithValues: latestTargets.map {
                (ObjectIdentifier($0.view), $0.view)
            })
        let newViewIDs = Set(targets.map { ObjectIdentifier($0.view) })
        for (identifier, view) in oldViews where !newViewIDs.contains(identifier) {
            view.bounds = CGRect(origin: .zero, size: view.frame.size)
            view.setViewportPresentationTransformActive(false)
            view.cancelViewportResize()
            lastNonterminalSizes.removeValue(forKey: identifier)
        }

        latestTargets = targets
        latestContainerBounds = containerBounds
        self.viewportHost = viewportHost

        var coordinatedTargets: [Target] = []
        for target in targets {
            if target.view.frame.isEmpty {
                initialize(target)
            } else {
                coordinatedTargets.append(target)
            }
        }

        if chromeActivityIsActive {
            beginPresentationHold(
                viewportHost: viewportHost, containerBounds: containerBounds)
            return
        }

        if liveResizeIsActive {
            beginPresentationHold(
                viewportHost: viewportHost, containerBounds: containerBounds)
            submit(coordinatedTargets, terminal: false)
            return
        }

        let geometryChanged = coordinatedTargets.contains {
            !framesApproximatelyEqual($0.view.frame, $0.frame)
        } || !sizesApproximatelyEqual(viewportHost.bounds.size, containerBounds.size)

        if awaitingGeneration != nil,
           targetsApproximatelyEqual(coordinatedTargets, terminalTargets),
           framesApproximatelyEqual(containerBounds, terminalContainerBounds) {
            return
        }

        if needsTerminalRequest || geometryChanged {
            beginPresentationHold(
                viewportHost: viewportHost, containerBounds: containerBounds)
            if coordinatedTargets.isEmpty || !geometryChanged {
                finishPresentation(
                    targets: coordinatedTargets,
                    viewportHost: viewportHost,
                    containerBounds: containerBounds)
            } else {
                submit(coordinatedTargets, terminal: true)
            }
        }
    }

    private func initialize(_ target: Target) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        target.view.frame = target.frame
        target.view.bounds = CGRect(origin: .zero, size: target.frame.size)
        target.view.presentCommittedViewportSize(target.frame.size)
        CATransaction.commit()
    }

    private func beginPresentationHold(
        viewportHost: NSView,
        containerBounds: CGRect
    ) {
        guard !presentationIsHeld else {
            for target in latestTargets {
                target.view.setViewportPresentationTransformActive(true)
            }
            updatePresentationGeometry(
                viewportHost: viewportHost, containerBounds: containerBounds)
            return
        }
        frozenViewportSize = viewportHost.bounds.size
        if frozenViewportSize.width <= 0 || frozenViewportSize.height <= 0 {
            frozenViewportSize = containerBounds.size
        }
        presentationIsHeld = true
        viewportHost.wantsLayer = true
        viewportHost.layer?.masksToBounds = true
        for target in latestTargets {
            target.view.setViewportPresentationTransformActive(true)
        }
        updatePresentationGeometry(
            viewportHost: viewportHost, containerBounds: containerBounds)
    }

    private func updatePresentationGeometry(
        viewportHost: NSView,
        containerBounds: CGRect
    ) {
        guard presentationIsHeld,
              frozenViewportSize.width > 0,
              frozenViewportSize.height > 0
        else { return }

        // A view frame-to-bounds transform participates in AppKit point
        // conversion and accessibility geometry. A CALayer sublayerTransform
        // would move pixels while leaving Chromium input coordinates stale.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        viewportHost.frame = containerBounds
        viewportHost.bounds = CGRect(origin: .zero, size: frozenViewportSize)
        for target in latestTargets {
            // The inner WebContents advances to the desired logical size while
            // this wrapper remains in its last presented frame. Its
            // frame-to-bounds transform cancels the viewport-host presentation
            // scale for coordinates and lets Chromium resize off-screen.
            target.view.bounds = CGRect(origin: .zero, size: target.frame.size)
        }
        CATransaction.commit()
    }

    private func submit(_ targets: [Target], terminal: Bool) {
        let submittedTargets: [Target]
        if terminal {
            submittedTargets = targets
        } else {
            submittedTargets = targets.filter {
                let identifier = ObjectIdentifier($0.view)
                return !sizesApproximatelyEqual(
                    lastNonterminalSizes[identifier] ?? .zero, $0.frame.size)
            }
        }
        guard !submittedTargets.isEmpty else { return }

        nextGeneration &+= 1
        let generation = nextGeneration
        if terminal {
            needsTerminalRequest = false
            awaitingGeneration = generation
            terminalTargets = targets
            terminalContainerBounds = latestContainerBounds
            remainingTerminalViews = Set(
                targets.map { ObjectIdentifier($0.view) })
        }

        for target in submittedTargets {
            let identifier = ObjectIdentifier(target.view)
            lastNonterminalSizes[identifier] = target.frame.size
            target.view.requestViewportSize(
                target.frame.size,
                generation: generation,
                terminal: terminal
            ) { [weak self, weak view = target.view] callbackGeneration, presented in
                guard terminal, view != nil, let self else { return }
                self.terminalRequestDidComplete(
                    viewID: identifier,
                    generation: callbackGeneration,
                    successfullyPresented: presented)
            }
        }
    }

    private func terminalRequestDidComplete(
        viewID: ObjectIdentifier,
        generation: UInt64,
        successfullyPresented: Bool
    ) {
        guard awaitingGeneration == generation else { return }
        guard successfullyPresented else {
            NSLog(
                "LIMITLESS_RESIZE terminal generation %llu entered degraded state",
                generation)
            return
        }

        remainingTerminalViews.remove(viewID)
        guard remainingTerminalViews.isEmpty, let viewportHost else { return }
        finishPresentation(
            targets: terminalTargets,
            viewportHost: viewportHost,
            containerBounds: latestContainerBounds)
    }

    private func finishPresentation(
        targets: [Target],
        viewportHost: NSView,
        containerBounds: CGRect
    ) {
        presentationIsHeld = false
        needsTerminalRequest = false
        awaitingGeneration = nil
        remainingTerminalViews.removeAll()
        terminalTargets.removeAll()
        terminalContainerBounds = .zero
        frozenViewportSize = .zero
        lastNonterminalSizes.removeAll()

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        viewportHost.frame = containerBounds
        viewportHost.bounds = CGRect(origin: .zero, size: containerBounds.size)
        for target in targets {
            target.view.frame = target.frame
            target.view.bounds = CGRect(origin: .zero, size: target.frame.size)
            target.view.presentCommittedViewportSize(target.frame.size)
            target.view.setViewportPresentationTransformActive(false)
        }
        CATransaction.commit()
    }

    private func framesApproximatelyEqual(_ left: CGRect, _ right: CGRect) -> Bool {
        abs(left.minX - right.minX) < 0.01
            && abs(left.minY - right.minY) < 0.01
            && sizesApproximatelyEqual(left.size, right.size)
    }

    private func sizesApproximatelyEqual(_ left: CGSize, _ right: CGSize) -> Bool {
        abs(left.width - right.width) < 0.01
            && abs(left.height - right.height) < 0.01
    }

    private func targetsApproximatelyEqual(_ left: [Target], _ right: [Target]) -> Bool {
        guard left.count == right.count else { return false }
        let rightFrames = Dictionary(
            uniqueKeysWithValues: right.map {
                (ObjectIdentifier($0.view), $0.frame)
            })
        return left.allSatisfy {
            guard let rightFrame = rightFrames[ObjectIdentifier($0.view)] else {
                return false
            }
            return framesApproximatelyEqual($0.frame, rightFrame)
        }
    }
}
