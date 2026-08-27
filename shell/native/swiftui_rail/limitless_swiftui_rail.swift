// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AppKit
import SwiftUI

private struct LimitlessTab: Identifiable, Equatable {
  let id: Int32
  let productID: String?
  let title: String
  let isActive: Bool
}

private enum LimitlessSplitLayout: String {
  case sideBySide
  case stacked

  var label: String {
    switch self {
    case .sideBySide:
      return "Side by side"
    case .stacked:
      return "Stacked"
    }
  }
}

private enum LimitlessSplitDestinationEligibility: String {
  case eligible
  case pinnedStateMismatch
  case groupMismatch
  case unavailable

  var reason: String? {
    switch self {
    case .eligible:
      return nil
    case .pinnedStateMismatch:
      return "pin state differs"
    case .groupMismatch:
      return "tab group differs"
    case .unavailable:
      return "unavailable"
    }
  }
}

private struct LimitlessSplitPair: Identifiable, Equatable {
  let id: String
  let firstTabID: String
  let secondTabID: String
  let activeTabID: String?
  let layout: LimitlessSplitLayout
  let ratio: Double

  func contains(_ tabID: String) -> Bool {
    tabID == firstTabID || tabID == secondTabID
  }
}

private struct LimitlessWorkspace: Identifiable, Equatable {
  let id: String
  let name: String
  let isDefault: Bool
  let isActive: Bool
}

private struct LimitlessWorkspaceProjection: Equatable {
  let enabled: Bool
  let windowID: String
  let workspaces: [LimitlessWorkspace]
  let tabs: [LimitlessTab]
  let splitActionsAvailable: Bool
  let splitPairs: [LimitlessSplitPair]
  let splitPairByMemberID: [String: LimitlessSplitPair]

  static let empty = LimitlessWorkspaceProjection(
    enabled: false,
    windowID: "",
    workspaces: [],
    tabs: [],
    splitActionsAvailable: false,
    splitPairs: [],
    splitPairByMemberID: [:])
}

private struct LimitlessAgent: Identifiable, Equatable {
  let id: String
  let workspaceID: String
  let workspaceLabel: String
  let agent: String
  let status: String
  let terminalTitle: String
  let isFocused: Bool
}

private enum LimitlessAgentSourceState: String {
  case loading
  case ready
  case missingExecutable
  case permissionDenied
  case unavailable
  case timedOut
  case malformedResponse
  case incompatibleProtocol
  case privateProfileDisabled
}

private enum LimitlessLeaderStatusText {
  static let visible = "Leader: n/p tabs, 1-9 jump, Shift+X close, b sidebar"
  static let accessibility =
    "\(visible). Press Escape to cancel. Leader mode automatically cancels within two seconds."
}

private enum LimitlessRailDensity: String {
  case compact
  case comfortable

  var tabRowHeight: CGFloat {
    switch self {
    case .compact:
      return 28
    case .comfortable:
      return 34
    }
  }
}

private enum LimitlessLayoutProbeRole: Hashable {
  case workspaceHeader
  case workspaceCreateControl
  case workspaceSwitcherViewport
  case workspaceSwitcherContent
  case workspaceViewport
  case workspaceContent
  case firstTabRow
  case agentViewport
  case agentContent
}

private final class LimitlessLayoutProbeView: NSView {
  override func hitTest(_ point: NSPoint) -> NSView? {
    nil
  }
}

private final class LimitlessWeakLayoutProbe {
  weak var view: NSView?

  init(_ view: NSView) {
    self.view = view
  }
}

@MainActor
private final class LimitlessLayoutProbeStore {
  private var probes: [LimitlessLayoutProbeRole: LimitlessWeakLayoutProbe] = [:]

  func register(_ view: NSView, for role: LimitlessLayoutProbeRole) {
    probes[role] = LimitlessWeakLayoutProbe(view)
  }

  func frame(for role: LimitlessLayoutProbeRole, in container: NSView) -> NSRect {
    guard let view = probes[role]?.view else {
      return .zero
    }
    return view.convert(view.bounds, to: container)
  }
}

@MainActor
private struct LimitlessViewportProbe: NSViewRepresentable {
  let role: LimitlessLayoutProbeRole
  let store: LimitlessLayoutProbeStore

  func makeNSView(context: Context) -> NSView {
    let view = LimitlessLayoutProbeView(frame: .zero)
    view.setAccessibilityElement(false)
    store.register(view, for: role)
    return view
  }

  func updateNSView(_ nsView: NSView, context: Context) {
    store.register(nsView, for: role)
  }
}

@MainActor
private final class LimitlessTabRailModel: ObservableObject {
  @Published private(set) var workspaceProjection = LimitlessWorkspaceProjection.empty
  @Published private(set) var agents: [LimitlessAgent] = []
  @Published private(set) var agentSourceState: LimitlessAgentSourceState = .loading
  @Published private(set) var leaderEnabled = false
  @Published private(set) var leaderArmed = false
  @Published private(set) var railDensity = LimitlessRailDensity.compact

  var leaderStatusText: String {
    leaderArmed ? LimitlessLeaderStatusText.visible : ""
  }

  var tabs: [LimitlessTab] {
    workspaceProjection.tabs
  }

  var workspaces: [LimitlessWorkspace] {
    workspaceProjection.workspaces
  }

  var activeSplitPair: LimitlessSplitPair? {
    return workspaceProjection.splitPairs.first { $0.activeTabID != nil }
  }

  func splitPair(forProductID productID: String?) -> LimitlessSplitPair? {
    guard let productID else {
      return nil
    }
    return workspaceProjection.splitPairByMemberID[productID]
  }

