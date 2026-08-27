// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_swiftui_rail_host.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#import "LimitlessSwiftUIBoundaryProbe.h"
#include "base/callback_list.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/process/process_handle.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "shell/agents/agent_presence_registry.h"
#include "shell/control_plane/limitless_profile_preference_backend.h"
#include "shell/shortcuts/limitless_command_dispatcher_delegate_mac.h"
#include "shell/shortcuts/limitless_leader_shortcut_controller.h"
#include "shell/splits/limitless_split_pane_controller.h"
#include "shell/workspaces/limitless_workspace_controller.h"
#include "shell/workspaces/limitless_workspace_session_persistence.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/layout_types.h"
#include "ui/views/widget/root_view.h"
#include "ui/views/widget/widget.h"

namespace limitless {
namespace {

int g_attached_host_count = 0;

class SwiftUITabRailHost;

std::vector<SwiftUITabRailHost*>& GetLiveHosts() {
  static base::NoDestructor<std::vector<SwiftUITabRailHost*>> hosts;
  return *hosts;
}

SwiftUILayoutRectForTesting LayoutRectForTesting(NSRect rect) {
  return SwiftUILayoutRectForTesting{
      .x = NSMinX(rect),
      .y = NSMinY(rect),
      .width = NSWidth(rect),
      .height = NSHeight(rect),
  };
}

NSArray* AccessibilityChildrenForTraversal(id root) {
  if ([root respondsToSelector:@selector(
                                   accessibilityChildrenInNavigationOrder)]) {
    NSArray* ordered = [root accessibilityChildrenInNavigationOrder];
    if (ordered.count > 0) {
      return ordered;
    }
  }
  if ([root respondsToSelector:@selector(accessibilityChildren)]) {
    return NSAccessibilityUnignoredChildren([root accessibilityChildren]
                                                ?: @[]);
  }
  return @[];
}

void MaterializeNativeAccessibilityTreeForTesting() {
  AXUIElementRef application =
      AXUIElementCreateApplication(base::GetCurrentProcId());
  CFTypeRef windows = nullptr;
  AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &windows);
  if (windows) {
    CFRelease(windows);
  }
  CFRelease(application);
}

id FindAccessibilityElement(id root,
                            NSString* identifier,
                            NSHashTable<id>* visited,
                            size_t depth,
                            bool traverse_shown_menu = true) {
  if (!root || depth > 64 || visited.count >= 4096 ||
      [visited containsObject:root]) {
    return nil;
  }
  [visited addObject:root];
  id native_identifier =
      [root respondsToSelector:@selector(accessibilityIdentifier)]
          ? [root accessibilityIdentifier]
          : nil;
  if ([native_identifier isKindOfClass:[NSString class]] &&
      [native_identifier isEqualToString:identifier]) {
    return root;
  }
  // AppKit exposes both an NSPopUpButton's configured NSMenu and a distinct
  // shown-menu accessibility representation while tracking. Once traversal
  // is rooted at either NSMenu, following accessibilityShownMenu again walks
  // the same logical commands through a second object graph.
  const bool entered_menu = [root isKindOfClass:[NSMenu class]];
  if (traverse_shown_menu && !entered_menu &&
      [root respondsToSelector:@selector(accessibilityShownMenu)]) {
    id shown_menu = [root accessibilityShownMenu];
    if (shown_menu) {
      if (id match = FindAccessibilityElement(shown_menu, identifier, visited,
                                              depth + 1, false)) {
        return match;
      }
    }
  }
  NSArray* children = AccessibilityChildrenForTraversal(root);
  for (id child in children) {
    if (id match =
            FindAccessibilityElement(child, identifier, visited, depth + 1,
                                     traverse_shown_menu && !entered_menu)) {
      return match;
    }
  }
  return nil;
}

id FindAccessibilityElement(NSView* root, const std::string& identifier) {
  MaterializeNativeAccessibilityTreeForTesting();
  NSHashTable<id>* visited = [NSHashTable
      hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
  NSString* native_identifier = base::SysUTF8ToNSString(identifier);
  for (id seed in NSAccessibilityUnignoredChildrenForOnlyChild(root)) {
    if (id match =
            FindAccessibilityElement(seed, native_identifier, visited, 0)) {
      return match;
    }
  }
  return nil;
}

id ShownAccessibilityMenu(id menu_button) {
  if ([menu_button isKindOfClass:[NSPopUpButtonCell class]]) {
    NSMenu* shown_menu = [(NSPopUpButtonCell*)menu_button menu];
    if (shown_menu.numberOfItems > 0) {
      return shown_menu;
    }
  }
  if ([menu_button respondsToSelector:@selector(accessibilityShownMenu)]) {
    id shown_menu = [menu_button accessibilityShownMenu];
    if (shown_menu && AccessibilityChildrenForTraversal(shown_menu).count > 0) {
      return shown_menu;
    }
  }
  return nil;
}

void CollectAccessibilityIdentifiers(id root,
                                     NSHashTable<id>* visited,
                                     size_t depth,
                                     size_t node_cap,
                                     std::vector<std::string>* identifiers,
                                     bool traverse_shown_menu = true) {
  if (!root || depth > 64 || visited.count >= node_cap ||
      [visited containsObject:root]) {
    return;
  }
  [visited addObject:root];
  if ([root respondsToSelector:@selector(accessibilityIdentifier)]) {
    id identifier = [root accessibilityIdentifier];
    if ([identifier isKindOfClass:[NSString class]] &&
        [identifier length] > 0) {
      identifiers->push_back(base::SysNSStringToUTF8(identifier));
    }
  }
  const bool entered_menu = [root isKindOfClass:[NSMenu class]];
  if (traverse_shown_menu && !entered_menu &&
      [root respondsToSelector:@selector(accessibilityShownMenu)]) {
    id shown_menu = [root accessibilityShownMenu];
    if (shown_menu) {
      CollectAccessibilityIdentifiers(shown_menu, visited, depth + 1, node_cap,
                                      identifiers, false);
    }
  }
  NSArray* children = AccessibilityChildrenForTraversal(root);
  for (id child in children) {
    CollectAccessibilityIdentifiers(child, visited, depth + 1, node_cap,
                                    identifiers,
                                    traverse_shown_menu && !entered_menu);
  }
}

SwiftUIAccessibilityControlForTesting AccessibilityControlForElement(
    id element) {
  SwiftUIAccessibilityControlForTesting control;
  if ([element respondsToSelector:@selector(accessibilityRole)]) {
    id role = [element accessibilityRole];
    if ([role isKindOfClass:[NSString class]]) {
      control.role = base::SysNSStringToUTF8(role);
    }
  }
  if ([element respondsToSelector:@selector(accessibilityLabel)]) {
    id label = [element accessibilityLabel];
    if ([label isKindOfClass:[NSString class]]) {
      control.label = base::SysNSStringToUTF16(label);
    }
  }
  if (control.label.empty() &&
      [element respondsToSelector:@selector(accessibilityTitle)]) {
    id title = [element accessibilityTitle];
    if ([title isKindOfClass:[NSString class]]) {
      control.label = base::SysNSStringToUTF16(title);
    }
  }
  if ([element respondsToSelector:@selector(accessibilityValue)]) {
    id value = [element accessibilityValue];
    if ([value isKindOfClass:[NSString class]]) {
      control.value = base::SysNSStringToUTF16(value);
    }
  }
  if ([element respondsToSelector:@selector(isAccessibilityEnabled)]) {
    control.enabled = [element isAccessibilityEnabled];
  }
  if ([element respondsToSelector:@selector(isAccessibilityFocused)]) {
    control.focused = [element isAccessibilityFocused];
  }
  if ([element respondsToSelector:@selector(accessibilityPerformPress)] &&
      [element isAccessibilitySelectorAllowed:@selector(
                                                  accessibilityPerformPress)]) {
    control.actions.push_back(
        base::SysNSStringToUTF8(NSAccessibilityPressAction));
  }
  if ([element respondsToSelector:@selector(accessibilityPerformShowMenu)] &&
      [element
          isAccessibilitySelectorAllowed:@selector(
                                             accessibilityPerformShowMenu)]) {
    control.actions.push_back(
        base::SysNSStringToUTF8(NSAccessibilityShowMenuAction));
  }
  return control;
}

class TabTitleObserver final : public content::WebContentsObserver {
 public:
  TabTitleObserver(content::WebContents* contents,
                   base::WeakPtr<SwiftUITabRailHost> host)
      : content::WebContentsObserver(contents), host_(std::move(host)) {}

  void TitleWasSet(content::NavigationEntry* entry) override;

 private:
  base::WeakPtr<SwiftUITabRailHost> host_;
};

class SwiftUITabRailHost final : public views::NativeViewHost,
                                 public AgentPresenceRegistryObserver {
 public:
  explicit SwiftUITabRailHost(BrowserWindowInterface* browser_window)
      : browser_window_(browser_window),
        tab_list_(TabListInterface::From(browser_window)) {
    CHECK(browser_window_);
    CHECK(tab_list_);

    base::WeakPtr<SwiftUITabRailHost> weak_this =
        weak_ptr_factory_.GetWeakPtr();
    native_view_ = [LimitlessSwiftUIRailFactory
        makeTabRailViewWithActivationHandler:^BOOL(int32_t tab_identifier) {
          return weak_this && weak_this->ActivateTab(tab_identifier);
        }
        closeHandler:^BOOL(int32_t tab_identifier) {
          return weak_this && weak_this->CloseTab(tab_identifier);
        }
        createWorkspaceHandler:^NSString*(NSString* name) {
          if (!weak_this) {
            return nil;
          }
          std::optional<std::string> identifier =
              weak_this->CreateWorkspace(base::SysNSStringToUTF16(name));
          return identifier ? base::SysUTF8ToNSString(*identifier) : nil;
        }
        switchWorkspaceHandler:^BOOL(NSString* workspace_identifier) {
          return weak_this &&
                 weak_this->SwitchWorkspace(
                     base::SysNSStringToUTF8(workspace_identifier));
        }
        moveTabHandler:^BOOL(int32_t tab_identifier,
                             NSString* workspace_identifier) {
          return weak_this && weak_this->MoveTabToWorkspace(
                                  tab_identifier, base::SysNSStringToUTF8(
                                                      workspace_identifier));
        }
        splitEligibilityHandler:^NSDictionary<NSString*, NSString*>*(
            NSString* window_identifier, NSString* source_tab_identifier,
            NSArray<NSString*>* destination_tab_identifiers) {
          if (!weak_this) {
            return @{};
          }
          std::vector<std::string> destinations;
          destinations.reserve(destination_tab_identifiers.count);
          for (NSString* identifier in destination_tab_identifiers) {
            destinations.push_back(base::SysNSStringToUTF8(identifier));
          }
          return weak_this->GetSplitEligibility(
              base::SysNSStringToUTF8(window_identifier),
              base::SysNSStringToUTF8(source_tab_identifier), destinations);
        }
        createSplitHandler:^BOOL(NSString* window_identifier,
                                 NSString* first_tab_identifier,
                                 NSString* second_tab_identifier) {
          return weak_this &&
                 weak_this->CreateSplit(
                     base::SysNSStringToUTF8(window_identifier),
                     base::SysNSStringToUTF8(first_tab_identifier),
                     base::SysNSStringToUTF8(second_tab_identifier));
        }
        focusSplitHandler:^BOOL(
            NSString* window_identifier, NSString* first_tab_identifier,
            NSString* second_tab_identifier, NSString* target_tab_identifier) {
          return weak_this &&
                 weak_this->FocusSplit(
                     base::SysNSStringToUTF8(window_identifier),
                     base::SysNSStringToUTF8(first_tab_identifier),
                     base::SysNSStringToUTF8(second_tab_identifier),
                     base::SysNSStringToUTF8(target_tab_identifier));
        }
        setSplitLayoutHandler:^BOOL(
            NSString* window_identifier, NSString* first_tab_identifier,
            NSString* second_tab_identifier, NSString* layout) {
          return weak_this &&
                 weak_this->SetSplitLayout(
                     base::SysNSStringToUTF8(window_identifier),
                     base::SysNSStringToUTF8(first_tab_identifier),
                     base::SysNSStringToUTF8(second_tab_identifier),
                     base::SysNSStringToUTF8(layout));
        }
        setSplitRatioHandler:^BOOL(
            NSString* window_identifier, NSString* first_tab_identifier,
            NSString* second_tab_identifier, double ratio) {
          return weak_this &&
                 weak_this->SetSplitRatio(
                     base::SysNSStringToUTF8(window_identifier),
                     base::SysNSStringToUTF8(first_tab_identifier),
                     base::SysNSStringToUTF8(second_tab_identifier), ratio);
        }
        swapSplitHandler:^BOOL(NSString* window_identifier,
                               NSString* first_tab_identifier,
                               NSString* second_tab_identifier) {
          return weak_this &&
                 weak_this->SwapSplit(
                     base::SysNSStringToUTF8(window_identifier),
                     base::SysNSStringToUTF8(first_tab_identifier),
                     base::SysNSStringToUTF8(second_tab_identifier));
        }
        unsplitHandler:^BOOL(NSString* window_identifier,
                             NSString* first_tab_identifier,
                             NSString* second_tab_identifier) {
          return weak_this &&
                 weak_this->Unsplit(
                     base::SysNSStringToUTF8(window_identifier),
                     base::SysNSStringToUTF8(first_tab_identifier),
                     base::SysNSStringToUTF8(second_tab_identifier));
        }];
    Profile* profile = browser_window_->GetProfile();
    if (!profile->IsOffTheRecord() && profile->GetPrefs() &&
        profile->GetPrefs()->FindPreference(kRailDensityPref)) {
      preference_backend_ =
          std::make_unique<ProfilePreferenceControlBackend>(
              profile,
              base::BindRepeating(&SwiftUITabRailHost::OnRailDensityChanged,
                                  weak_this));
      OnRailDensityChanged(preference_backend_->GetEffectiveValue(
          PreferenceKey::kRailDensity));
    }
    workspace_controller_ = std::make_unique<WorkspaceController>(
        browser_window_,
        CreateWorkspaceSessionPersistence(browser_window_->GetProfile()),
        base::BindRepeating(&SwiftUITabRailHost::OnWorkspaceStateChanged,
                            weak_this));
    split_controller_ = std::make_unique<SplitPaneController>(
        browser_window_,
        base::BindRepeating(&SwiftUITabRailHost::ScheduleProjectionUpdate,
                            weak_this));
    const bool leader_enabled =
        AreLeaderShortcutsEnabled(*base::CommandLine::ForCurrentProcess());
    leader_controller_ = std::make_unique<LeaderShortcutController>(
        leader_enabled,
        base::BindRepeating(&SwiftUITabRailHost::CanHandleLeader,
                            base::Unretained(this)),
        base::BindRepeating(&SwiftUITabRailHost::ExecuteLeaderCommand,
                            base::Unretained(this)),
        base::BindRepeating(&SwiftUITabRailHost::OnLeaderArmedChanged,
                            base::Unretained(this)));
    [native_view_ updateLeaderEnabled:leader_enabled armed:NO];

    SetFocusBehavior(FocusBehavior::ALWAYS);
    GetViewAccessibility().SetRole(ax::mojom::Role::kPane);
    GetViewAccessibility().SetName(u"Limitless sidebar");
    browser_did_close_subscription_ =
        browser_window_->RegisterBrowserDidClose(base::BindRepeating(
            &SwiftUITabRailHost::OnBrowserDidClose, base::Unretained(this)));
    RebuildTitleObservers();
    UpdateSnapshot();
    if (browser_window_->GetProfile()->IsOffTheRecord()) {
      OnHerdrAgentSnapshotChanged(HerdrAgentSnapshot{
          .state = HerdrAgentSourceState::kPrivateProfileDisabled,
      });
    } else {
      GetAgentPresenceRegistry().AddObserver(this);
      observing_agent_presence_ = true;
    }
    GetLiveHosts().push_back(this);
  }

  SwiftUITabRailHost(const SwiftUITabRailHost&) = delete;
  SwiftUITabRailHost& operator=(const SwiftUITabRailHost&) = delete;

  ~SwiftUITabRailHost() override {
    UnregisterPreTargetHandler();
    leader_controller_->Stop();
    StopObservingAgentPresence();
    std::erase(GetLiveHosts(), this);
    if (native_view()) {
      Detach();
      --g_attached_host_count;
    }
  }

  void AddedToWidget() override {
    views::NativeViewHost::AddedToWidget();
    CHECK(!native_view());
    Attach(gfx::NativeView(native_view_));
    pre_target_root_ = GetWidget()->GetRootView();
    pre_target_root_->AddPreTargetHandler(this);
    leader_controller_->SetNativeWindow(GetWidget()->GetNativeWindow());
    ++g_attached_host_count;
  }

  void RemovedFromWidget() override {
    UnregisterPreTargetHandler();
    leader_controller_->SetNativeWindow(gfx::NativeWindow());
    if (native_view()) {
      Detach();
      --g_attached_host_count;
    }
    views::NativeViewHost::RemovedFromWidget();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(kSwiftUITabRailWidth, 400);
  }

  void OnKeyEvent(ui::KeyEvent* event) override {
    if (event->phase() != ui::EP_PRETARGET || event->handled()) {
      return;
    }
    if (leader_controller_->HandleKeyEvent(*event)) {
      event->SetHandled();
      event->StopPropagation();
    }
  }

  void OnBrowserDidClose(BrowserWindowInterface* browser_window) {
    leader_controller_->Stop();
    StopObservingAgentPresence();
    split_controller_->Stop();
    workspace_controller_->Stop();
    preference_backend_.reset();
    tab_list_ = nullptr;
    browser_window_ = nullptr;
    title_observers_.clear();
    UpdateSnapshot();
  }

  void OnHerdrAgentSnapshotChanged(
      const HerdrAgentSnapshot& snapshot) override {
    NSMutableArray<NSString*>* pane_identifiers = [NSMutableArray array];
    NSMutableArray<NSString*>* workspace_identifiers = [NSMutableArray array];
    NSMutableArray<NSString*>* workspace_labels = [NSMutableArray array];
    NSMutableArray<NSString*>* agents = [NSMutableArray array];
    NSMutableArray<NSString*>* statuses = [NSMutableArray array];
    NSMutableArray<NSString*>* terminal_titles = [NSMutableArray array];
    NSMutableArray<NSNumber*>* focused = [NSMutableArray array];
    for (const HerdrAgentPresence& presence : snapshot.agents) {
      [pane_identifiers addObject:base::SysUTF8ToNSString(presence.pane_id)];
      [workspace_identifiers
          addObject:base::SysUTF8ToNSString(presence.workspace_id)];
      [workspace_labels
          addObject:base::SysUTF16ToNSString(presence.workspace_label)];
      [agents addObject:base::SysUTF8ToNSString(presence.agent)];
      [statuses addObject:base::SysUTF8ToNSString(std::string(
                              HerdrAgentStatusName(presence.status)))];
      [terminal_titles
          addObject:base::SysUTF16ToNSString(presence.terminal_title)];
      [focused addObject:@(presence.focused)];
    }
    [native_view_
        updateWithAgentPaneIdentifiers:pane_identifiers
                  workspaceIdentifiers:workspace_identifiers
                       workspaceLabels:workspace_labels
                                agents:agents
                              statuses:statuses
                        terminalTitles:terminal_titles
                               focused:focused
                           sourceState:base::SysUTF8ToNSString(std::string(
                                           HerdrAgentSourceStateName(
                                               snapshot.state)))];
  }

  void OnRailDensityChanged(const EffectivePreferenceValue& effective_value) {
    NSString* value = effective_value.value.rail_density ==
                              RailDensity::kComfortable
                          ? @"comfortable"
                          : @"compact";
    [native_view_ updateRailDensity:value];
  }

  void UpdateSnapshot() {
    NSMutableArray<NSString*>* workspace_identifiers = [NSMutableArray array];
    NSMutableArray<NSString*>* workspace_names = [NSMutableArray array];
    NSMutableArray<NSNumber*>* default_workspaces = [NSMutableArray array];
    NSMutableArray<NSNumber*>* identifiers = [NSMutableArray array];
    NSMutableArray<NSString*>* product_identifiers = [NSMutableArray array];
    NSMutableArray<NSString*>* titles = [NSMutableArray array];
    NSMutableArray<NSString*>* split_pair_identifiers = [NSMutableArray array];
    NSMutableArray<NSString*>* split_first_tab_identifiers =
        [NSMutableArray array];
    NSMutableArray<NSString*>* split_second_tab_identifiers =
        [NSMutableArray array];
    NSMutableArray<NSString*>* split_active_tab_identifiers =
        [NSMutableArray array];
    NSMutableArray<NSString*>* split_layouts = [NSMutableArray array];
    NSMutableArray<NSNumber*>* split_ratios = [NSMutableArray array];
    BOOL workspaces_enabled = NO;
    BOOL split_actions_available = NO;
    bool has_complete_product_tab_identifiers = true;
    NSString* window_identifier = @"";
    NSString* active_workspace_identifier = nil;
    NSNumber* active_identifier = nil;

    if (workspace_controller_) {
      const WorkspaceWindowSnapshot workspace_snapshot =
          workspace_controller_->GetSnapshot();
      workspaces_enabled = workspace_snapshot.enabled;
      window_identifier =
          base::SysUTF8ToNSString(workspace_snapshot.window_identifier);
      if (workspace_snapshot.active_workspace_identifier) {
        active_workspace_identifier = base::SysUTF8ToNSString(
            *workspace_snapshot.active_workspace_identifier);
      }
      for (const WorkspaceProjection& workspace :
           workspace_snapshot.workspaces) {
        [workspace_identifiers
            addObject:base::SysUTF8ToNSString(workspace.identifier)];
        [workspace_names addObject:base::SysUTF16ToNSString(workspace.name)];
        [default_workspaces addObject:@(workspace.is_default)];
      }
      std::map<int32_t, std::string> product_identifier_by_handle;
      std::set<std::string> product_identifiers_seen;
      for (const WorkspaceTabProjection& tab :
           workspace_snapshot.visible_tab_projections) {
        const base::Uuid parsed = base::Uuid::ParseLowercase(tab.identifier);
        const bool canonical =
            parsed.is_valid() && parsed.AsLowercaseString() == tab.identifier;
        const bool unique_product_identifier =
            product_identifiers_seen.insert(tab.identifier).second;
        const bool unique_handle =
            product_identifier_by_handle
                .emplace(tab.handle.raw_value(), tab.identifier)
                .second;
        has_complete_product_tab_identifiers &=
            canonical && unique_product_identifier && unique_handle;
      }
      if (simulate_missing_product_identity_mapping_for_testing_ &&
          !workspace_snapshot.visible_tabs.empty()) {
        product_identifier_by_handle.erase(
            workspace_snapshot.visible_tabs.front().raw_value());
      }
      for (tabs::TabHandle handle : workspace_snapshot.visible_tabs) {
        tabs::TabInterface* tab = handle.Get();
        if (!tab || tab->GetBrowserWindowInterface() != browser_window_) {
          continue;
        }
        const int32_t identifier = handle.raw_value();
        [identifiers addObject:@(identifier)];
        auto product_identifier = product_identifier_by_handle.find(identifier);
        if (product_identifier == product_identifier_by_handle.end()) {
          has_complete_product_tab_identifiers = false;
        } else {
          [product_identifiers
              addObject:base::SysUTF8ToNSString(product_identifier->second)];
        }
        [titles addObject:base::SysUTF16ToNSString(tab->GetTitle())];
        if (workspace_snapshot.active_tab == handle) {
          active_identifier = @(identifier);
        }
      }
      if (split_controller_) {
        const SplitPaneWindowSnapshot split_snapshot =
            split_controller_->GetSnapshot();
        split_actions_available = has_complete_product_tab_identifiers &&
                                  split_snapshot.available() &&
                                  split_snapshot.window_identifier ==
                                      workspace_snapshot.window_identifier;
        const bool can_project_splits =
            has_complete_product_tab_identifiers &&
            split_snapshot.window_identifier ==
                workspace_snapshot.window_identifier;
        if (can_project_splits) {
          for (const SplitPaneProjection& split : split_snapshot.splits) {
            [split_pair_identifiers
                addObject:base::SysUTF8ToNSString(split.pair_identifier)];
            [split_first_tab_identifiers
                addObject:base::SysUTF8ToNSString(split.first_tab_identifier)];
            [split_second_tab_identifiers
                addObject:base::SysUTF8ToNSString(split.second_tab_identifier)];
            [split_active_tab_identifiers
                addObject:split.active_tab_identifier
                              ? base::SysUTF8ToNSString(
                                    *split.active_tab_identifier)
                              : @""];
            [split_layouts
                addObject:split.layout == SplitPaneLayout::kSideBySide
                              ? @"sideBySide"
                              : @"stacked"];
            [split_ratios addObject:@(split.ratio)];
          }
        }
      }
      if (!workspaces_enabled || !has_complete_product_tab_identifiers) {
        [product_identifiers removeAllObjects];
        [split_pair_identifiers removeAllObjects];
        [split_first_tab_identifiers removeAllObjects];
        [split_second_tab_identifiers removeAllObjects];
        [split_active_tab_identifiers removeAllObjects];
        [split_layouts removeAllObjects];
        [split_ratios removeAllObjects];
        split_actions_available = NO;
      }
    }

    [native_view_ updateWithWorkspacesEnabled:workspaces_enabled
                             windowIdentifier:window_identifier
                         workspaceIdentifiers:workspace_identifiers
                               workspaceNames:workspace_names
                            defaultWorkspaces:default_workspaces
                    activeWorkspaceIdentifier:active_workspace_identifier
                               tabIdentifiers:identifiers
                        productTabIdentifiers:product_identifiers
                                       titles:titles
                          activeTabIdentifier:active_identifier
                        splitActionsAvailable:split_actions_available
                         splitPairIdentifiers:split_pair_identifiers
                     splitFirstTabIdentifiers:split_first_tab_identifiers
                    splitSecondTabIdentifiers:split_second_tab_identifiers
                    splitActiveTabIdentifiers:split_active_tab_identifiers
                                 splitLayouts:split_layouts
                                  splitRatios:split_ratios];
  }

  SwiftUITabRailSnapshotForTesting SnapshotForTesting() const {
    [native_view_ layoutSubtreeIfNeeded];
    SwiftUITabRailSnapshotForTesting snapshot;
    snapshot.rail_density =
        base::SysNSStringToUTF8(native_view_.railDensityForTesting);
    snapshot.first_tab_row_height =
        NSHeight(native_view_.firstTabRowFrameForTesting);
    snapshot.workspaces_enabled = native_view_.workspacesEnabledForTesting;
    snapshot.window_identifier = base::SysNSStringToUTF8(
        native_view_.workspaceWindowIdentifierForTesting);
    for (NSString* identifier in native_view_.workspaceIdentifiersForTesting) {
      snapshot.workspace_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* name in native_view_.workspaceNamesForTesting) {
      snapshot.workspace_names.push_back(base::SysNSStringToUTF16(name));
    }
    for (NSNumber* is_default in native_view_.defaultWorkspacesForTesting) {
      snapshot.default_workspaces.push_back(is_default.boolValue);
    }
    if (native_view_.activeWorkspaceIdentifierForTesting) {
      snapshot.active_workspace_identifier = base::SysNSStringToUTF8(
          native_view_.activeWorkspaceIdentifierForTesting);
    }
    for (NSNumber* identifier in native_view_.tabIdentifiersForTesting) {
      snapshot.tab_identifiers.push_back(identifier.intValue);
    }
    for (NSString* identifier in native_view_.tabProductIdentifiersForTesting) {
      snapshot.tab_product_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* title in native_view_.tabTitlesForTesting) {
      snapshot.tab_titles.push_back(base::SysNSStringToUTF16(title));
    }
    if (native_view_.activeTabIdentifierForTesting) {
      snapshot.active_tab_identifier =
          native_view_.activeTabIdentifierForTesting.intValue;
    }
    snapshot.split_actions_available =
        native_view_.splitActionsAvailableForTesting;
    for (NSString* identifier in native_view_.splitPairIdentifiersForTesting) {
      snapshot.split_pair_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* identifier in native_view_
             .splitFirstTabIdentifiersForTesting) {
      snapshot.split_first_tab_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* identifier in native_view_
             .splitSecondTabIdentifiersForTesting) {
      snapshot.split_second_tab_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* identifier in native_view_
             .splitActiveTabIdentifiersForTesting) {
      snapshot.split_active_tab_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* layout in native_view_.splitLayoutsForTesting) {
      snapshot.split_layouts.push_back(base::SysNSStringToUTF8(layout));
    }
    for (NSNumber* ratio in native_view_.splitRatiosForTesting) {
      snapshot.split_ratios.push_back(ratio.doubleValue);
    }
    snapshot.split_eligibility_batch_invocation_count =
        split_eligibility_batch_invocation_count_;
    snapshot.leader_enabled = native_view_.leaderEnabledForTesting;
    snapshot.leader_armed = native_view_.leaderArmedForTesting;
    snapshot.leader_status_text =
        base::SysNSStringToUTF16(native_view_.leaderStatusTextForTesting);
    snapshot.leader_arm_generation =
        leader_controller_->arm_generation_for_testing();
    snapshot.leader_arm_deadline =
        leader_controller_->arm_deadline_for_testing();
    snapshot.leader_native_delegate_requested =
        leader_native_delegate_requested_;
    return snapshot;
  }

  SwiftUIAgentRailSnapshotForTesting AgentSnapshotForTesting() const {
    [native_view_ layoutSubtreeIfNeeded];
    SwiftUIAgentRailSnapshotForTesting snapshot;
    snapshot.source_state =
        base::SysNSStringToUTF8(native_view_.agentSourceStateForTesting);
    for (NSString* identifier in native_view_.agentPaneIdentifiersForTesting) {
      snapshot.pane_identifiers.push_back(base::SysNSStringToUTF8(identifier));
    }
    for (NSString* identifier in native_view_
             .agentWorkspaceIdentifiersForTesting) {
      snapshot.workspace_identifiers.push_back(
          base::SysNSStringToUTF8(identifier));
    }
    for (NSString* label in native_view_.agentWorkspaceLabelsForTesting) {
      snapshot.workspace_labels.push_back(base::SysNSStringToUTF16(label));
    }
    for (NSString* agent in native_view_.agentKindsForTesting) {
      snapshot.agent_kinds.push_back(base::SysNSStringToUTF8(agent));
    }
    for (NSString* status in native_view_.agentStatusesForTesting) {
      snapshot.statuses.push_back(base::SysNSStringToUTF8(status));
    }
    for (NSString* title in native_view_.agentTitlesForTesting) {
      snapshot.terminal_titles.push_back(base::SysNSStringToUTF16(title));
    }
    for (NSNumber* focused in native_view_.agentFocusedForTesting) {
      snapshot.focused.push_back(focused.boolValue);
    }
    snapshot.rail = LayoutRectForTesting(native_view_.railFrameForTesting);
    snapshot.workspace_header =
        LayoutRectForTesting(native_view_.workspaceHeaderFrameForTesting);
    snapshot.workspace_create_control = LayoutRectForTesting(
        native_view_.workspaceCreateControlFrameForTesting);
    snapshot.workspace_switcher_viewport = LayoutRectForTesting(
        native_view_.workspaceSwitcherViewportFrameForTesting);
    snapshot.workspace_switcher_content = LayoutRectForTesting(
        native_view_.workspaceSwitcherContentFrameForTesting);
    snapshot.workspace_viewport =
        LayoutRectForTesting(native_view_.workspaceViewportFrameForTesting);
    snapshot.workspace_content =
        LayoutRectForTesting(native_view_.workspaceContentFrameForTesting);
    snapshot.agent_viewport =
        LayoutRectForTesting(native_view_.agentViewportFrameForTesting);
    snapshot.agent_content =
        LayoutRectForTesting(native_view_.agentContentFrameForTesting);
    return snapshot;
  }

  std::optional<SwiftUIAccessibilityControlForTesting>
  AccessibilityControlForTesting(const std::string& identifier) const {
    [native_view_.window layoutIfNeeded];
    [native_view_ layoutSubtreeIfNeeded];
    id element = FindAccessibilityElement(native_view_, identifier);
    if (!element) {
      return std::nullopt;
    }
    return AccessibilityControlForElement(element);
  }

  bool FocusAccessibilityControlForTesting(const std::string& identifier) {
    id element = FindAccessibilityElement(native_view_, identifier);
    if (!element ||
        ![element respondsToSelector:@selector(setAccessibilityFocused:)]) {
      return false;
    }
    [element setAccessibilityFocused:YES];
    return true;
  }

  bool PerformAccessibilityActionForTesting(
      const std::string& identifier,
      SwiftUIAccessibilityActionForTesting action) {
    id element = FindAccessibilityElement(native_view_, identifier);
    if (!element) {
      return false;
    }
    return action == SwiftUIAccessibilityActionForTesting::kPress &&
           [element respondsToSelector:@selector(accessibilityPerformPress)] &&
           [element accessibilityPerformPress];
  }

  SwiftUIAccessibilityInventoryForTesting AccessibilityInventoryForTesting()
      const {
    MaterializeNativeAccessibilityTreeForTesting();
    [native_view_.window layoutIfNeeded];
    [native_view_ layoutSubtreeIfNeeded];
    const size_t maximum_node_count =
        48 + 8 * native_view_.tabIdentifiersForTesting.count +
        4 * native_view_.workspaceIdentifiersForTesting.count +
        4 * native_view_.agentPaneIdentifiersForTesting.count;
    NSHashTable<id>* visited = [NSHashTable
        hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
    SwiftUIAccessibilityInventoryForTesting inventory;
    for (id seed in NSAccessibilityUnignoredChildrenForOnlyChild(
             native_view_)) {
      CollectAccessibilityIdentifiers(seed, visited, 0, maximum_node_count + 1,
                                      &inventory.identifiers);
    }
    inventory.node_count = visited.count;
    inventory.hit_node_cap = visited.count > maximum_node_count;
    return inventory;
  }

  std::optional<SwiftUIAccessibilityMenuActionResultForTesting>
  PerformAccessibilityMenuItemForTesting(
      const std::string& menu_button_identifier,
      const std::string& destination_identifier) {
    id menu_button =
        FindAccessibilityElement(native_view_, menu_button_identifier);
    if (!menu_button) {
      return std::nullopt;
    }

    base::RunLoop run_loop;
    base::RepeatingTimer poll_timer;
    base::OneShotTimer timeout_timer;
    size_t destination_count = 0;
    if (base::StartsWith(menu_button_identifier, "limitless-tab-split-")) {
      destination_count = native_view_.tabIdentifiersForTesting.count > 0
                              ? native_view_.tabIdentifiersForTesting.count - 1
                              : 0;
    } else if (menu_button_identifier == "limitless-split-layout") {
      destination_count = 2;
    } else if (menu_button_identifier == "limitless-split-ratio") {
      destination_count = 3;
    } else {
      destination_count =
          native_view_.workspaceIdentifiersForTesting.count > 0
              ? native_view_.workspaceIdentifiersForTesting.count - 1
              : 0;
    }
    struct MenuActionState {
      id menu_button;
      id destination = nil;
      id shown_menu = nil;
      std::string destination_identifier;
      size_t maximum_node_count;
      size_t focus_poll_count = 0;
      bool completed = false;
      SwiftUIAccessibilityMenuActionResultForTesting result;
      raw_ptr<base::RepeatingTimer> poll_timer;
      raw_ptr<base::RunLoop> run_loop;
    } state{
        .menu_button = menu_button,
        .destination_identifier = destination_identifier,
        .maximum_node_count = 4 + 2 * destination_count,
        .poll_timer = &poll_timer,
        .run_loop = &run_loop,
    };
    poll_timer.Start(
        FROM_HERE, base::Milliseconds(5),
        base::BindRepeating(
            [](MenuActionState* state) {
              state->shown_menu = ShownAccessibilityMenu(state->menu_button);
              if (!state->shown_menu) {
                return;
              }
              if (!state->destination) {
                state->result.inventory.identifiers.clear();
                NSHashTable<id>* visited = [NSHashTable
                    hashTableWithOptions:
                        NSPointerFunctionsObjectPointerPersonality];
                CollectAccessibilityIdentifiers(
                    state->shown_menu, visited, 0,
                    state->maximum_node_count + 1,
                    &state->result.inventory.identifiers, false);
                state->result.inventory.node_count = visited.count;
                state->result.inventory.hit_node_cap =
                    visited.count > state->maximum_node_count;
                visited = [NSHashTable
                    hashTableWithOptions:
                        NSPointerFunctionsObjectPointerPersonality];
                state->destination = FindAccessibilityElement(
                    state->shown_menu,
                    base::SysUTF8ToNSString(state->destination_identifier),
                    visited, 0, false);
                if (!state->destination) {
                  return;
                }
                if ([state->destination
                        respondsToSelector:@selector(
                                               setAccessibilityFocused:)]) {
                  [state->destination setAccessibilityFocused:YES];
                }
                return;
              }
              state->result.destination =
                  AccessibilityControlForElement(state->destination);
              if (!state->result.destination.focused &&
                  ++state->focus_poll_count < 40) {
                return;
              }
              if ([state->destination
                      respondsToSelector:@selector(
                                             accessibilityPerformPress)]) {
                state->result.press_invoked = true;
                state->result.press_reported_success =
                    [state->destination accessibilityPerformPress];
              }
              if ([state->shown_menu isKindOfClass:[NSMenu class]]) {
                [state->shown_menu cancelTracking];
              }
              state->completed = true;
              state->poll_timer->Stop();
              state->run_loop->Quit();
            },
            base::Unretained(&state)));
    timeout_timer.Start(
        FROM_HERE, base::Seconds(2),
        base::BindOnce(
            [](MenuActionState* state) {
              state->poll_timer->Stop();
              if ([state->shown_menu isKindOfClass:[NSMenu class]]) {
                [state->shown_menu cancelTracking];
              } else if ([state->menu_button
                             respondsToSelector:
                                 @selector(accessibilityPerformPress)]) {
                [state->menu_button accessibilityPerformPress];
              }
              state->run_loop->Quit();
            },
            base::Unretained(&state)));

    bool opened = false;
    if ([menu_button
            respondsToSelector:@selector(accessibilityPerformShowMenu)] &&
        [menu_button
            isAccessibilitySelectorAllowed:@selector(
                                               accessibilityPerformShowMenu)]) {
      opened = [menu_button accessibilityPerformShowMenu];
    }
    if (!opened &&
        [menu_button respondsToSelector:@selector(accessibilityPerformPress)]) {
      [menu_button accessibilityPerformPress];
    }
    if (!state.completed) {
      run_loop.Run();
    }
    poll_timer.Stop();
    timeout_timer.Stop();
    return state.completed ? std::optional(state.result) : std::nullopt;
  }

  bool ActivateTabForTesting(int32_t tab_identifier) {
    return [native_view_ activateTabForTesting:tab_identifier];
  }

  bool CloseTabForTesting(int32_t tab_identifier) {
    return [native_view_ closeTabForTesting:tab_identifier];
  }

  std::optional<std::string> CreateWorkspaceForTesting(
      const std::u16string& name) {
    NSString* identifier =
        [native_view_ createWorkspaceForTesting:base::SysUTF16ToNSString(name)];
    return identifier
               ? std::optional<std::string>(base::SysNSStringToUTF8(identifier))
               : std::nullopt;
  }

  bool SwitchWorkspaceForTesting(const std::string& workspace_identifier) {
    return [native_view_ switchWorkspaceForTesting:base::SysUTF8ToNSString(
                                                       workspace_identifier)];
  }

  bool MoveTabToWorkspaceForTesting(int32_t tab_identifier,
                                    const std::string& workspace_identifier) {
    return [native_view_
        moveTabForTesting:tab_identifier
              toWorkspace:base::SysUTF8ToNSString(workspace_identifier)];
  }

  void SimulateMissingProductIdentityMappingForTesting() {
    simulate_missing_product_identity_mapping_for_testing_ = true;
    UpdateSnapshot();
  }

  bool UpdateRailDensityForTesting(const std::string& rail_density) {
    return
        [native_view_ updateRailDensity:base::SysUTF8ToNSString(rail_density)];
  }

  bool PreHandleLeaderKeyboardEvent(
      const input::NativeWebKeyboardEvent& event) {
    return leader_controller_->HandleNativeWebKeyboardEvent(event);
  }

  bool PreHandleLeaderViewsKeyEventForTesting(const ui::KeyEvent& event) {
    return leader_controller_->HandleKeyEvent(event);
  }

  bool ExpireLeaderForTesting() {
    return leader_controller_->ExpireForTesting();
  }

  std::optional<std::string> CreateWorkspace(const std::u16string& name) {
    WorkspaceOperationResult result =
        workspace_controller_->CreateWorkspace(name);
    return result.succeeded() ? result.workspace_identifier : std::nullopt;
  }

  bool SwitchWorkspace(const std::string& workspace_identifier) {
    return workspace_controller_->SwitchWorkspace(workspace_identifier)
        .succeeded();
  }

  bool MoveTabToWorkspace(int32_t tab_identifier,
                          const std::string& workspace_identifier) {
    return workspace_controller_
        ->MoveTabToWorkspace(tabs::TabHandle(tab_identifier),
                             workspace_identifier)
        .succeeded();
  }

  bool CreateSplit(const std::string& window_identifier,
                   const std::string& first_tab_identifier,
                   const std::string& second_tab_identifier) {
    return split_controller_ &&
           split_controller_
               ->CreateSplit(window_identifier, first_tab_identifier,
                             second_tab_identifier)
               .succeeded();
  }

  NSDictionary<NSString*, NSString*>* GetSplitEligibility(
      const std::string& window_identifier,
      const std::string& source_tab_identifier,
      const std::vector<std::string>& destination_tab_identifiers) {
    ++split_eligibility_batch_invocation_count_;
    if (!split_controller_) {
      return @{};
    }
    NSMutableDictionary<NSString*, NSString*>* result =
        [NSMutableDictionary dictionary];
    for (const auto& [identifier, status] :
         split_controller_->GetCreateSplitStatuses(
             window_identifier, source_tab_identifier,
             destination_tab_identifiers)) {
      NSString* value = @"unavailable";
      switch (status) {
        case SplitPaneOperationStatus::kSuccess:
          value = @"eligible";
          break;
        case SplitPaneOperationStatus::kPinnedStateMismatch:
          value = @"pinnedStateMismatch";
          break;
        case SplitPaneOperationStatus::kGroupMismatch:
          value = @"groupMismatch";
          break;
        default:
          break;
      }
      result[base::SysUTF8ToNSString(identifier)] = value;
    }
    return result;
  }

  bool FocusSplit(const std::string& window_identifier,
                  const std::string& first_tab_identifier,
                  const std::string& second_tab_identifier,
                  const std::string& target_tab_identifier) {
    return split_controller_ &&
           split_controller_
               ->FocusPane(
                   SplitPanePairTarget{
                       .window_identifier = window_identifier,
                       .first_tab_identifier = first_tab_identifier,
                       .second_tab_identifier = second_tab_identifier,
                   },
                   target_tab_identifier)
               .succeeded();
  }

  bool SetSplitLayout(const std::string& window_identifier,
                      const std::string& first_tab_identifier,
                      const std::string& second_tab_identifier,
                      const std::string& layout) {
    std::optional<SplitPaneLayout> parsed_layout;
    if (layout == "sideBySide") {
      parsed_layout = SplitPaneLayout::kSideBySide;
    } else if (layout == "stacked") {
      parsed_layout = SplitPaneLayout::kStacked;
    }
    return split_controller_ && parsed_layout &&
           split_controller_
               ->SetLayout(
                   SplitPanePairTarget{
                       .window_identifier = window_identifier,
                       .first_tab_identifier = first_tab_identifier,
                       .second_tab_identifier = second_tab_identifier,
                   },
                   *parsed_layout)
               .succeeded();
  }

  bool SetSplitRatio(const std::string& window_identifier,
                     const std::string& first_tab_identifier,
                     const std::string& second_tab_identifier,
                     double ratio) {
    return split_controller_ &&
           split_controller_
               ->SetRatio(
                   SplitPanePairTarget{
                       .window_identifier = window_identifier,
                       .first_tab_identifier = first_tab_identifier,
                       .second_tab_identifier = second_tab_identifier,
                   },
                   ratio)
               .succeeded();
  }

  bool SwapSplit(const std::string& window_identifier,
                 const std::string& first_tab_identifier,
                 const std::string& second_tab_identifier) {
    return split_controller_ &&
           split_controller_
               ->Swap(SplitPanePairTarget{
                   .window_identifier = window_identifier,
                   .first_tab_identifier = first_tab_identifier,
                   .second_tab_identifier = second_tab_identifier,
               })
               .succeeded();
  }

  bool Unsplit(const std::string& window_identifier,
               const std::string& first_tab_identifier,
               const std::string& second_tab_identifier) {
    return split_controller_ &&
           split_controller_
               ->Unsplit(SplitPanePairTarget{
                   .window_identifier = window_identifier,
                   .first_tab_identifier = first_tab_identifier,
                   .second_tab_identifier = second_tab_identifier,
               })
               .succeeded();
  }

  bool IsForWindow(BrowserWindowInterface* browser_window) const {
    return browser_window_ == browser_window;
  }

  NSObject<CommandDispatcherDelegate>* CreateLeaderCommandDispatcherDelegate() {
    leader_native_delegate_requested_ = true;
    return limitless::CreateLeaderCommandDispatcherDelegate(
        leader_controller_->GetWeakPtr());
  }

 private:
  void UnregisterPreTargetHandler() {
    if (!pre_target_root_) {
      return;
    }
    pre_target_root_->RemovePreTargetHandler(this);
    pre_target_root_ = nullptr;
  }

  bool CanHandleLeader() {
    if (!browser_window_ || !tab_list_ ||
        browser_window_->IsDeleteScheduled() || !GetWidget()) {
      return false;
    }
    views::FocusManager* focus_manager = GetFocusManager();
    if (!focus_manager || focus_manager->shortcut_handling_suspended()) {
      return false;
    }
    NSWindow* native_window =
        GetWidget()->GetNativeWindow().GetNativeNSWindow();
    NSResponder* first_responder = native_window.firstResponder;
    if ([first_responder respondsToSelector:@selector(hasMarkedText)] &&
        [(id<NSTextInputClient>)first_responder hasMarkedText]) {
      return false;
    }
    ui::InputMethod* input_method = GetWidget()->GetInputMethod();
    if (input_method && input_method->GetTextInputClient() &&
        input_method->GetTextInputClient()->HasCompositionText()) {
      return false;
    }
    tabs::TabInterface* active_tab = tab_list_->GetActiveTab();
    content::WebContents* contents =
        active_tab ? active_tab->GetContents() : nullptr;
    content::RenderWidgetHostView* renderer_view =
        contents ? contents->GetRenderWidgetHostView() : nullptr;
    return !renderer_view || !renderer_view->HasFocus() ||
           !contents->IsFocusedElementEditable();
  }

  bool ExecuteLeaderCommand(LeaderCommand command) {
    if (!browser_window_ || !tab_list_ ||
        browser_window_->IsDeleteScheduled() || tab_list_->IsClosingAllTabs()) {
      return false;
    }
    if (command == LeaderCommand::kToggleSidebar) {
      sidebar_visible_ = !sidebar_visible_;
      SetVisible(sidebar_visible_ || leader_controller_->armed());
      return true;
    }

    tabs::TabInterface* active_tab = tab_list_->GetActiveTab();
    if (!active_tab || !CanOperateOnTab(active_tab->GetHandle().raw_value())) {
      return false;
    }
    if (command == LeaderCommand::kCloseTab) {
      return CloseTab(active_tab->GetHandle().raw_value());
    }

    int target_index = -1;
    const std::vector<tabs::TabHandle> visible_tabs =
        workspace_controller_->GetVisibleTabs();
    auto active = std::ranges::find(visible_tabs, active_tab->GetHandle());
    const int active_index =
        active == visible_tabs.end()
            ? -1
            : static_cast<int>(active - visible_tabs.begin());
    const int tab_count = static_cast<int>(visible_tabs.size());
    if (active_index < 0 || tab_count <= 0) {
      return false;
    }
    switch (command) {
      case LeaderCommand::kNextTab:
        target_index = (active_index + 1) % tab_count;
        break;
      case LeaderCommand::kPreviousTab:
        target_index = (active_index + tab_count - 1) % tab_count;
        break;
      case LeaderCommand::kSelectTab1:
      case LeaderCommand::kSelectTab2:
      case LeaderCommand::kSelectTab3:
      case LeaderCommand::kSelectTab4:
      case LeaderCommand::kSelectTab5:
      case LeaderCommand::kSelectTab6:
      case LeaderCommand::kSelectTab7:
      case LeaderCommand::kSelectTab8:
      case LeaderCommand::kSelectTab9:
        target_index = static_cast<int>(command) -
                       static_cast<int>(LeaderCommand::kSelectTab1);
        break;
      case LeaderCommand::kCloseTab:
      case LeaderCommand::kToggleSidebar:
        NOTREACHED();
    }
    tabs::TabInterface* target_tab =
        target_index >= 0 && target_index < tab_count
            ? visible_tabs[target_index].Get()
            : nullptr;
    if (!target_tab || !CanOperateOnTab(target_tab->GetHandle().raw_value())) {
      return false;
    }
    return ActivateTab(target_tab->GetHandle().raw_value());
  }

  void OnLeaderArmedChanged(bool armed) {
    [native_view_ updateLeaderEnabled:leader_controller_->enabled()
                                armed:armed];
    SetVisible(sidebar_visible_ || armed);
  }

  void StopObservingAgentPresence() {
    if (!observing_agent_presence_) {
      return;
    }
    GetAgentPresenceRegistry().RemoveObserver(this);
    observing_agent_presence_ = false;
  }

  void ScheduleProjectionUpdate() {
    if (projection_update_scheduled_) {
      return;
    }
    projection_update_scheduled_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<SwiftUITabRailHost> host) {
                         if (!host) {
                           return;
                         }
                         host->projection_update_scheduled_ = false;
                         host->RebuildTitleObservers();
                         host->UpdateSnapshot();
                       },
                       weak_ptr_factory_.GetWeakPtr()));
  }

