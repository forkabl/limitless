// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AppKit
import ApplicationServices
import CoreGraphics
import Darwin
import Foundation

private let railIdentifier = "limitless-swiftui-boundary-rail"
private let tabRowIdentifierPrefix = "limitless-tab-"
private let tabCloseIdentifierPrefix = "limitless-tab-close-"
private let maximumAccessibilityDepth = 64
private let maximumAccessibilityNodes = 20_000

private struct ProbeFailure: Error, CustomStringConvertible {
  let description: String
}

private struct Arguments {
  let pid: pid_t
  let screenshot: URL?
  let absentWindowID: CGWindowID?
  let timeout: TimeInterval
}

private struct NativeWindow {
  let bounds: CGRect
  let identifier: CGWindowID
  let ownerPID: pid_t
  let title: String
}

private struct BoundaryElements {
  let focus: AXUIElement
  let rail: AXUIElement
  let window: AXUIElement
}

private struct DisplayEvidence {
  let bounds: CGRect
  let identifier: CGDirectDisplayID
  let scale: CGFloat
}

private func parsePositiveInteger(_ value: String, flag: String) throws -> Int {
  guard let parsed = Int(value), parsed > 0 else {
    throw ProbeFailure(description: "\(flag) must be a positive integer")
  }
  return parsed
}

private func parseArguments() throws -> Arguments {
  var pid: pid_t?
  var screenshot: URL?
  var absentWindowID: CGWindowID?
  var timeout: TimeInterval = 30
  var index = 1
  let arguments = CommandLine.arguments
  while index < arguments.count {
    let flag = arguments[index]
    guard index + 1 < arguments.count else {
      throw ProbeFailure(description: "missing value for \(flag)")
    }
    let value = arguments[index + 1]
    switch flag {
    case "--pid":
      pid = pid_t(try parsePositiveInteger(value, flag: flag))
    case "--screenshot":
      screenshot = URL(fileURLWithPath: value).standardizedFileURL
    case "--assert-absent-window-id":
      absentWindowID = CGWindowID(try parsePositiveInteger(value, flag: flag))
    case "--timeout":
      guard let parsed = TimeInterval(value), parsed > 0 else {
        throw ProbeFailure(description: "--timeout must be greater than zero")
      }
      timeout = parsed
    default:
      throw ProbeFailure(description: "unknown argument: \(flag)")
    }
    index += 2
  }
  guard let pid else {
    throw ProbeFailure(description: "--pid is required")
  }
  if (screenshot == nil) == (absentWindowID == nil) {
    throw ProbeFailure(
      description: "exactly one of --screenshot and --assert-absent-window-id is required"
    )
  }
  return Arguments(
    pid: pid,
    screenshot: screenshot,
    absentWindowID: absentWindowID,
    timeout: timeout
  )
}

private func nativeWindows(ownerPID: pid_t) throws -> [NativeWindow] {
  let options: CGWindowListOption = [.excludeDesktopElements, .optionOnScreenOnly]
  guard
    let rows = CGWindowListCopyWindowInfo(options, kCGNullWindowID)
      as? [[String: Any]]
  else {
    throw ProbeFailure(description: "CGWindowListCopyWindowInfo returned no window list")
  }
  return rows.compactMap { row in
    guard
      let pidNumber = row[kCGWindowOwnerPID as String] as? NSNumber,
      pidNumber.int32Value == ownerPID,
      let layerNumber = row[kCGWindowLayer as String] as? NSNumber,
      layerNumber.intValue == 0,
      let windowNumber = row[kCGWindowNumber as String] as? NSNumber,
      let boundsDictionary = row[kCGWindowBounds as String] as? NSDictionary,
      let bounds = CGRect(dictionaryRepresentation: boundsDictionary),
      bounds.width > 0,
      bounds.height > 0
    else {
      return nil
    }
    let title = row[kCGWindowName as String] as? String ?? ""
    return NativeWindow(
      bounds: bounds,
      identifier: CGWindowID(windowNumber.uint32Value),
      ownerPID: ownerPID,
      title: title
    )
  }
}