  func replaceWorkspaceProjection(
    enabled: Bool,
    windowIdentifier: String,
    workspaceIdentifiers: [String],
    workspaceNames: [String],
    defaultWorkspaces: [NSNumber],
    activeWorkspaceIdentifier: String?,
    identifiers: [NSNumber],
    productTabIdentifiers: [String],
    titles: [String],
    activeIdentifier: NSNumber?,
    splitActionsAvailable: Bool,
    splitPairIdentifiers: [String],
    splitFirstTabIdentifiers: [String],
    splitSecondTabIdentifiers: [String],
    splitActiveTabIdentifiers: [String],
    splitLayouts: [String],
    splitRatios: [NSNumber]
  ) {
    let splitCount = splitPairIdentifiers.count
    let productTabIdentifiersAreAligned =
      productTabIdentifiers.count == identifiers.count
    let productTabIdentifiersAreRequired =
      splitActionsAvailable || splitCount > 0
    guard
      identifiers.count == titles.count,
      productTabIdentifiers.isEmpty || productTabIdentifiersAreAligned,
      !productTabIdentifiersAreRequired || productTabIdentifiersAreAligned,
      !productTabIdentifiersAreRequired || !productTabIdentifiers.isEmpty,
      workspaceIdentifiers.count == workspaceNames.count,
      workspaceIdentifiers.count == defaultWorkspaces.count,
      !enabled || (!windowIdentifier.isEmpty && activeWorkspaceIdentifier != nil),
      splitFirstTabIdentifiers.count == splitCount,
      splitSecondTabIdentifiers.count == splitCount,
      splitActiveTabIdentifiers.count == splitCount,
      splitLayouts.count == splitCount,
      splitRatios.count == splitCount,
      !productTabIdentifiersAreRequired || !windowIdentifier.isEmpty
    else {
      workspaceProjection = .empty
      return
    }
    let active = activeIdentifier?.int32Value
    let productTabIDSet = Set(productTabIdentifiers)
    let workspaceIDSet = Set(workspaceIdentifiers)
    guard
      productTabIDSet.count == productTabIdentifiers.count,
      !productTabIdentifiers.contains(where: \.isEmpty),
      Set(identifiers.map(\.int32Value)).count == identifiers.count,
      active == nil || identifiers.contains(where: { $0.int32Value == active }),
      workspaceIDSet.count == workspaceIdentifiers.count,
      !workspaceIdentifiers.contains(where: \.isEmpty),
      !enabled
        || workspaceIdentifiers.filter({ $0 == activeWorkspaceIdentifier }).count == 1
    else {
      workspaceProjection = .empty
      return
    }
    let tabs = identifiers.indices.map { index in
      let identifier = identifiers[index]
      let title = titles[index]
      let tabIdentifier = identifier.int32Value
      return LimitlessTab(
        id: tabIdentifier,
        productID: productTabIdentifiersAreAligned
          ? productTabIdentifiers[index]
          : nil,
        title: title.isEmpty ? "New Tab" : title,
        isActive: tabIdentifier == active)
    }
    let workspaces = workspaceIdentifiers.indices.map { index in
      LimitlessWorkspace(
        id: workspaceIdentifiers[index],
        name: workspaceNames[index],
        isDefault: defaultWorkspaces[index].boolValue,
        isActive: workspaceIdentifiers[index] == activeWorkspaceIdentifier)
    }

    var splitPairs: [LimitlessSplitPair] = []
    var splitPairIDSet = Set<String>()
    var splitMemberIDSet = Set<String>()
    var splitPairByMemberID: [String: LimitlessSplitPair] = [:]
    var activeSplitCount = 0
    for index in splitPairIdentifiers.indices {
      guard
        !splitPairIdentifiers[index].isEmpty,
        splitPairIDSet.insert(splitPairIdentifiers[index]).inserted,
        splitFirstTabIdentifiers[index] != splitSecondTabIdentifiers[index],
        productTabIDSet.contains(splitFirstTabIdentifiers[index]),
        productTabIDSet.contains(splitSecondTabIdentifiers[index]),
        splitMemberIDSet.insert(splitFirstTabIdentifiers[index]).inserted,
        splitMemberIDSet.insert(splitSecondTabIdentifiers[index]).inserted,
        let layout = LimitlessSplitLayout(rawValue: splitLayouts[index])
      else {
        workspaceProjection = .empty
        return
      }
      let ratio = splitRatios[index].doubleValue
      guard ratio.isFinite && (0.0...1.0).contains(ratio) else {
        workspaceProjection = .empty
        return
      }
      let activeTabID =
        splitActiveTabIdentifiers[index].isEmpty
        ? nil
        : splitActiveTabIdentifiers[index]
      guard
        activeTabID == nil
          || activeTabID == splitFirstTabIdentifiers[index]
          || activeTabID == splitSecondTabIdentifiers[index]
      else {
        workspaceProjection = .empty
        return
      }
      if activeTabID != nil {
        activeSplitCount += 1
      }
      let pair = LimitlessSplitPair(
        id: splitPairIdentifiers[index],
        firstTabID: splitFirstTabIdentifiers[index],
        secondTabID: splitSecondTabIdentifiers[index],
        activeTabID: activeTabID,
        layout: layout,
        ratio: ratio)
      splitPairs.append(pair)
      splitPairByMemberID[pair.firstTabID] = pair
      splitPairByMemberID[pair.secondTabID] = pair
    }
    let activeProductTabID = tabs.first(where: \.isActive)?.productID
    let splitPairContainingActiveTab = splitPairs.first { pair in
      activeProductTabID.map(pair.contains) ?? false
    }
    guard
      activeSplitCount <= 1,
      splitPairs.allSatisfy({ pair in
        pair.activeTabID == nil || pair.activeTabID == activeProductTabID
      }),
      splitPairContainingActiveTab == nil
        || splitPairContainingActiveTab?.activeTabID == activeProductTabID
    else {
      workspaceProjection = .empty
      return
    }
    workspaceProjection = LimitlessWorkspaceProjection(
      enabled: enabled,
      windowID: windowIdentifier,
      workspaces: workspaces,
      tabs: tabs,
      splitActionsAvailable: splitActionsAvailable,
      splitPairs: splitPairs,
      splitPairByMemberID: splitPairByMemberID)
  }

  func replaceAgents(
    paneIdentifiers: [String],
    workspaceIdentifiers: [String],
    workspaceLabels: [String],
    agents agentKinds: [String],
    statuses: [String],
    terminalTitles: [String],
    focused: [NSNumber],
    sourceState: String
  ) {
    guard
      let parsedState = LimitlessAgentSourceState(rawValue: sourceState),
      paneIdentifiers.count == workspaceIdentifiers.count,
      paneIdentifiers.count == workspaceLabels.count,
      paneIdentifiers.count == agentKinds.count,
      paneIdentifiers.count == statuses.count,
      paneIdentifiers.count == terminalTitles.count,
      paneIdentifiers.count == focused.count
    else {
      agents = []
      agentSourceState = .malformedResponse
      return
    }
    agentSourceState = parsedState
    guard parsedState == .ready else {
      agents = []
      return
    }
    agents = paneIdentifiers.indices.map { index in
      LimitlessAgent(
        id: paneIdentifiers[index],
        workspaceID: workspaceIdentifiers[index],
        workspaceLabel: workspaceLabels[index],
        agent: agentKinds[index],
        status: statuses[index],
        terminalTitle: terminalTitles[index],
        isFocused: focused[index].boolValue)
    }
  }

  func replaceLeader(enabled: Bool, armed: Bool) {
    leaderEnabled = enabled
    leaderArmed = enabled && armed
  }

  @discardableResult
  func replaceRailDensity(_ value: String) -> Bool {
    guard let density = LimitlessRailDensity(rawValue: value) else {
      return false
    }
    railDensity = density
    return true
  }
}

private struct LimitlessLeaderStatus: View {
  var body: some View {
    HStack(spacing: 6) {
      Rectangle()
        .fill(Color.accentColor)
        .frame(width: 2, height: 14)
        .accessibilityHidden(true)
      Text(LimitlessLeaderStatusText.visible)
        .font(.system(size: 10, weight: .semibold))
        .lineLimit(1)
        .truncationMode(.tail)
    }
    .padding(.horizontal, 6)
    .frame(maxWidth: .infinity, minHeight: 22, maxHeight: 22, alignment: .leading)
    .background {
      Rectangle()
        .fill(Color.accentColor.opacity(0.12))
    }
    .overlay(alignment: .bottom) {
      Rectangle()
        .fill(Color.accentColor.opacity(0.35))
        .frame(height: 1)
        .accessibilityHidden(true)
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(LimitlessLeaderStatusText.accessibility)
    .accessibilityIdentifier("limitless-leader-status")
  }
}

private struct LimitlessSplitMenuButton: NSViewRepresentable {
  let tab: LimitlessTab
  let candidates: [LimitlessTab]
  let windowID: String
  let eligibility: (String, String, [String]) -> [String: String]
  let createSplit: (String, String, String) -> Bool