  void OnWorkspaceStateChanged() {
    if (split_controller_) {
      split_controller_->ReconcileWorkspaceState();
    }
    ScheduleProjectionUpdate();
  }

  void RebuildTitleObservers() {
    title_observers_.clear();
    if (!tab_list_) {
      return;
    }
    for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
      if (content::WebContents* contents = tab->GetContents()) {
        title_observers_.push_back(std::make_unique<TabTitleObserver>(
            contents, weak_ptr_factory_.GetWeakPtr()));
      }
    }
  }

  bool ActivateTab(int32_t tab_identifier) {
    return workspace_controller_->ActivateTab(tabs::TabHandle(tab_identifier))
        .succeeded();
  }

  bool CloseTab(int32_t tab_identifier) {
    return workspace_controller_->CloseTab(tabs::TabHandle(tab_identifier))
        .succeeded();
  }

  bool CanOperateOnTab(int32_t tab_identifier) const {
    if (!browser_window_ || !tab_list_ ||
        browser_window_->IsDeleteScheduled() || tab_list_->IsClosingAllTabs()) {
      return false;
    }
    const tabs::TabHandle handle(tab_identifier);
    tabs::TabInterface* tab = handle.Get();
    return tab && tab->GetBrowserWindowInterface() == browser_window_ &&
           tab->GetProfile() == browser_window_->GetProfile() &&
           tab_list_->GetIndexOfTab(handle) >= 0 &&
           TabListInterface::CanEditTabList(*browser_window_->GetProfile());
  }

  raw_ptr<BrowserWindowInterface> browser_window_;
  raw_ptr<TabListInterface> tab_list_;
  raw_ptr<views::View> pre_target_root_ = nullptr;
  LimitlessSwiftUITabRailView* __strong native_view_;
  std::unique_ptr<WorkspaceController> workspace_controller_;
  std::unique_ptr<SplitPaneController> split_controller_;
  std::unique_ptr<LeaderShortcutController> leader_controller_;
  std::unique_ptr<ProfilePreferenceControlBackend> preference_backend_;
  base::CallbackListSubscription browser_did_close_subscription_;
  std::vector<std::unique_ptr<TabTitleObserver>> title_observers_;
  bool observing_agent_presence_ = false;
  bool sidebar_visible_ = true;
  bool leader_native_delegate_requested_ = false;
  bool projection_update_scheduled_ = false;
  bool simulate_missing_product_identity_mapping_for_testing_ = false;
  uint64_t split_eligibility_batch_invocation_count_ = 0;
  base::WeakPtrFactory<SwiftUITabRailHost> weak_ptr_factory_{this};
};

