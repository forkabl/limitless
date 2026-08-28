import AppKit
import SwiftUI

/// The geometry changes that can alter the live Chromium viewport.
enum ChromeTransitionKind: String {
    case sidebar
    case sidebarPosition
    case aiPanel
    case extensionPanel
    case splitView
}

/// The browser edge that should stay visually fixed during a transition.
enum ChromeTransitionAnchor: Equatable {
    case leading
    case trailing
    case center
}

struct ActiveChromeTransition: Equatable {
    let id: UInt64
    let kind: ChromeTransitionKind
    let anchor: ChromeTransitionAnchor
}

/// Owns browser-chrome update scheduling and geometry-changing transitions.
///
/// SwiftUI describes the settled state. AppKit-backed web containers observe
/// `activeTransition` and keep Chromium at stable geometry while their native
/// layers animate. The final engine resize happens when the transition ends.
final class ChromeTransitionDriver: ObservableObject {
    static let shared = ChromeTransitionDriver()

    @Published private(set) var activeTransition: ActiveChromeTransition?

    private var nextID: UInt64 = 0
    private var startedAt: TimeInterval = 0
    private var earliestFinishAt: TimeInterval = 0
    private var deferredFinishID: UInt64?
    private var frameTimer: Timer?
    private var clockStopGeneration: UInt64 = 0
    private let frameSettleDuration: TimeInterval = 0.1
    private let performanceLoggingEnabled =
        ProcessInfo.processInfo.environment["LIMITLESS_CHROME_PERF_LOG"] == "1"

    private init() {}

    /// Keep SwiftUI's normal AppKit update phases moving after any external
    /// chrome mutation. Geometry transitions call this internally; shortcut
    /// and native-overlay paths use it without holding Chromium geometry.
    func scheduleViewUpdates(for duration: TimeInterval = 0.35) {
        precondition(Thread.isMainThread)
        objectWillChange.send()
        startFrameClock()
        scheduleFrameClockStop(after: duration)
    }

    func perform(
        kind: ChromeTransitionKind,
        anchor: ChromeTransitionAnchor,
        animation: Animation,
        fallbackDuration: TimeInterval,
        changes: () -> Void
    ) {
        precondition(Thread.isMainThread)

        nextID &+= 1
        let transition = ActiveChromeTransition(id: nextID, kind: kind, anchor: anchor)
        activeTransition = transition
        // @Published announces before assignment. Chromium's macOS message
        // pump can reconcile that pass immediately, so announce once more
        // after the transition is committed.
        objectWillChange.send()
        startedAt = ProcessInfo.processInfo.systemUptime
        let reducedMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        earliestFinishAt = startedAt + (reducedMotion ? 1.0 / 60.0 : fallbackDuration)

        if performanceLoggingEnabled {
            NSLog(
                "LIMITLESS_PERF transition-begin kind=%@ id=%llu reduced_motion=%d",
                kind.rawValue, transition.id, reducedMotion ? 1 : 0)
        }

        startFrameClock()
        if reducedMotion {
            var transaction = Transaction()
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                changes()
                objectWillChange.send()
            }
            finish(transition)
            return
        }

        withAnimation(animation, completionCriteria: .logicallyComplete) {
            changes()
            // This post-mutation notification lets the root read committed
            // product state in the same animation transaction.
            objectWillChange.send()
        } completion: { [weak self] in
            self?.finish(transition)
        }

        // A view can disappear before SwiftUI reports animation completion.
        // This bounded fallback releases frozen Chromium geometry without
        // driving frames or forcing redraws.
        DispatchQueue.main.asyncAfter(deadline: .now() + fallbackDuration + 0.1) {
            [weak self] in
            self?.finish(transition)
        }
    }

    private func finish(_ transition: ActiveChromeTransition) {
        guard activeTransition == transition else { return }

        let remaining = earliestFinishAt - ProcessInfo.processInfo.systemUptime
        if remaining > 0 {
            guard deferredFinishID != transition.id else { return }
            deferredFinishID = transition.id
            DispatchQueue.main.asyncAfter(deadline: .now() + remaining) { [weak self] in
                guard let self else { return }
                if self.deferredFinishID == transition.id {
                    self.deferredFinishID = nil
                }
                self.finish(transition)
            }
            return
        }

        activeTransition = nil
        objectWillChange.send()
        deferredFinishID = nil
        scheduleFrameClockStop(after: frameSettleDuration)

        if performanceLoggingEnabled {
            let elapsedMS = (ProcessInfo.processInfo.systemUptime - startedAt) * 1_000
            NSLog(
                "LIMITLESS_PERF transition-end kind=%@ id=%llu duration_ms=%.2f",
                transition.kind.rawValue, transition.id, elapsedMS)
        }
    }

    /// Chromium owns the outer macOS message pump, so a SwiftUI transaction
    /// started by browser chrome can otherwise remain parked until the next
    /// AppKit event. A bounded RunLoop timer posts inert wake events
    /// that give SwiftUI its normal update phases without forcing view redraws.
    private func startFrameClock() {
        clockStopGeneration &+= 1
        frameTimer?.invalidate()
        let timer = Timer(timeInterval: 1.0 / 60.0, repeats: true) { _ in
            Self.wakeAppKit()
        }
        timer.tolerance = 1.0 / 240.0
        RunLoop.main.add(timer, forMode: .common)
        frameTimer = timer
        Self.wakeAppKit()
    }

    /// Releasing held Chromium geometry requires one additional SwiftUI/AppKit
    /// reconciliation after logical animation completion. Keep the same clock
    /// alive briefly so the settled native frame cannot wait for user input.
    private func scheduleFrameClockStop(after duration: TimeInterval) {
        clockStopGeneration &+= 1
        let generation = clockStopGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + duration) {
            [weak self] in
            guard let self,
                  self.clockStopGeneration == generation else { return }
            self.frameTimer?.invalidate()
            self.frameTimer = nil
            Self.wakeAppKit()
        }
    }

    private static func wakeAppKit() {
        guard
            let event = NSEvent.otherEvent(
                with: .applicationDefined,
                location: .zero,
                modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: 0,
                context: nil,
                subtype: 0,
                data1: 0,
                data2: 0)
        else { return }
        NSApp.postEvent(event, atStart: false)
    }
}