  @MainActor final class Coordinator: NSObject, NSMenuDelegate {
    var tab: LimitlessTab?
    var candidates: [LimitlessTab] = []
    var windowID = ""
    var eligibility: ((String, String, [String]) -> [String: String])?
    var createSplit: ((String, String, String) -> Bool)?

    func update(from source: LimitlessSplitMenuButton) {
      tab = source.tab
      candidates = source.candidates
      windowID = source.windowID
      eligibility = source.eligibility
      createSplit = source.createSplit
    }

    func installPlaceholder(in menu: NSMenu) {
      menu.removeAllItems()
      let placeholder = NSMenuItem(title: "Split", action: nil, keyEquivalent: "")
      placeholder.image = NSImage(
        systemSymbolName: "rectangle.split.2x1",
        accessibilityDescription: "Split tab")
      menu.addItem(placeholder)
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
      guard
        let tab,
        let productID = tab.productID,
        let eligibility
      else {
        installPlaceholder(in: menu)
        return
      }

      let destinations = candidates.compactMap { candidate in
        candidate.productID == productID ? nil : candidate.productID
      }
      let eligibilityByDestination = eligibility(windowID, productID, destinations)
      menu.removeAllItems()

      let heading = NSMenuItem(
        title: "Split \(tab.title) with another tab",
        action: nil,
        keyEquivalent: "")
      heading.isEnabled = false
      menu.addItem(heading)

      for peer in candidates {
        guard let peerProductID = peer.productID, peerProductID != productID else {
          continue
        }
        let status =
          LimitlessSplitDestinationEligibility(
            rawValue: eligibilityByDestination[peerProductID] ?? "")
          ?? .unavailable
        let title: String
        if let reason = status.reason {
          title = "Cannot split with \(peer.title) - \(reason)"
        } else {
          title = "Split \(tab.title) with \(peer.title)"
        }
        let item = NSMenuItem(
          title: title,
          action: #selector(selectDestination(_:)),
          keyEquivalent: "")
        item.target = self
        item.representedObject = peerProductID
        item.isEnabled = status == .eligible
        item.setAccessibilityIdentifier(
          "limitless-tab-split-\(productID)-with-\(peerProductID)")
        menu.addItem(item)
      }
    }

    func menuDidClose(_ menu: NSMenu) {
      installPlaceholder(in: menu)
    }

    @objc private func selectDestination(_ sender: NSMenuItem) {
      guard
        let productID = tab?.productID,
        let peerProductID = sender.representedObject as? String,
        let createSplit
      else {
        return
      }
      _ = createSplit(windowID, productID, peerProductID)
    }
  }

  func makeCoordinator() -> Coordinator {
    Coordinator()
  }

  func makeNSView(context: Context) -> NSPopUpButton {
    let button = NSPopUpButton(frame: .zero, pullsDown: true)
    button.isBordered = false
    button.controlSize = .small
    button.focusRingType = .default
    button.imagePosition = .imageOnly
    (button.cell as? NSPopUpButtonCell)?.arrowPosition = .noArrow
    button.menu?.autoenablesItems = false
    button.menu?.delegate = context.coordinator
    context.coordinator.update(from: self)
    if let menu = button.menu {
      context.coordinator.installPlaceholder(in: menu)
    }
    updateAccessibility(for: button)
    return button
  }

  func updateNSView(_ button: NSPopUpButton, context: Context) {
    context.coordinator.update(from: self)
    updateAccessibility(for: button)
    button.isEnabled = candidates.count >= 2
  }

  private func updateAccessibility(for button: NSPopUpButton) {
    guard let productID = tab.productID else {
      return
    }
    button.setAccessibilityLabel("Split \(tab.title) with another tab")
    button.setAccessibilityIdentifier("limitless-tab-split-\(productID)")
  }
}

private struct LimitlessTabRow: View {
  let tab: LimitlessTab
  let position: Int
  let count: Int
  let workspaces: [LimitlessWorkspace]
  let splitPair: LimitlessSplitPair?
  let splitCandidates: [LimitlessTab]
  let splitActionsAvailable: Bool
  let windowID: String
  let activate: (Int32) -> Bool
  let close: (Int32) -> Bool
  let move: (Int32, String) -> Bool
  let createSplit: (String, String, String) -> Bool
  let focusSplit: (String, String, String, String) -> Bool
  let splitEligibility: (String, String, [String]) -> [String: String]
  let density: LimitlessRailDensity
  let layoutProbeStore: LimitlessLayoutProbeStore
  let isFirstRow: Bool

  @State private var isHovering = false

  private var rowBackground: Color {
    if tab.isActive {
      return Color.primary.opacity(0.10)
    }
    if isHovering {
      return Color.primary.opacity(0.06)
    }
    return .clear
  }

  private var splitPosition: String? {
    guard let splitPair, let productID = tab.productID else {
      return nil
    }
    return productID == splitPair.firstTabID ? "first" : "second"
  }

  private var tabAccessibilityLabel: String {
    guard let splitPosition, let splitPair else {
      return tab.title
    }
    if splitPair.activeTabID == nil {
      return "Background split member, \(splitPosition) pane, \(tab.title)"
    }
    return "Focus \(splitPosition) pane, \(tab.title)"
  }

  private var tabAccessibilityValue: String {
    guard let splitPair else {
      return tab.isActive ? "Selected" : ""
    }
    if splitPair.activeTabID == nil {
      return "Background split member"
    }
    return tab.isActive ? "Focused pane" : "Visible pane"
  }

  private func activateOrFocus() {
    if let splitPair, let productID = tab.productID {
      _ = focusSplit(
        windowID,
        splitPair.firstTabID,
        splitPair.secondTabID,
        productID)
    } else {
      _ = activate(tab.id)
    }
  }

