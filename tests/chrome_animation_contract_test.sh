#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
overlay="$repo_root/ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless"
integration_patch="$repo_root/patches/chromium/limitless-integration.patch"

assert_contains() {
  pattern=$1
  file=$2
  if ! rg -q -F "$pattern" "$file"; then
    echo "error: expected '$pattern' in $file" >&2
    exit 1
  fi
}

assert_absent() {
  pattern=$1
  file=$2
  if rg -q -F "$pattern" "$file"; then
    echo "error: unexpected '$pattern' in $file" >&2
    exit 1
  fi
}

assert_contains 'bool RouteChromeShortcut(NSEvent* event);' \
  "$overlay/limitless_chrome_hooks.h"
assert_contains 'dispatch_async(dispatch_get_main_queue(), ^{' \
  "$overlay/limitless_chrome_bridge.mm"
assert_contains 'return limitless::RouteChromeShortcut(ns_event);' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'static func canHandle(_ event: NSEvent, store: BrowserStore)' \
  "$overlay/ShortcutRegistry.swift"

assert_absent 'flushChrome' "$overlay/LimitlessRoot.swift"
assert_absent 'NSEvent.otherEvent' "$overlay/LimitlessRoot.swift"
assert_absent 'scheduleFlushPulse' "$overlay/LimitlessRoot.swift"

assert_contains 'final class ChromeTransitionDriver: ObservableObject' \
  "$overlay/ChromeTransitionDriver.swift"
assert_contains 'accessibilityDisplayShouldReduceMotion' \
  "$overlay/ChromeTransitionDriver.swift"
assert_contains 'objectWillChange.send()' \
  "$overlay/ChromeTransitionDriver.swift"
assert_contains 'func scheduleViewUpdates(for duration: TimeInterval = 0.35)' \
  "$overlay/ChromeTransitionDriver.swift"
assert_contains 'RunLoop.main.add(timer, forMode: .common)' \
  "$overlay/ChromeTransitionDriver.swift"
assert_contains 'ChromeTransitionDriver.shared.scheduleViewUpdates()' \
  "$overlay/ShortcutRegistry.swift"
assert_contains 'NSApp.postEvent(event, atStart: false)' \
  "$overlay/ChromeTransitionDriver.swift"
assert_contains 'let _ = chromeTransitions.activeTransition' \
  "$overlay/RootView.swift"
assert_contains 'final class WebContentsResizeCoordinator' \
  "$overlay/WebContentsResizeCoordinator.swift"
assert_contains 'let targets = browserSubviews.compactMap' \
  "$overlay/WebContainerView.swift"
assert_contains 'resizeCoordinator.updateTargets(' "$overlay/WebContainerView.swift"
assert_contains 'requestViewportSize(' "$overlay/WebContentsResizeCoordinator.swift"
assert_contains 'setViewportPresentationTransformActive(true)' \
  "$overlay/WebContentsResizeCoordinator.swift"
assert_contains '_pendingResize = request;' "$overlay/limitless_chrome_bridge.mm"
assert_contains 'render_view->ObserveViewportResize(' \
  "$overlay/limitless_chrome_bridge.mm"
assert_absent 'commitViewportLayout' "$overlay/WebContainerView.swift"
assert_absent 'layoutSubtreeIfNeeded' "$overlay/limitless_chrome_bridge.mm"

assert_contains 'limitless/ChromeTransitionDriver.swift' "$integration_patch"
assert_contains 'limitless/WebContentsResizeCoordinator.swift' "$integration_patch"
assert_contains 'ObserveViewportResize(' "$integration_patch"
assert_contains 'setViewportLogicalSize:' "$integration_patch"
assert_contains 'QuartzCore.framework' "$integration_patch"
assert_contains 'ScreenCaptureKit.framework' "$integration_patch"
assert_contains 'SCScreenshotManager.captureImage' "$overlay/Screenshot.swift"
assert_absent 'CGWindowListCreateImage' "$overlay/Screenshot.swift"
assert_contains "Browser shells without Chromium's SidePanelCoordinator" \
  "$integration_patch"
assert_contains 'TearDownPreBrowserWindowDestruction();' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'browser_->tab_strip_model()->empty() && !features_torn_down_' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'browser_->OnWindowClosing();' \
  "$overlay/limitless_browser_window.mm"
assert_absent 'SynchronouslyDestroyBrowser();' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'PrepareLimitlessBrowserForDestruction(browser_);' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'NSView* content = limitless::LimitlessMainWindow().contentView;' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'return GetRestoredBounds().size();' \
  "$overlay/limitless_browser_window.mm"
assert_contains 'RegisterOrphanWebContents("limitless-retired://startup", blank);' \
  "$overlay/limitless_chrome_bridge.mm"
assert_contains 'class JavaScriptResultCompletion' \
  "$overlay/limitless_chrome_bridge.mm"
assert_contains 'Page changed before script evaluation completed' \
  "$overlay/limitless_chrome_bridge.mm"
assert_absent 'for tab in tabs { tab.close() }' "$overlay/BrowserStore.swift"

echo "Chrome animation contract tests passed."