private func rawWindowRecordExists(
  ownerPID: pid_t, windowID: CGWindowID
) throws -> Bool {
  guard
    let rows = CGWindowListCopyWindowInfo(.optionAll, kCGNullWindowID)
      as? [[String: Any]]
  else {
    throw ProbeFailure(description: "CGWindowListCopyWindowInfo returned no window list")
  }
  return rows.contains { row in
    guard
      let pidNumber = row[kCGWindowOwnerPID as String] as? NSNumber,
      let windowNumber = row[kCGWindowNumber as String] as? NSNumber
    else {
      return false
    }
    return pidNumber.int32Value == ownerPID
      && CGWindowID(windowNumber.uint32Value) == windowID
  }
}

private func copyAttribute(_ element: AXUIElement, _ attribute: String) -> CFTypeRef? {
  var value: CFTypeRef?
  guard
    AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success
  else {
    return nil
  }
  return value
}

private func elements(from value: CFTypeRef?) -> [AXUIElement] {
  guard let value, CFGetTypeID(value) == CFArrayGetTypeID() else { return [] }
  let array = value as! CFArray
  let values = array as NSArray
  return values.compactMap { item -> AXUIElement? in
    guard let item = item as CFTypeRef?,
          CFGetTypeID(item) == AXUIElementGetTypeID() else { return nil }
    return (item as! AXUIElement)
  }
}

private func axElement(from value: CFTypeRef?) -> AXUIElement? {
  guard let value, CFGetTypeID(value) == AXUIElementGetTypeID() else {
    return nil
  }
  return (value as! AXUIElement)
}

private func stringAttribute(_ element: AXUIElement, _ attribute: String) -> String? {
  guard let value = copyAttribute(element, attribute) else {
    return nil
  }
  return value as? String
}

private func sizeAttribute(_ element: AXUIElement) -> CGSize? {
  guard
    let value = copyAttribute(element, kAXSizeAttribute),
    CFGetTypeID(value) == AXValueGetTypeID()
  else {
    return nil
  }
  var size = CGSize.zero
  guard AXValueGetValue(value as! AXValue, .cgSize, &size) else {
    return nil
  }
  return size
}

private func isTabRowIdentifier(_ identifier: String) -> Bool {
  identifier.hasPrefix(tabRowIdentifierPrefix)
    && !identifier.hasPrefix(tabCloseIdentifierPrefix)
}

private func scanBoundaryElements(
  from roots: [AXUIElement], rail: inout AXUIElement?, focus: inout AXUIElement?
) {
  var stack = roots.map { (element: $0, depth: 0) }
  var visitedNodes = 0
  while let current = stack.popLast() {
    visitedNodes += 1
    if visitedNodes > maximumAccessibilityNodes {
      return
    }
    if let identifier = stringAttribute(current.element, kAXIdentifierAttribute) {
      if identifier == railIdentifier {
        rail = current.element
      } else if isTabRowIdentifier(identifier) {
        focus = current.element
      }
    }
    if rail != nil, focus != nil {
      return
    }
    if current.depth >= maximumAccessibilityDepth {
      continue
    }
    let children = elements(from: copyAttribute(current.element, kAXChildrenAttribute))
    for child in children.reversed() {
      stack.append((child, current.depth + 1))
    }
  }
}

private func scanBoundaryAncestry(
  from element: AXUIElement, rail: inout AXUIElement?, focus: inout AXUIElement?
) {
  var current: AXUIElement? = element
  for _ in 0..<maximumAccessibilityDepth {
    guard let candidate = current else {
      return
    }
    if let identifier = stringAttribute(candidate, kAXIdentifierAttribute) {
      if identifier == railIdentifier {
        rail = candidate
      } else if isTabRowIdentifier(identifier) {
        focus = candidate
      }
    }
    guard let parent = axElement(from: copyAttribute(candidate, kAXParentAttribute)) else {
      return
    }
    current = parent
  }
}

private func element(
  at point: CGPoint, in application: AXUIElement
) -> AXUIElement? {
  var result: AXUIElement?
  guard
    AXUIElementCopyElementAtPosition(
      application, Float(point.x), Float(point.y), &result
    ) == .success
  else {
    return nil
  }
  return result
}