  var body: some View {
    HStack(spacing: 4) {
      Button {
        activateOrFocus()
      } label: {
        HStack(spacing: 7) {
          if let splitPosition {
            Text(splitPosition == "first" ? "1" : "2")
              .font(.system(size: 10, weight: .semibold))
              .frame(width: 16, height: 16)
              .foregroundStyle(.secondary)
              .accessibilityHidden(true)
          } else {
            Image(systemName: "doc")
              .font(.system(size: 12, weight: .regular))
              .frame(width: 16, height: 16)
              .foregroundStyle(.secondary)
          }
          Text(tab.title)
            .font(.system(size: 13, weight: tab.isActive ? .medium : .regular))
            .lineLimit(1)
            .truncationMode(.tail)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityLabel(tabAccessibilityLabel)
      .accessibilityValue(tabAccessibilityValue)
      .accessibilityHint("Tab \(position) of \(count)")
      .accessibilityAddTraits(tab.isActive ? [.isSelected] : [])
      .accessibilityIdentifier("limitless-tab-\(tab.id)")
      .disabled(splitPair != nil && !splitActionsAvailable)

      if splitActionsAvailable, splitPair == nil, let productID = tab.productID {
        LimitlessSplitMenuButton(
          tab: tab,
          candidates: splitCandidates,
          windowID: windowID,
          eligibility: splitEligibility,
          createSplit: createSplit)
        .frame(width: 22, height: 22)
        .opacity(splitCandidates.count < 2 ? 0.35 : 1)
        .accessibilityLabel("Split \(tab.title) with another tab")
        .accessibilityIdentifier("limitless-tab-split-\(productID)")
      }

      Menu {
        ForEach(workspaces.filter { !$0.isActive }) { workspace in
          Button("Move \(tab.title) to \(workspace.name)") {
            _ = move(tab.id, workspace.id)
          }
          .accessibilityIdentifier(
            "limitless-tab-move-\(tab.id)-to-\(workspace.id)")
        }
      } label: {
        Image(systemName: "arrow.right")
          .font(.system(size: 10, weight: .semibold))
          .frame(width: 22, height: 22)
          .contentShape(Rectangle())
      }
      .menuStyle(.borderlessButton)
      .disabled(workspaces.count < 2)
      .opacity(workspaces.count < 2 ? 0.35 : 1)
      .accessibilityLabel("Move \(tab.title) to another workspace")
      .accessibilityIdentifier("limitless-tab-move-\(tab.id)")

      Button {
        _ = close(tab.id)
      } label: {
        Image(systemName: "xmark")
          .font(.system(size: 10, weight: .semibold))
          .frame(width: 22, height: 22)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .opacity(tab.isActive || isHovering ? 1 : 0.45)
      .accessibilityLabel("Close \(tab.title)")
      .accessibilityIdentifier("limitless-tab-close-\(tab.id)")
    }
    .padding(.leading, 8)
    .padding(.trailing, 4)
    .frame(height: density.tabRowHeight)
    .background(alignment: .leading) {
      Rectangle()
        .fill(rowBackground)
      if tab.isActive {
        Rectangle()
          .fill(Color.accentColor)
          .frame(width: 2, height: 18)
      }
      if isFirstRow {
        GeometryReader { geometry in
          LimitlessViewportProbe(role: .firstTabRow, store: layoutProbeStore)
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
      }
    }
    .onHover { isHovering = $0 }
    .animation(.easeOut(duration: 0.10), value: tab.isActive)
    .animation(.easeOut(duration: 0.07), value: isHovering)
  }
}

private struct LimitlessSplitControls: View {
  let windowID: String
  let pair: LimitlessSplitPair
  let actionsAvailable: Bool
  let setLayout: (String, String, String, String) -> Bool
  let setRatio: (String, String, String, Double) -> Bool
  let swap: (String, String, String) -> Bool
  let unsplit: (String, String, String) -> Bool

  private var ratioPercent: Int {
    Int((pair.ratio * 100).rounded())
  }

  var body: some View {
    HStack(spacing: 2) {
      if actionsAvailable {
        Text("Split")
          .font(.system(size: 10, weight: .semibold))
          .lineLimit(1)
      } else {
        Button("Actions unavailable") {}
          .buttonStyle(.plain)
          .font(.system(size: 9, weight: .semibold))
          .lineLimit(1)
          .disabled(true)
          .accessibilityLabel("Split actions unavailable")
          .accessibilityValue("Actions unavailable")
          .accessibilityIdentifier("limitless-split-controls")
      }
      Spacer(minLength: 0)

      Menu {
        Button("Side by side") {
          _ = setLayout(
            windowID,
            pair.firstTabID,
            pair.secondTabID,
            LimitlessSplitLayout.sideBySide.rawValue)
        }
        .accessibilityIdentifier("limitless-split-layout-side-by-side")
        Button("Stacked") {
          _ = setLayout(
            windowID,
            pair.firstTabID,
            pair.secondTabID,
            LimitlessSplitLayout.stacked.rawValue)
        }
        .accessibilityIdentifier("limitless-split-layout-stacked")
      } label: {
        Image(
          systemName: pair.layout == .sideBySide
            ? "rectangle.split.2x1"
            : "rectangle.split.1x2"
        )
        .font(.system(size: 10, weight: .semibold))
        .frame(width: 22, height: 22)
        .contentShape(Rectangle())
      }
      .menuStyle(.borderlessButton)
      .frame(width: 22, height: 22)
      .disabled(!actionsAvailable)
      .accessibilityLabel("Split layout, \(pair.layout.label)")
      .accessibilityIdentifier("limitless-split-layout")

      Menu {
        Button("25/75") {
          _ = setRatio(windowID, pair.firstTabID, pair.secondTabID, 0.25)
        }
        .accessibilityIdentifier("limitless-split-ratio-25")
        Button("50/50") {
          _ = setRatio(windowID, pair.firstTabID, pair.secondTabID, 0.50)
        }
        .accessibilityIdentifier("limitless-split-ratio-50")
        Button("75/25") {
          _ = setRatio(windowID, pair.firstTabID, pair.secondTabID, 0.75)
        }
        .accessibilityIdentifier("limitless-split-ratio-75")
      } label: {
        Text("\(ratioPercent)")
          .font(.system(size: 9, weight: .semibold))
          .frame(width: 22, height: 22)
          .contentShape(Rectangle())
      }
      .menuStyle(.borderlessButton)
      .frame(width: 22, height: 22)
      .disabled(!actionsAvailable)
      .accessibilityLabel("Split ratio, \(ratioPercent) percent for first pane")
      .accessibilityIdentifier("limitless-split-ratio")

      Button {
        _ = swap(windowID, pair.firstTabID, pair.secondTabID)
      } label: {
        Image(systemName: "arrow.left.arrow.right")
          .font(.system(size: 10, weight: .semibold))
          .frame(width: 22, height: 22)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .disabled(!actionsAvailable)
      .accessibilityLabel("Swap pane order")
      .accessibilityIdentifier("limitless-split-swap")

      Button {
        _ = unsplit(windowID, pair.firstTabID, pair.secondTabID)
      } label: {
        Image(systemName: "rectangle")
          .font(.system(size: 10, weight: .semibold))
          .frame(width: 22, height: 22)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .disabled(!actionsAvailable)
      .accessibilityLabel("Unsplit tabs")
      .accessibilityIdentifier("limitless-split-unsplit")
    }
    .padding(.leading, 8)
    .padding(.trailing, 4)
    .frame(maxWidth: .infinity, minHeight: 24, maxHeight: 24)
    .background {
      Rectangle()
        .fill(Color.primary.opacity(0.06))
    }
    .overlay(alignment: .bottom) {
      Rectangle()
        .fill(Color.primary.opacity(0.12))
        .frame(height: 1)
        .accessibilityHidden(true)
    }
    .opacity(actionsAvailable ? 1 : 0.55)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(
      "Active split, \(pair.layout.label), \(ratioPercent) percent for first pane"
    )
    .accessibilityValue(actionsAvailable ? "Available" : "Actions unavailable")
  }
}

private struct LimitlessAgentRow: View {
  let agent: LimitlessAgent

  private var statusColor: Color {
    switch agent.status {
    case "working":
      return .blue
    case "blocked":
      return .orange
    case "done":
      return .green
    case "idle":
      return .secondary
    default:
      return .gray
    }
  }

  var body: some View {
    HStack(spacing: 7) {
      Rectangle()
        .fill(statusColor)
        .frame(width: 6, height: 6)
        .accessibilityHidden(true)
      VStack(alignment: .leading, spacing: 1) {
        Text(agent.terminalTitle)
          .font(.system(size: 12, weight: agent.isFocused ? .medium : .regular))
          .lineLimit(1)
          .truncationMode(.tail)
        Text("\(agent.agent) · \(agent.workspaceLabel) · \(agent.status)")
          .font(.system(size: 10, weight: .regular))
          .foregroundStyle(.secondary)
          .lineLimit(1)
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 8)
    .frame(height: 32)
    .background {
      Rectangle()
        .fill(agent.isFocused ? Color.primary.opacity(0.08) : .clear)
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(
      "\(agent.terminalTitle), \(agent.agent), \(agent.status), workspace \(agent.workspaceLabel)"
        + (agent.isFocused ? ", focused in Herdr" : "")
    )
    .accessibilityIdentifier("limitless-agent-\(agent.id)")
  }
}

private struct LimitlessAgentSection: View {
  @ObservedObject var model: LimitlessTabRailModel
  let layoutProbeStore: LimitlessLayoutProbeStore

  private var stateMessage: String? {
    switch model.agentSourceState {
    case .loading:
      return "Loading Herdr agents"
    case .ready:
      return model.agents.isEmpty ? "No live agents" : nil
    case .missingExecutable:
      return "Herdr is not installed"
    case .permissionDenied:
      return "Herdr access denied"
    case .unavailable:
      return "Herdr unavailable"
    case .timedOut:
      return "Herdr did not respond"
    case .malformedResponse:
      return "Herdr returned invalid data"
    case .incompatibleProtocol:
      return "Herdr update required"
    case .privateProfileDisabled:
      return "Agents are unavailable in private windows"
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      Text("Agents")
        .font(.system(size: 11, weight: .semibold))
        .lineLimit(1)
        .frame(height: 24)
        .accessibilityAddTraits(.isHeader)

      if let stateMessage {
        Text(stateMessage)
          .font(.system(size: 11, weight: .regular))
          .foregroundStyle(.secondary)
          .padding(.horizontal, 8)
          .frame(minHeight: 28, alignment: .leading)
          .accessibilityIdentifier("limitless-agent-state")
      } else {
        ScrollView {
          LazyVStack(spacing: 2) {
            ForEach(model.agents) { agent in
              LimitlessAgentRow(agent: agent)
            }
          }
          .background {
            GeometryReader { geometry in
              LimitlessViewportProbe(role: .agentContent, store: layoutProbeStore)
                .frame(width: geometry.size.width, height: geometry.size.height)
            }
          }
        }
        .frame(height: min(max(CGFloat(model.agents.count) * 34, 32), 160))
        .background {
          GeometryReader { geometry in
            LimitlessViewportProbe(role: .agentViewport, store: layoutProbeStore)
              .frame(width: geometry.size.width, height: geometry.size.height)
          }
        }
      }
    }
    .accessibilityElement(children: .contain)
    .accessibilityIdentifier("limitless-agent-list")
  }
}

private struct LimitlessWorkspaceSwitcher: View {
  @ObservedObject var model: LimitlessTabRailModel
  let layoutProbeStore: LimitlessLayoutProbeStore
  let createWorkspace: (String) -> String?
  let switchWorkspace: (String) -> Bool

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 4) {
        Text("Workspaces")
          .font(.system(size: 11, weight: .semibold))
          .lineLimit(1)
          .frame(height: 24)
          .accessibilityAddTraits(.isHeader)
        Spacer(minLength: 0)
        Button {
          _ = createWorkspace("Workspace \(model.workspaces.count + 1)")
        } label: {
          Image(systemName: "plus")
            .font(.system(size: 10, weight: .semibold))
            .frame(width: 22, height: 22)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Create workspace")
        .accessibilityIdentifier("limitless-workspace-create")
        .background {
          GeometryReader { geometry in
            LimitlessViewportProbe(role: .workspaceCreateControl, store: layoutProbeStore)
              .frame(width: geometry.size.width, height: geometry.size.height)
          }
        }
      }
      .background {
        GeometryReader { geometry in
          LimitlessViewportProbe(role: .workspaceHeader, store: layoutProbeStore)
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
      }

      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 2) {
          ForEach(Array(model.workspaces.enumerated()), id: \.element.id) { index, workspace in
            Button {
              _ = switchWorkspace(workspace.id)
            } label: {
              HStack(spacing: 4) {
                Rectangle()
                  .fill(workspace.isActive ? Color.accentColor : Color.clear)
                  .frame(width: 2, height: 14)
                Text(workspace.name)
                  .font(.system(size: 11, weight: workspace.isActive ? .semibold : .regular))
                  .lineLimit(1)
              }
              .padding(.horizontal, 5)
              .frame(height: 24)
              .background {
                Rectangle()
                  .fill(workspace.isActive ? Color.primary.opacity(0.10) : .clear)
              }
              .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(workspace.name)
            .accessibilityValue(
              workspace.isActive
                ? "Selected, \(index + 1) of \(model.workspaces.count)"
                : "\(index + 1) of \(model.workspaces.count)"
            )
            .accessibilityAddTraits(workspace.isActive ? [.isSelected] : [])
            .accessibilityIdentifier("limitless-workspace-\(workspace.id)")
          }
        }
        .background {
          GeometryReader { geometry in
            LimitlessViewportProbe(role: .workspaceSwitcherContent, store: layoutProbeStore)
              .frame(width: geometry.size.width, height: geometry.size.height)
          }
        }
      }
      .frame(height: 24)
      .background {
        GeometryReader { geometry in
          LimitlessViewportProbe(role: .workspaceSwitcherViewport, store: layoutProbeStore)
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
      }
      .accessibilityLabel("Switch workspace")
      .accessibilityIdentifier("limitless-workspace-switcher")
    }
  }
}

private struct LimitlessSwiftUITabRail: View {
  @ObservedObject var model: LimitlessTabRailModel
  let layoutProbeStore: LimitlessLayoutProbeStore
  let activate: (Int32) -> Bool
  let close: (Int32) -> Bool
  let createWorkspace: (String) -> String?
  let switchWorkspace: (String) -> Bool
  let moveTab: (Int32, String) -> Bool
  let splitEligibility: (String, String, [String]) -> [String: String]
  let createSplit: (String, String, String) -> Bool
  let focusSplit: (String, String, String, String) -> Bool
  let setSplitLayout: (String, String, String, String) -> Bool
  let setSplitRatio: (String, String, String, Double) -> Bool
  let swapSplit: (String, String, String) -> Bool
  let unsplit: (String, String, String) -> Bool

  @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
  @Environment(\.colorScheme) private var colorScheme

  private var background: AnyShapeStyle {
    let colors: [Color]
    if colorScheme == .dark {
      colors = [
        Color(red: 23 / 255, green: 26 / 255, blue: 34 / 255),
        Color(red: 17 / 255, green: 19 / 255, blue: 24 / 255),
      ]
    } else {
      colors = [
        Color(red: 247 / 255, green: 245 / 255, blue: 252 / 255),
        Color(red: 236 / 255, green: 239 / 255, blue: 247 / 255),
      ]
    }
    if reduceTransparency {
      return AnyShapeStyle(colors[0])
    }
    return AnyShapeStyle(
      LinearGradient(colors: colors, startPoint: .topLeading, endPoint: .bottomTrailing))
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      if model.leaderArmed {
        LimitlessLeaderStatus()
      }

      if model.workspaceProjection.enabled {
        LimitlessWorkspaceSwitcher(
          model: model,
          layoutProbeStore: layoutProbeStore,
          createWorkspace: createWorkspace,
          switchWorkspace: switchWorkspace)
      }

      VStack(alignment: .leading, spacing: 4) {
        Text("Tabs")
          .font(.system(size: 11, weight: .semibold))
          .lineLimit(1)
          .frame(height: 24)
          .accessibilityAddTraits(.isHeader)

        if let activeSplitPair = model.activeSplitPair {
          LimitlessSplitControls(
            windowID: model.workspaceProjection.windowID,
            pair: activeSplitPair,
            actionsAvailable: model.workspaceProjection.splitActionsAvailable,
            setLayout: setSplitLayout,
            setRatio: setSplitRatio,
            swap: swapSplit,
            unsplit: unsplit)
        }

        ScrollView {
          LazyVStack(spacing: 2) {
            let unsplitTabs = model.tabs.filter { tab in
              tab.productID != nil
                && model.splitPair(forProductID: tab.productID) == nil
            }
            ForEach(Array(model.tabs.enumerated()), id: \.element.id) { index, tab in
              let splitPair = model.splitPair(forProductID: tab.productID)
              LimitlessTabRow(
                tab: tab,
                position: index + 1,
                count: model.tabs.count,
                workspaces: model.workspaces,
                splitPair: splitPair,
                splitCandidates: unsplitTabs,
                splitActionsAvailable: model.workspaceProjection.splitActionsAvailable,
                windowID: model.workspaceProjection.windowID,
                activate: activate,
                close: close,
                move: moveTab,
                createSplit: createSplit,
                focusSplit: focusSplit,
                splitEligibility: splitEligibility,
                density: model.railDensity,
                layoutProbeStore: layoutProbeStore,
                isFirstRow: index == 0)
            }
          }
          .background {
            GeometryReader { geometry in
              LimitlessViewportProbe(role: .workspaceContent, store: layoutProbeStore)
                .frame(width: geometry.size.width, height: geometry.size.height)
            }
          }
        }
        .frame(minHeight: 80, maxHeight: .infinity)
        .background {
          GeometryReader { geometry in
            LimitlessViewportProbe(role: .workspaceViewport, store: layoutProbeStore)
              .frame(width: geometry.size.width, height: geometry.size.height)
          }
        }
        .accessibilityLabel("Tabs")
        .accessibilityIdentifier("limitless-workspace-tab-list")
      }

      Rectangle()
        .fill(Color.primary.opacity(0.10))
        .frame(height: 1)
        .accessibilityHidden(true)

      LimitlessAgentSection(model: model, layoutProbeStore: layoutProbeStore)
    }
    .padding(.top, 40)
    .padding(.horizontal, 8)
    .padding(.bottom, 8)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(background)
    .overlay(alignment: .trailing) {
      Rectangle()
        .fill(Color.primary.opacity(0.10))
        .frame(width: 1)
        .accessibilityHidden(true)
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Limitless sidebar")
    .accessibilityIdentifier("limitless-swiftui-boundary-rail")
  }
}

@MainActor
@objc(LimitlessSwiftUITabRailView)
public final class LimitlessSwiftUITabRailView: NSView {
  private let model: LimitlessTabRailModel
  private let activationHandler: (Int32) -> Bool
  private let closeHandler: (Int32) -> Bool
  private let createWorkspaceHandler: (String) -> String?
  private let switchWorkspaceHandler: (String) -> Bool
  private let moveTabHandler: (Int32, String) -> Bool
  private let splitEligibilityHandler: (String, String, [String]) -> [String: String]
  private let createSplitHandler: (String, String, String) -> Bool
  private let focusSplitHandler: (String, String, String, String) -> Bool
  private let setSplitLayoutHandler: (String, String, String, String) -> Bool
  private let setSplitRatioHandler: (String, String, String, Double) -> Bool
  private let swapSplitHandler: (String, String, String) -> Bool
  private let unsplitHandler: (String, String, String) -> Bool
  private let layoutProbeStore: LimitlessLayoutProbeStore
  private let hostingView: NSHostingView<LimitlessSwiftUITabRail>

  fileprivate init(
    activationHandler: @escaping (Int32) -> Bool,
    closeHandler: @escaping (Int32) -> Bool,
    createWorkspaceHandler: @escaping (String) -> String?,
    switchWorkspaceHandler: @escaping (String) -> Bool,
    moveTabHandler: @escaping (Int32, String) -> Bool,
    splitEligibilityHandler: @escaping (String, String, [String]) -> [String: String],
    createSplitHandler: @escaping (String, String, String) -> Bool,
    focusSplitHandler: @escaping (String, String, String, String) -> Bool,
    setSplitLayoutHandler: @escaping (String, String, String, String) -> Bool,
    setSplitRatioHandler: @escaping (String, String, String, Double) -> Bool,
    swapSplitHandler: @escaping (String, String, String) -> Bool,
    unsplitHandler: @escaping (String, String, String) -> Bool
  ) {
    let model = LimitlessTabRailModel()
    let layoutProbeStore = LimitlessLayoutProbeStore()
    self.model = model
    self.activationHandler = activationHandler
    self.closeHandler = closeHandler
    self.createWorkspaceHandler = createWorkspaceHandler
    self.switchWorkspaceHandler = switchWorkspaceHandler
    self.moveTabHandler = moveTabHandler
    self.splitEligibilityHandler = splitEligibilityHandler
    self.createSplitHandler = createSplitHandler
    self.focusSplitHandler = focusSplitHandler
    self.setSplitLayoutHandler = setSplitLayoutHandler
    self.setSplitRatioHandler = setSplitRatioHandler
    self.swapSplitHandler = swapSplitHandler
    self.unsplitHandler = unsplitHandler
    self.layoutProbeStore = layoutProbeStore
    hostingView = NSHostingView(
      rootView: LimitlessSwiftUITabRail(
        model: model,
        layoutProbeStore: layoutProbeStore,
        activate: activationHandler,
        close: closeHandler,
        createWorkspace: createWorkspaceHandler,
        switchWorkspace: switchWorkspaceHandler,
        moveTab: moveTabHandler,
        splitEligibility: splitEligibilityHandler,
        createSplit: createSplitHandler,
        focusSplit: focusSplitHandler,
        setSplitLayout: setSplitLayoutHandler,
        setSplitRatio: setSplitRatioHandler,
        swapSplit: swapSplitHandler,
        unsplit: unsplitHandler))
    super.init(frame: .zero)

    addSubview(hostingView)
    hostingView.translatesAutoresizingMaskIntoConstraints = false
    NSLayoutConstraint.activate([
      hostingView.leadingAnchor.constraint(equalTo: leadingAnchor),
      hostingView.trailingAnchor.constraint(equalTo: trailingAnchor),
      hostingView.topAnchor.constraint(equalTo: topAnchor),
      hostingView.bottomAnchor.constraint(equalTo: bottomAnchor),
    ])
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) {
    fatalError("init(coder:) is unavailable")
  }

  @objc(
    updateWithWorkspacesEnabled:windowIdentifier:workspaceIdentifiers:workspaceNames:
    defaultWorkspaces:activeWorkspaceIdentifier:tabIdentifiers:productTabIdentifiers:titles:
    activeTabIdentifier:splitActionsAvailable:splitPairIdentifiers:splitFirstTabIdentifiers:
    splitSecondTabIdentifiers:splitActiveTabIdentifiers:splitLayouts:splitRatios:
  )
  public func updateWorkspaceProjection(
    workspacesEnabled: Bool,
    windowIdentifier: String,
    workspaceIdentifiers: [String],
    workspaceNames: [String],
    defaultWorkspaces: [NSNumber],
    activeWorkspaceIdentifier: String?,
    tabIdentifiers: [NSNumber],
    productTabIdentifiers: [String],
    titles: [String],
    activeTabIdentifier: NSNumber?,
    splitActionsAvailable: Bool,
    splitPairIdentifiers: [String],
    splitFirstTabIdentifiers: [String],
    splitSecondTabIdentifiers: [String],
    splitActiveTabIdentifiers: [String],
    splitLayouts: [String],
    splitRatios: [NSNumber]
  ) {
    model.replaceWorkspaceProjection(
      enabled: workspacesEnabled,
      windowIdentifier: windowIdentifier,
      workspaceIdentifiers: workspaceIdentifiers,
      workspaceNames: workspaceNames,
      defaultWorkspaces: defaultWorkspaces,
      activeWorkspaceIdentifier: activeWorkspaceIdentifier,
      identifiers: tabIdentifiers,
      productTabIdentifiers: productTabIdentifiers,
      titles: titles,
      activeIdentifier: activeTabIdentifier,
      splitActionsAvailable: splitActionsAvailable,
      splitPairIdentifiers: splitPairIdentifiers,
      splitFirstTabIdentifiers: splitFirstTabIdentifiers,
      splitSecondTabIdentifiers: splitSecondTabIdentifiers,
      splitActiveTabIdentifiers: splitActiveTabIdentifiers,
      splitLayouts: splitLayouts,
      splitRatios: splitRatios)
  }

  @objc(
    updateWithAgentPaneIdentifiers:workspaceIdentifiers:workspaceLabels:agents:statuses:
    terminalTitles:focused:sourceState:
  )
  public func updateAgents(
    paneIdentifiers: [String],
    workspaceIdentifiers: [String],
    workspaceLabels: [String],
    agents: [String],
    statuses: [String],
    terminalTitles: [String],
    focused: [NSNumber],
    sourceState: String
  ) {
    model.replaceAgents(
      paneIdentifiers: paneIdentifiers,
      workspaceIdentifiers: workspaceIdentifiers,
      workspaceLabels: workspaceLabels,
      agents: agents,
      statuses: statuses,
      terminalTitles: terminalTitles,
      focused: focused,
      sourceState: sourceState)
  }

  @objc(updateLeaderEnabled:armed:)
  public func updateLeader(enabled: Bool, armed: Bool) {
    model.replaceLeader(enabled: enabled, armed: armed)
  }

  @objc(updateRailDensity:)
  @discardableResult
  public func updateRailDensity(_ value: String) -> Bool {
    model.replaceRailDensity(value)
  }

  @objc public var railDensityForTesting: String {
    model.railDensity.rawValue
  }

  @objc public var firstTabRowFrameForTesting: NSRect {
    layoutProbeFrame(for: .firstTabRow)
  }

  @objc public var tabIdentifiersForTesting: [NSNumber] {
    model.tabs.map { NSNumber(value: $0.id) }
  }

  @objc public var tabProductIdentifiersForTesting: [String] {
    model.tabs.compactMap(\.productID)
  }

  @objc public var workspacesEnabledForTesting: Bool {
    model.workspaceProjection.enabled
  }

  @objc public var workspaceWindowIdentifierForTesting: String {
    model.workspaceProjection.windowID
  }

  @objc public var workspaceIdentifiersForTesting: [String] {
    model.workspaces.map(\.id)
  }

  @objc public var workspaceNamesForTesting: [String] {
    model.workspaces.map(\.name)
  }

  @objc public var defaultWorkspacesForTesting: [NSNumber] {
    model.workspaces.map { NSNumber(value: $0.isDefault) }
  }

  @objc public var activeWorkspaceIdentifierForTesting: String? {
    model.workspaces.first(where: \.isActive)?.id
  }

  @objc public var tabTitlesForTesting: [String] {
    model.tabs.map(\.title)
  }

  @objc public var activeTabIdentifierForTesting: NSNumber? {
    model.tabs.first(where: \.isActive).map { NSNumber(value: $0.id) }
  }

  @objc public var splitActionsAvailableForTesting: Bool {
    model.workspaceProjection.splitActionsAvailable
  }

  @objc public var splitPairIdentifiersForTesting: [String] {
    model.workspaceProjection.splitPairs.map(\.id)
  }

  @objc public var splitFirstTabIdentifiersForTesting: [String] {
    model.workspaceProjection.splitPairs.map(\.firstTabID)
  }

  @objc public var splitSecondTabIdentifiersForTesting: [String] {
    model.workspaceProjection.splitPairs.map(\.secondTabID)
  }

  @objc public var splitActiveTabIdentifiersForTesting: [String] {
    model.workspaceProjection.splitPairs.map { $0.activeTabID ?? "" }
  }

  @objc public var splitLayoutsForTesting: [String] {
    model.workspaceProjection.splitPairs.map { $0.layout.rawValue }
  }

  @objc public var splitRatiosForTesting: [NSNumber] {
    model.workspaceProjection.splitPairs.map { NSNumber(value: $0.ratio) }
  }

  @objc public var leaderEnabledForTesting: Bool {
    model.leaderEnabled
  }

  @objc public var leaderArmedForTesting: Bool {
    model.leaderArmed
  }

  @objc public var leaderStatusTextForTesting: String {
    model.leaderStatusText
  }

  @objc public var agentPaneIdentifiersForTesting: [String] {
    model.agents.map(\.id)
  }

  @objc public var agentWorkspaceIdentifiersForTesting: [String] {
    model.agents.map(\.workspaceID)
  }

  @objc public var agentWorkspaceLabelsForTesting: [String] {
    model.agents.map(\.workspaceLabel)
  }

  @objc public var agentKindsForTesting: [String] {
    model.agents.map(\.agent)
  }

  @objc public var agentStatusesForTesting: [String] {
    model.agents.map(\.status)
  }

  @objc public var agentTitlesForTesting: [String] {
    model.agents.map(\.terminalTitle)
  }

  @objc public var agentFocusedForTesting: [NSNumber] {
    model.agents.map { NSNumber(value: $0.isFocused) }
  }

  @objc public var agentSourceStateForTesting: String {
    model.agentSourceState.rawValue
  }

  @objc public var workspaceViewportFrameForTesting: NSRect {
    layoutProbeFrame(for: .workspaceViewport)
  }

  @objc public var workspaceHeaderFrameForTesting: NSRect {
    layoutProbeFrame(for: .workspaceHeader)
  }

  @objc public var workspaceCreateControlFrameForTesting: NSRect {
    layoutProbeFrame(for: .workspaceCreateControl)
  }

  @objc public var workspaceSwitcherViewportFrameForTesting: NSRect {
    layoutProbeFrame(for: .workspaceSwitcherViewport)
  }

  @objc public var workspaceSwitcherContentFrameForTesting: NSRect {
    layoutProbeFrame(for: .workspaceSwitcherContent)
  }

  @objc public var workspaceContentFrameForTesting: NSRect {
    layoutProbeFrame(for: .workspaceContent)
  }

  @objc public var agentViewportFrameForTesting: NSRect {
    layoutProbeFrame(for: .agentViewport)
  }

  @objc public var agentContentFrameForTesting: NSRect {
    layoutProbeFrame(for: .agentContent)
  }

  @objc public var railFrameForTesting: NSRect {
    bounds
  }

  private func layoutProbeFrame(for role: LimitlessLayoutProbeRole) -> NSRect {
    window?.layoutIfNeeded()
    layoutSubtreeIfNeeded()
    hostingView.layoutSubtreeIfNeeded()
    return layoutProbeStore.frame(for: role, in: self)
  }

  @objc public func activateTabForTesting(_ tabIdentifier: Int32) -> Bool {
    activationHandler(tabIdentifier)
  }

  @objc public func closeTabForTesting(_ tabIdentifier: Int32) -> Bool {
    closeHandler(tabIdentifier)
  }

  @objc public func createWorkspaceForTesting(_ name: String) -> String? {
    createWorkspaceHandler(name)
  }

  @objc public func switchWorkspaceForTesting(_ workspaceIdentifier: String) -> Bool {
    switchWorkspaceHandler(workspaceIdentifier)
  }

  @objc public func moveTabForTesting(
    _ tabIdentifier: Int32,
    toWorkspace workspaceIdentifier: String
  ) -> Bool {
    moveTabHandler(tabIdentifier, workspaceIdentifier)
  }

  @objc(
    createSplitForTestingWithWindowIdentifier:firstTabIdentifier:secondTabIdentifier:
  )
  public func createSplitForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    secondTabIdentifier: String
  ) -> Bool {
    createSplitHandler(windowIdentifier, firstTabIdentifier, secondTabIdentifier)
  }

  @objc(
    splitEligibilityForTestingWithWindowIdentifier:firstTabIdentifier:
    destinationTabIdentifiers:
  )
  public func splitEligibilityForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    destinationTabIdentifiers: [String]
  ) -> [String: String] {
    splitEligibilityHandler(
      windowIdentifier,
      firstTabIdentifier,
      destinationTabIdentifiers)
  }

  @objc(
    focusSplitForTestingWithWindowIdentifier:firstTabIdentifier:secondTabIdentifier:
    targetTabIdentifier:
  )
  public func focusSplitForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    secondTabIdentifier: String,
    targetTabIdentifier: String
  ) -> Bool {
    focusSplitHandler(
      windowIdentifier,
      firstTabIdentifier,
      secondTabIdentifier,
      targetTabIdentifier)
  }

  @objc(
    setSplitLayoutForTestingWithWindowIdentifier:firstTabIdentifier:secondTabIdentifier:
    layout:
  )
  public func setSplitLayoutForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    secondTabIdentifier: String,
    layout: String
  ) -> Bool {
    setSplitLayoutHandler(
      windowIdentifier,
      firstTabIdentifier,
      secondTabIdentifier,
      layout)
  }

  @objc(
    setSplitRatioForTestingWithWindowIdentifier:firstTabIdentifier:secondTabIdentifier:ratio:
  )
  public func setSplitRatioForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    secondTabIdentifier: String,
    ratio: Double
  ) -> Bool {
    setSplitRatioHandler(
      windowIdentifier,
      firstTabIdentifier,
      secondTabIdentifier,
      ratio)
  }

  @objc(
    swapSplitForTestingWithWindowIdentifier:firstTabIdentifier:secondTabIdentifier:
  )
  public func swapSplitForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    secondTabIdentifier: String
  ) -> Bool {
    swapSplitHandler(windowIdentifier, firstTabIdentifier, secondTabIdentifier)
  }

  @objc(
    unsplitForTestingWithWindowIdentifier:firstTabIdentifier:secondTabIdentifier:
  )
  public func unsplitForTesting(
    windowIdentifier: String,
    firstTabIdentifier: String,
    secondTabIdentifier: String
  ) -> Bool {
    unsplitHandler(windowIdentifier, firstTabIdentifier, secondTabIdentifier)
  }
}

@MainActor
@objc(LimitlessSwiftUIRailFactory)
public final class LimitlessSwiftUIRailFactory: NSObject {
  @objc(
    makeTabRailViewWithActivationHandler:closeHandler:createWorkspaceHandler:
    switchWorkspaceHandler:moveTabHandler:splitEligibilityHandler:createSplitHandler:
    focusSplitHandler:setSplitLayoutHandler:setSplitRatioHandler:swapSplitHandler:
    unsplitHandler:
  )
  public static func makeTabRailView(
    activationHandler: @escaping (Int32) -> Bool,
    closeHandler: @escaping (Int32) -> Bool,
    createWorkspaceHandler: @escaping (String) -> String?,
    switchWorkspaceHandler: @escaping (String) -> Bool,
    moveTabHandler: @escaping (Int32, String) -> Bool,
    splitEligibilityHandler: @escaping (String, String, [String]) -> [String: String],
    createSplitHandler: @escaping (String, String, String) -> Bool,
    focusSplitHandler: @escaping (String, String, String, String) -> Bool,
    setSplitLayoutHandler: @escaping (String, String, String, String) -> Bool,
    setSplitRatioHandler: @escaping (String, String, String, Double) -> Bool,
    swapSplitHandler: @escaping (String, String, String) -> Bool,
    unsplitHandler: @escaping (String, String, String) -> Bool
  ) -> LimitlessSwiftUITabRailView {
    LimitlessSwiftUITabRailView(
      activationHandler: activationHandler,
      closeHandler: closeHandler,
      createWorkspaceHandler: createWorkspaceHandler,
      switchWorkspaceHandler: switchWorkspaceHandler,
      moveTabHandler: moveTabHandler,
      splitEligibilityHandler: splitEligibilityHandler,
      createSplitHandler: createSplitHandler,
      focusSplitHandler: focusSplitHandler,
      setSplitLayoutHandler: setSplitLayoutHandler,
      setSplitRatioHandler: setSplitRatioHandler,
      swapSplitHandler: swapSplitHandler,
      unsplitHandler: unsplitHandler)
  }
}
