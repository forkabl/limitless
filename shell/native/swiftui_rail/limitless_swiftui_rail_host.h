// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_NATIVE_SWIFTUI_RAIL_LIMITLESS_SWIFTUI_RAIL_HOST_H_
#define LIMITLESS_SHELL_NATIVE_SWIFTUI_RAIL_LIMITLESS_SWIFTUI_RAIL_HOST_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "limitless_leader_shortcut_controller.h"

class BrowserWindowInterface;

#ifdef __OBJC__
@protocol CommandDispatcherDelegate;
#endif

namespace input {
struct NativeWebKeyboardEvent;
}

namespace ui {
class KeyEvent;
}

namespace views {
class View;
}

namespace limitless {

struct ProductCommandInvocation;
struct ProductCommandQueryResult;
struct ProductCommandExecutionResult;

ProductCommandQueryResult QuerySwiftUIProductCommandForTesting(
    BrowserWindowInterface* browser_window,
    const ProductCommandInvocation& invocation);
ProductCommandExecutionResult ExecuteSwiftUIProductCommandForTesting(
    BrowserWindowInterface* browser_window,
    const ProductCommandInvocation& invocation);

inline constexpr char kSwiftUIBoundaryProbeSwitch[] =
    "limitless-swiftui-boundary-probe";
inline constexpr int kSwiftUITabRailWidth = 216;

// Returns a product-owned SwiftUI tab rail only for an eligible normal window
// in the explicit Gate 2B experiment. The returned generic Views host observes
// the supplied window's authoritative tab list. It creates no Browser,
// BrowserWindow, or NSWindow and never receives a WebContents native view.
std::unique_ptr<views::View> MaybeCreateSwiftUITabRail(
    BrowserWindowInterface* browser_window);

#ifdef __OBJC__
// Returns a product-owned native delegate only when the exact normal window
// has an enabled leader controller. Disabled and ineligible windows return nil
// so Chromium installs its stock delegate unchanged.
NSObject<CommandDispatcherDelegate>*
MaybeCreateLeaderCommandDispatcherDelegateForWindow(
    BrowserWindowInterface* browser_window);
#endif

// Returns the number of probes currently attached to a Chromium Widget. This
// is lifecycle evidence for the bounded Gate 2B experiment, not product state.
int GetSwiftUIBoundaryProbeAttachedHostCountForTesting();

struct SwiftUITabRailSnapshotForTesting {
  std::string rail_density;
  double first_tab_row_height = 0;
  bool workspaces_enabled = false;
  std::string window_identifier;
  std::vector<std::string> workspace_identifiers;
  std::vector<std::u16string> workspace_names;
  std::vector<bool> default_workspaces;
  std::optional<std::string> active_workspace_identifier;
  std::vector<int32_t> tab_identifiers;
  std::vector<std::string> tab_product_identifiers;
  std::vector<std::u16string> tab_titles;
  std::optional<int32_t> active_tab_identifier;
  bool split_actions_available = false;
  std::vector<std::string> split_pair_identifiers;
  std::vector<std::string> split_first_tab_identifiers;
  std::vector<std::string> split_second_tab_identifiers;
  std::vector<std::string> split_active_tab_identifiers;
  std::vector<std::string> split_layouts;
  std::vector<double> split_ratios;
  uint64_t split_eligibility_batch_invocation_count = 0;
  bool leader_enabled = false;
  bool leader_armed = false;
  std::u16string leader_status_text;
  uint64_t leader_arm_generation = 0;
  base::TimeTicks leader_arm_deadline;
  bool leader_native_delegate_requested = false;
};

struct SwiftUILayoutRectForTesting {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct SwiftUIAgentRailSnapshotForTesting {
  std::string source_state;
  std::vector<std::string> pane_identifiers;
  std::vector<std::string> workspace_identifiers;
  std::vector<std::u16string> workspace_labels;
  std::vector<std::string> agent_kinds;
  std::vector<std::string> statuses;
  std::vector<std::u16string> terminal_titles;
  std::vector<bool> focused;
  SwiftUILayoutRectForTesting rail;
  SwiftUILayoutRectForTesting workspace_header;
  SwiftUILayoutRectForTesting workspace_create_control;
  SwiftUILayoutRectForTesting workspace_switcher_viewport;
  SwiftUILayoutRectForTesting workspace_switcher_content;
  SwiftUILayoutRectForTesting workspace_viewport;
  SwiftUILayoutRectForTesting workspace_content;
  SwiftUILayoutRectForTesting agent_viewport;
  SwiftUILayoutRectForTesting agent_content;
};

struct SwiftUIAccessibilityControlForTesting {
  std::string role;
  std::u16string label;
  std::u16string value;
  std::vector<std::string> actions;
  bool enabled = false;
  bool focused = false;
};

struct SwiftUIAccessibilityInventoryForTesting {
  size_t node_count = 0;
  bool hit_node_cap = false;
  std::vector<std::string> identifiers;
};

struct SwiftUIAccessibilityMenuActionResultForTesting {
  SwiftUIAccessibilityControlForTesting destination;
  SwiftUIAccessibilityInventoryForTesting inventory;
  // Keep invocation and AppKit's return value separate. SwiftUI menu items can
  // dispatch their native action while reporting false to this in-process
  // caller, so the browser test verifies the authoritative product mutation.
  bool press_invoked = false;
  bool press_reported_success = false;
};

enum class SwiftUIAccessibilityActionForTesting {
  kPress,
};

// These projection and action helpers cross the generated Swift/Objective-C
// boundary without simulating native macOS input.
std::optional<SwiftUITabRailSnapshotForTesting>
GetSwiftUITabRailSnapshotForTesting(views::View* rail);
bool ActivateSwiftUITabForTesting(views::View* rail, int32_t tab_identifier);
bool CloseSwiftUITabForTesting(views::View* rail, int32_t tab_identifier);
std::optional<std::string> CreateSwiftUIWorkspaceForTesting(
    views::View* rail,
    const std::u16string& name);
bool SwitchSwiftUIWorkspaceForTesting(views::View* rail,
                                      const std::string& workspace_identifier);
bool MoveSwiftUITabToWorkspaceForTesting(
    views::View* rail,
    int32_t tab_identifier,
    const std::string& workspace_identifier);
bool SimulateMissingSwiftUIProductIdentityMappingForTesting(views::View* rail);
// Updates only the product-owned SwiftUI appearance model. This test seam
// exercises the same typed native bridge used by the profile preference
// adapter and never mutates Chromium tab or page state.
bool UpdateSwiftUIRailDensityForTesting(views::View* rail,
                                        const std::string& rail_density);
std::optional<SwiftUIAgentRailSnapshotForTesting>
GetSwiftUIAgentRailSnapshotForTesting(views::View* rail);
// These helpers traverse and invoke the actual native SwiftUI accessibility
// surface in-process. They are not external VoiceOver or hardware-input proof.
std::optional<SwiftUIAccessibilityControlForTesting>
GetSwiftUIAccessibilityControlForTesting(views::View* rail,
                                         const std::string& identifier);
bool FocusSwiftUIAccessibilityControlForTesting(views::View* rail,
                                                const std::string& identifier);
bool PerformSwiftUIAccessibilityActionForTesting(
    views::View* rail,
    const std::string& identifier,
    SwiftUIAccessibilityActionForTesting action);
SwiftUIAccessibilityInventoryForTesting
GetSwiftUIAccessibilityInventoryForTesting(views::View* rail);
std::optional<SwiftUIAccessibilityMenuActionResultForTesting>
PerformSwiftUIAccessibilityMenuItemForTesting(
    views::View* rail,
    const std::string& menu_button_identifier,
    const std::string& destination_identifier);
void RequestHerdrAgentRefreshForTesting();
bool PreHandleSwiftUILeaderKeyboardEvent(
    views::View* rail,
    const input::NativeWebKeyboardEvent& event);
bool PreHandleSwiftUILeaderViewsKeyEventForTesting(views::View* rail,
                                                   const ui::KeyEvent& event);
base::TimeDelta GetLeaderShortcutTimeoutForTesting();
bool ExpireSwiftUILeaderForTesting(views::View* rail);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_NATIVE_SWIFTUI_RAIL_LIMITLESS_SWIFTUI_RAIL_HOST_H_