private func findBoundaryElements(
  in window: AXUIElement,
  application: AXUIElement,
  nativeBounds: CGRect,
  deadline: Date
) -> BoundaryElements? {
  var rail: AXUIElement?
  var focus: AXUIElement?
  scanBoundaryElements(from: [window], rail: &rail, focus: &focus)
  if let rail, let focus {
    return BoundaryElements(focus: focus, rail: rail, window: window)
  }

  // Chromium exposes its Views accessibility tree as the AXWindow's direct
  // subtree. The hosted AppKit hierarchy can instead be discoverable through
  // native accessibility hit testing. Sample only the rail's fixed leading
  // strip, then verify identifiers and parent chains before trusting a hit.
  let xOffsets: [CGFloat] = [16, 40, 72, 108, 152, 200]
  var y = nativeBounds.minY + 8
  while y < nativeBounds.maxY - 8, Date() < deadline {
    for xOffset in xOffsets where xOffset < nativeBounds.width {
      guard Date() < deadline else {
        return nil
      }
      guard
        let hit = element(
          at: CGPoint(x: nativeBounds.minX + xOffset, y: y), in: application
        )
      else {
        continue
      }
      scanBoundaryAncestry(from: hit, rail: &rail, focus: &focus)
      scanBoundaryElements(from: [hit], rail: &rail, focus: &focus)
      if let rail, let focus {
        return BoundaryElements(focus: focus, rail: rail, window: window)
      }
    }
    y += 8
  }
  return nil
}

private func describeAXElement(_ element: AXUIElement) -> String {
  let identifier = stringAttribute(element, kAXIdentifierAttribute) ?? ""
  let role = stringAttribute(element, kAXRoleAttribute) ?? ""
  let title = stringAttribute(element, kAXTitleAttribute) ?? ""
  let description = stringAttribute(element, kAXDescriptionAttribute) ?? ""
  return "role=\(role),id=\(identifier),title=\(title),description=\(description)"
}

private func accessibilityDiagnostics(
  window: AXUIElement, application: AXUIElement, nativeBounds: CGRect
) -> String {
  var stack: [(element: AXUIElement, depth: Int)] = [(window, 0)]
  var nodeCount = 0
  var noteworthy: [String] = []
  while let current = stack.popLast(), nodeCount < 250 {
    nodeCount += 1
    let detail = describeAXElement(current.element)
    if detail.localizedCaseInsensitiveContains("limitless")
      || detail.localizedCaseInsensitiveContains("swift")
      || !((stringAttribute(current.element, kAXIdentifierAttribute) ?? "").isEmpty)
    {
      noteworthy.append(detail)
    }
    if current.depth < 16 {
      for child in elements(
        from: copyAttribute(current.element, kAXChildrenAttribute)
      ).reversed() {
        stack.append((child, current.depth + 1))
      }
    }
  }
  var hits: [String] = []
  for xOffset: CGFloat in [24, 64, 108, 160, 200] {
    for yOffset: CGFloat in [80, 120, 160, 200, 240] {
      guard
        let hit = element(
          at: CGPoint(
            x: nativeBounds.minX + xOffset, y: nativeBounds.minY + yOffset
          ),
          in: application
        )
      else {
        continue
      }
      let detail = describeAXElement(hit)
      if !hits.contains(detail) {
        hits.append(detail)
      }
    }
  }
  return "nodes=\(nodeCount),noteworthy=\(noteworthy.prefix(20)),hits=\(hits.prefix(20))"
}

private func parentChainReachesWindow(
  from element: AXUIElement, window: AXUIElement
) -> Bool {
  var current: AXUIElement? = element
  for _ in 0..<maximumAccessibilityDepth {
    guard let candidate = current else {
      return false
    }
    if CFEqual(candidate, window) {
      return true
    }
    guard let parent = axElement(from: copyAttribute(candidate, kAXParentAttribute)) else {
      return false
    }
    current = parent
  }
  return false
}

private func focusedElement(of application: AXUIElement) -> AXUIElement? {
  return axElement(from: copyAttribute(application, kAXFocusedUIElementAttribute))
}