void TabTitleObserver::TitleWasSet(content::NavigationEntry* entry) {
  if (host_) {
    host_->UpdateSnapshot();
  }
}

SwiftUITabRailHost* FindHost(views::View* rail) {
  auto& hosts = GetLiveHosts();
  auto iter = std::ranges::find(hosts, rail);
  return iter == hosts.end() ? nullptr : *iter;
}

SwiftUITabRailHost* FindHost(BrowserWindowInterface* browser_window) {
  auto& hosts = GetLiveHosts();
  auto iter =
      std::ranges::find_if(hosts, [browser_window](SwiftUITabRailHost* host) {
        return host->IsForWindow(browser_window);
      });
  return iter == hosts.end() ? nullptr : *iter;
}

}  // namespace

std::unique_ptr<views::View> MaybeCreateSwiftUITabRail(
    BrowserWindowInterface* browser_window) {
  if (!browser_window ||
      browser_window->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          kSwiftUIBoundaryProbeSwitch)) {
    return nullptr;
  }
  return std::make_unique<SwiftUITabRailHost>(browser_window);
}

NSObject<CommandDispatcherDelegate>*
MaybeCreateLeaderCommandDispatcherDelegateForWindow(
    BrowserWindowInterface* browser_window) {
  SwiftUITabRailHost* host = FindHost(browser_window);
  return host ? host->CreateLeaderCommandDispatcherDelegate() : nil;
}

