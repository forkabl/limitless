// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_SPLITS_LIMITLESS_SPLIT_PANE_CONTROLLER_H_
#define LIMITLESS_SHELL_SPLITS_LIMITLESS_SPLIT_PANE_CONTROLLER_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/tabs/public/tab_interface.h"

class BrowserWindowInterface;
class BrowserFocusController;
class TabListInterface;
class TabStripModel;

namespace limitless {

struct WorkspaceWindowSnapshot;

enum class SplitPaneLayout {
  kSideBySide,
  kStacked,
};

enum class SplitPaneOperationStatus {
  kSuccess,
  kPolicyDisabled,
  kUnsupportedWindowType,
  kPrivateProfileUnsupported,
  kWindowUnavailable,
  kWorkspaceUnavailable,
  kWorkspaceRestoreInProgress,
  kTabListUnavailable,
  kInvalidIdentifier,
  kForeignWindow,
  kProfileMismatch,
  kTabUnavailable,
  kInactiveWorkspace,
  kTabListNotEditable,
  kSameTab,
  kAlreadySplit,
  kPairNotFound,
  kPairChanged,
  kPinnedStateMismatch,
  kGroupMismatch,
  kInvalidLayout,
  kInvalidRatio,
  kFocusUnavailable,
};

struct SplitPaneOperationResult {
  SplitPaneOperationStatus status =
      SplitPaneOperationStatus::kWindowUnavailable;
  std::optional<std::string> pair_identifier;

  bool succeeded() const {
    return status == SplitPaneOperationStatus::kSuccess;
  }
};

struct SplitPanePairTarget {
  std::string window_identifier;
  std::string first_tab_identifier;
  std::string second_tab_identifier;
};

struct SplitPaneProjection {
  std::string pair_identifier;
  std::string first_tab_identifier;
  std::string second_tab_identifier;
  SplitPaneLayout layout = SplitPaneLayout::kSideBySide;
  double ratio = 0.5;
  std::optional<std::string> active_tab_identifier;
};

struct SplitPaneWindowSnapshot {
  SplitPaneOperationStatus availability_status =
      SplitPaneOperationStatus::kWindowUnavailable;
  std::string window_identifier;
  std::vector<SplitPaneProjection> splits;

  bool available() const {
    return availability_status == SplitPaneOperationStatus::kSuccess;
  }
};

// Per-window product adapter over Chromium's authoritative two-tab split
// model. Product callers use stable workspace window and tab identifiers; raw
// Chromium split identifiers never cross this interface.
class SplitPaneController final : public TabStripModelObserver {
 public:
  using FocusControllerResolver =
      base::RepeatingCallback<BrowserFocusController*()>;

  SplitPaneController(BrowserWindowInterface* browser_window,
                      base::RepeatingClosure state_changed,
                      FocusControllerResolver focus_controller_resolver = {});
  ~SplitPaneController() override;

  SplitPaneController(const SplitPaneController&) = delete;
  SplitPaneController& operator=(const SplitPaneController&) = delete;

  SplitPaneWindowSnapshot GetSnapshot() const;
  std::map<std::string, SplitPaneOperationStatus> GetCreateSplitStatuses(
      const std::string& window_identifier,
      const std::string& tab_identifier,
      const std::vector<std::string>& destination_tab_identifiers) const;
  SplitPaneOperationStatus GetCreateSplitStatus(
      const std::string& window_identifier,
      const std::string& tab_identifier,
      const std::string& other_tab_identifier) const;
  SplitPaneOperationStatus GetPairStatus(const SplitPanePairTarget& pair) const;

  SplitPaneOperationResult CreateSplit(const std::string& window_identifier,
                                       const std::string& tab_identifier,
                                       const std::string& other_tab_identifier);
  SplitPaneOperationResult FocusPane(const SplitPanePairTarget& pair,
                                     const std::string& member_tab_identifier);
  SplitPaneOperationResult SetLayout(const SplitPanePairTarget& pair,
                                     SplitPaneLayout layout);
  SplitPaneOperationResult SetRatio(const SplitPanePairTarget& pair,
                                    double ratio);
  SplitPaneOperationResult Swap(const SplitPanePairTarget& pair);
  SplitPaneOperationResult Unsplit(const SplitPanePairTarget& pair);

  // Schedules end-of-transaction reconciliation after product workspace
  // metadata changes. This never mutates Chromium synchronously.
  void ReconcileWorkspaceState();
  void Stop();

  BrowserWindowInterface* browser_window() const { return browser_window_; }
  static SplitPaneController* From(BrowserWindowInterface* browser_window);

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabStripEditabilityChanged(TabStripModel* tab_strip_model,
                                    bool is_editable) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;
  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override;

 private:
  SplitPaneOperationStatus ValidateMutableWindow(
      WorkspaceWindowSnapshot* workspace_snapshot = nullptr) const;
  SplitPaneOperationStatus ResolveVisibleTab(
      const std::string& window_identifier,
      const std::string& tab_identifier,
      tabs::TabHandle* resolved) const;
  SplitPaneOperationStatus ValidateCreateSplit(
      const std::string& window_identifier,
      const std::string& tab_identifier,
      const std::string& other_tab_identifier,
      tabs::TabHandle* tab,
      tabs::TabHandle* other_tab) const;
  SplitPaneOperationStatus ResolvePair(const SplitPanePairTarget& pair,
                                       tabs::TabHandle* first,
                                       tabs::TabHandle* second) const;
  void ScheduleReconcile();
  void ReconcileAndPublish(uint64_t generation);

  bool stopped_ = false;
  bool reconcile_scheduled_ = false;
  bool reconciling_ = false;
  bool reconcile_requested_ = false;
  uint64_t reconcile_generation_ = 0;
  raw_ptr<BrowserWindowInterface> browser_window_;
  raw_ptr<TabListInterface> tab_list_;
  raw_ptr<TabStripModel> tab_strip_model_;
  base::RepeatingClosure state_changed_;
  FocusControllerResolver focus_controller_resolver_;
  base::WeakPtrFactory<SplitPaneController> weak_ptr_factory_{this};
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_SPLITS_LIMITLESS_SPLIT_PANE_CONTROLLER_H_