private func displayEvidence(for windowBounds: CGRect) throws -> DisplayEvidence {
  var displayCount: UInt32 = 0
  guard CGGetActiveDisplayList(0, nil, &displayCount) == .success, displayCount > 0 else {
    throw ProbeFailure(description: "could not enumerate active displays")
  }
  var displays = [CGDirectDisplayID](repeating: 0, count: Int(displayCount))
  guard
    CGGetActiveDisplayList(displayCount, &displays, &displayCount) == .success
  else {
    throw ProbeFailure(description: "could not read active display identifiers")
  }
  let epsilon: CGFloat = 0.5
  let containingDisplays = displays.filter { display in
    let bounds = CGDisplayBounds(display)
    return windowBounds.minX >= bounds.minX - epsilon
      && windowBounds.minY >= bounds.minY - epsilon
      && windowBounds.maxX <= bounds.maxX + epsilon
      && windowBounds.maxY <= bounds.maxY + epsilon
  }
  guard containingDisplays.count == 1, let matchedDisplay = containingDisplays.first else {
    throw ProbeFailure(
      description: "native window did not belong to exactly one active display"
    )
  }
  guard let displayMode = CGDisplayCopyDisplayMode(matchedDisplay) else {
    throw ProbeFailure(description: "could not read the matched display mode")
  }
  let pointWidth = CGFloat(displayMode.width)
  let pointHeight = CGFloat(displayMode.height)
  guard pointWidth > 0, pointHeight > 0 else {
    throw ProbeFailure(description: "matched display mode had invalid point dimensions")
  }
  let horizontalScale = CGFloat(displayMode.pixelWidth) / pointWidth
  let verticalScale = CGFloat(displayMode.pixelHeight) / pointHeight
  guard abs(horizontalScale - verticalScale) < 0.01, horizontalScale > 0 else {
    throw ProbeFailure(description: "matched display reported inconsistent scaling")
  }
  guard let normalizedScale = [CGFloat(1), CGFloat(2)].first(where: {
    abs(horizontalScale - $0) < 0.01
  }) else {
    throw ProbeFailure(description: "matched display reported an unsupported backing scale")
  }
  return DisplayEvidence(
    bounds: CGDisplayBounds(matchedDisplay),
    identifier: matchedDisplay,
    scale: normalizedScale
  )
}

extension CGRect {
  fileprivate var area: CGFloat {
    guard !isNull, !isInfinite else {
      return 0
    }
    return width * height
  }
}

private func waitForExit(_ process: Process, until deadline: Date) -> Bool {
  while process.isRunning, Date() < deadline {
    Thread.sleep(forTimeInterval: 0.02)
  }
  return !process.isRunning
}

private func captureWindow(
  _ window: NativeWindow, to screenshot: URL, deadline: Date
) throws {
  var existingStatus = stat()
  if lstat(screenshot.path, &existingStatus) == 0 || errno != ENOENT {
    throw ProbeFailure(description: "native screenshot path already exists: \(screenshot.path)")
  }
  let process = Process()
  let stderr = Pipe()
  process.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
  process.arguments = [
    "-x", "-o", "-a", "-t", "png", "-l\(window.identifier)", screenshot.path,
  ]
  process.standardError = stderr
  try process.run()
  if !waitForExit(process, until: deadline) {
    process.terminate()
    if !waitForExit(
      process, until: Date().addingTimeInterval(0.4)
    ) {
      _ = Darwin.kill(process.processIdentifier, SIGKILL)
      guard
        waitForExit(
          process, until: Date().addingTimeInterval(0.4)
        )
      else {
        throw ProbeFailure(description: "could not stop timed-out screencapture process")
      }
    }
    process.waitUntilExit()
    throw ProbeFailure(description: "screencapture timed out")
  }
  process.waitUntilExit()
  guard process.terminationStatus == 0 else {
    let detail =
      String(
        data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8
      )?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "no error output"
    throw ProbeFailure(
      description: "screencapture failed with status \(process.terminationStatus): \(detail)"
    )
  }
  var screenshotStatus = stat()
  guard
    lstat(screenshot.path, &screenshotStatus) == 0,
    screenshotStatus.st_mode & S_IFMT == S_IFREG,
    screenshotStatus.st_size > 0
  else {
    throw ProbeFailure(
      description: "screencapture did not produce one nonempty regular PNG file"
    )
  }
}

private func approximatelyEqual(_ left: CGRect, _ right: CGRect) -> Bool {
  let epsilon: CGFloat = 0.5
  return abs(left.minX - right.minX) <= epsilon
    && abs(left.minY - right.minY) <= epsilon
    && abs(left.width - right.width) <= epsilon
    && abs(left.height - right.height) <= epsilon
}