int GetSwiftUIBoundaryProbeAttachedHostCountForTesting() {
  return g_attached_host_count;
}

std::optional<SwiftUITabRailSnapshotForTesting>
GetSwiftUITabRailSnapshotForTesting(views::View* rail) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host ? std::optional(host->SnapshotForTesting()) : std::nullopt;
}

bool ActivateSwiftUITabForTesting(views::View* rail, int32_t tab_identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->ActivateTabForTesting(tab_identifier);
}

bool CloseSwiftUITabForTesting(views::View* rail, int32_t tab_identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->CloseTabForTesting(tab_identifier);
}

std::optional<std::string> CreateSwiftUIWorkspaceForTesting(
    views::View* rail,
    const std::u16string& name) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host ? host->CreateWorkspaceForTesting(name) : std::nullopt;
}

bool SwitchSwiftUIWorkspaceForTesting(views::View* rail,
                                      const std::string& workspace_identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->SwitchWorkspaceForTesting(workspace_identifier);
}

bool MoveSwiftUITabToWorkspaceForTesting(
    views::View* rail,
    int32_t tab_identifier,
    const std::string& workspace_identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->MoveTabToWorkspaceForTesting(tab_identifier,
                                                    workspace_identifier);
}

bool SimulateMissingSwiftUIProductIdentityMappingForTesting(views::View* rail) {
  SwiftUITabRailHost* host = FindHost(rail);
  if (!host) {
    return false;
  }
  host->SimulateMissingProductIdentityMappingForTesting();
  return true;
}