private func waitForBoundary(
  pid: pid_t, deadline: Date
) throws -> (NativeWindow, AXUIElement, BoundaryElements) {
  let application = AXUIElementCreateApplication(pid)
  AXUIElementSetMessagingTimeout(application, 2)
  var lastWindowCount = 0
  var lastAXWindowCount = 0
  var lastAXWindowRole = "missing"
  while Date() < deadline {
    let windows = try nativeWindows(ownerPID: pid)
    lastWindowCount = windows.count
    let axWindows = elements(from: copyAttribute(application, kAXWindowsAttribute))
    lastAXWindowCount = axWindows.count
    if axWindows.count == 1 {
      lastAXWindowRole = stringAttribute(axWindows[0], kAXRoleAttribute) ?? "missing"
    }
    if windows.count == 1, axWindows.count == 1, lastAXWindowRole == kAXWindowRole,
      let boundary = findBoundaryElements(
        in: axWindows[0],
        application: application,
        nativeBounds: windows[0].bounds,
        deadline: deadline
      )
    {
      return (windows[0], application, boundary)
    }
    Thread.sleep(forTimeInterval: 0.1)
  }
  var diagnostics = "unavailable"
  let finalWindows = try nativeWindows(ownerPID: pid)
  let finalAXWindows = elements(from: copyAttribute(application, kAXWindowsAttribute))
  if finalWindows.count == 1, finalAXWindows.count == 1 {
    diagnostics = accessibilityDiagnostics(
      window: finalAXWindows[0],
      application: application,
      nativeBounds: finalWindows[0].bounds
    )
  }
  throw ProbeFailure(
    description: "timed out waiting for one PID-owned window and the SwiftUI AX boundary "
      + "(native windows: \(lastWindowCount), AX windows: \(lastAXWindowCount), "
      + "AX window role: \(lastAXWindowRole), "
      + "AX diagnostics: \(diagnostics))"
  )
}

private func focusBoundary(
  application: AXUIElement, boundary: BoundaryElements, pid: pid_t, deadline: Date
) throws {
  guard let runningApplication = NSRunningApplication(processIdentifier: pid) else {
    throw ProbeFailure(description: "could not resolve the running browser application")
  }
  _ = runningApplication.activate(options: [.activateAllWindows])
  let focusError = AXUIElementSetAttributeValue(
    boundary.focus, kAXFocusedAttribute as CFString, kCFBooleanTrue
  )
  guard focusError == .success else {
    throw ProbeFailure(description: "could not focus SwiftUI AX probe: \(focusError.rawValue)")
  }
  while Date() < deadline {
    if let focused = focusedElement(of: application), CFEqual(focused, boundary.focus) {
      return
    }
    Thread.sleep(forTimeInterval: 0.05)
  }
  throw ProbeFailure(description: "timed out focusing the SwiftUI AX probe")
}

private func verifyAndCapture(_ arguments: Arguments) throws -> [String: Any] {
  guard CGPreflightScreenCaptureAccess() else {
    throw ProbeFailure(
      description: "Screen Recording permission is required for native whole-window capture"
    )
  }
  guard AXIsProcessTrusted() else {
    throw ProbeFailure(
      description: "Accessibility permission is required for the native SwiftUI boundary probe"
    )
  }
  guard let screenshot = arguments.screenshot else {
    throw ProbeFailure(description: "internal error: native screenshot path is missing")
  }
  let deadline = Date().addingTimeInterval(arguments.timeout)
  let (window, application, boundary) = try waitForBoundary(
    pid: arguments.pid, deadline: deadline
  )
  guard parentChainReachesWindow(from: boundary.rail, window: boundary.window) else {
    throw ProbeFailure(description: "SwiftUI rail AX parent chain did not reach the AXWindow")
  }
  guard parentChainReachesWindow(from: boundary.focus, window: boundary.window) else {
    throw ProbeFailure(description: "SwiftUI focus AX parent chain did not reach the AXWindow")
  }
  guard let railSize = sizeAttribute(boundary.rail), railSize.width > 0 else {
    throw ProbeFailure(description: "SwiftUI rail AX element did not expose a positive size")
  }
  try focusBoundary(
    application: application,
    boundary: boundary,
    pid: arguments.pid,
    deadline: deadline
  )
  let beforeCaptureWindows = try nativeWindows(ownerPID: arguments.pid).filter {
    $0.identifier == window.identifier
  }
  guard beforeCaptureWindows.count == 1,
    let beforeCaptureWindow = beforeCaptureWindows.first,
    approximatelyEqual(beforeCaptureWindow.bounds, window.bounds)
  else {
    throw ProbeFailure(description: "native window bounds changed before capture")
  }
  let display = try displayEvidence(for: beforeCaptureWindow.bounds)
  try captureWindow(beforeCaptureWindow, to: screenshot, deadline: deadline)
  let afterCaptureWindows = try nativeWindows(ownerPID: arguments.pid).filter {
    $0.identifier == window.identifier
  }
  guard afterCaptureWindows.count == 1,
    let afterCaptureWindow = afterCaptureWindows.first,
    approximatelyEqual(afterCaptureWindow.bounds, beforeCaptureWindow.bounds)
  else {
    throw ProbeFailure(description: "native window bounds changed during capture")
  }
  guard
    let focusIdentifier = stringAttribute(boundary.focus, kAXIdentifierAttribute),
    isTabRowIdentifier(focusIdentifier)
  else {
    throw ProbeFailure(description: "focused SwiftUI tab row lost its AX identifier")
  }
  return [
    "accessibility_permission": true,
    "ax_window_count": 1,
    "cg_window_id": Int(window.identifier),
    "display_bounds": [
      "height": Double(display.bounds.height),
      "width": Double(display.bounds.width),
      "x": Double(display.bounds.minX),
      "y": Double(display.bounds.minY),
    ],
    "display_id": Int(display.identifier),
    "display_scale": Double(display.scale),
    "focus_identifier": focusIdentifier,
    "focus_method": "macos-accessibility-AXFocused",
    "focus_parent_reaches_window": true,
    "focus_succeeded": true,
    "matching_window_count": 1,
    "native_keyboard_input": false,
    "owner_pid": Int(window.ownerPID),
    "rail_identifier": railIdentifier,
    "rail_parent_reaches_window": true,
    "rail_width": Double(railSize.width),
    "screen_capture_permission": true,
    "screenshot": screenshot.path,
    "screenshot_method": "screencapture-window-id",
    "window_bounds": [
      "height": Double(beforeCaptureWindow.bounds.height),
      "width": Double(beforeCaptureWindow.bounds.width),
      "x": Double(beforeCaptureWindow.bounds.minX),
      "y": Double(beforeCaptureWindow.bounds.minY),
    ],
    "window_title": window.title,
  ]
}

private func assertWindowAbsent(_ arguments: Arguments) throws -> [String: Any] {
  guard let absentWindowID = arguments.absentWindowID else {
    throw ProbeFailure(description: "internal error: absent window identifier is missing")
  }
  let deadline = Date().addingTimeInterval(arguments.timeout)
  while Date() < deadline {
    if try !rawWindowRecordExists(
      ownerPID: arguments.pid, windowID: absentWindowID
    ) {
      return [
        "owner_pid": Int(arguments.pid),
        "window_absent": true,
        "window_id": Int(absentWindowID),
      ]
    }
    Thread.sleep(forTimeInterval: 0.1)
  }
  throw ProbeFailure(
    description: "native window \(absentWindowID) remained after Browser.close"
  )
}

private func emitJSON(_ value: [String: Any]) throws {
  guard JSONSerialization.isValidJSONObject(value) else {
    throw ProbeFailure(description: "probe result was not valid JSON")
  }
  let data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
  guard let output = String(data: data, encoding: .utf8) else {
    throw ProbeFailure(description: "could not encode probe result as UTF-8")
  }
  print(output)
}

do {
  let arguments = try parseArguments()
  let result =
    try arguments.absentWindowID == nil
    ? verifyAndCapture(arguments) : assertWindowAbsent(arguments)
  try emitJSON(result)
} catch {
  let message = "macOS boundary probe failed: \(error)\n"
  FileHandle.standardError.write(Data(message.utf8))
  exit(1)
}