bool UpdateSwiftUIRailDensityForTesting(views::View* rail,
                                        const std::string& rail_density) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->UpdateRailDensityForTesting(rail_density);
}

std::optional<SwiftUIAgentRailSnapshotForTesting>
GetSwiftUIAgentRailSnapshotForTesting(views::View* rail) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host ? std::optional(host->AgentSnapshotForTesting()) : std::nullopt;
}

std::optional<SwiftUIAccessibilityControlForTesting>
GetSwiftUIAccessibilityControlForTesting(views::View* rail,
                                         const std::string& identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host ? host->AccessibilityControlForTesting(identifier) : std::nullopt;
}

bool FocusSwiftUIAccessibilityControlForTesting(views::View* rail,
                                                const std::string& identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->FocusAccessibilityControlForTesting(identifier);
}

bool PerformSwiftUIAccessibilityActionForTesting(
    views::View* rail,
    const std::string& identifier,
    SwiftUIAccessibilityActionForTesting action) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->PerformAccessibilityActionForTesting(identifier, action);
}

SwiftUIAccessibilityInventoryForTesting
GetSwiftUIAccessibilityInventoryForTesting(views::View* rail) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host ? host->AccessibilityInventoryForTesting()
              : SwiftUIAccessibilityInventoryForTesting();
}

std::optional<SwiftUIAccessibilityMenuActionResultForTesting>
PerformSwiftUIAccessibilityMenuItemForTesting(
    views::View* rail,
    const std::string& menu_button_identifier,
    const std::string& destination_identifier) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host ? host->PerformAccessibilityMenuItemForTesting(
                    menu_button_identifier, destination_identifier)
              : std::nullopt;
}

void RequestHerdrAgentRefreshForTesting() {
  GetAgentPresenceRegistry().RequestRefreshForTesting();
}

bool PreHandleSwiftUILeaderKeyboardEvent(
    views::View* rail,
    const input::NativeWebKeyboardEvent& event) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->PreHandleLeaderKeyboardEvent(event);
}

bool PreHandleSwiftUILeaderViewsKeyEventForTesting(views::View* rail,
                                                   const ui::KeyEvent& event) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->PreHandleLeaderViewsKeyEventForTesting(event);
}

base::TimeDelta GetLeaderShortcutTimeoutForTesting() {
  return GetLeaderShortcutTimeout();
}

bool ExpireSwiftUILeaderForTesting(views::View* rail) {
  SwiftUITabRailHost* host = FindHost(rail);
  return host && host->ExpireLeaderForTesting();
}

}  // namespace limitless
