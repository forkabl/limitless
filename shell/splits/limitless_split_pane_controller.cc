// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_split_pane_controller.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "base/check.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/focus/browser_focus_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tabs/public/split_tab_data.h"
#include "limitless_workspace_controller.h"

namespace limitless {
namespace {

using ControllerMap = std::map<BrowserWindowInterface*, SplitPaneController*>;

ControllerMap& GetControllers() {
  static base::NoDestructor<ControllerMap> controllers;
  return *controllers;
}

bool IsCanonicalIdentifier(const std::string& identifier) {
  const base::Uuid parsed = base::Uuid::ParseLowercase(identifier);
  return parsed.is_valid() && parsed.AsLowercaseString() == identifier;
}

std::string MakePairIdentifier(const std::string& first,
                               const std::string& second) {
  const auto [lower, upper] = std::minmax(first, second);
  return base::StrCat({lower, ":", upper});
}

SplitPaneLayout ToProductLayout(split_tabs::SplitTabLayout layout) {
  switch (layout) {
    case split_tabs::SplitTabLayout::kSideBySide:
      return SplitPaneLayout::kSideBySide;
    case split_tabs::SplitTabLayout::kStacked:
      return SplitPaneLayout::kStacked;
  }
  NOTREACHED();
}

std::optional<split_tabs::SplitTabLayout> ToChromiumLayout(
    SplitPaneLayout layout) {
  switch (layout) {
    case SplitPaneLayout::kSideBySide:
      return split_tabs::SplitTabLayout::kSideBySide;
    case SplitPaneLayout::kStacked:
      return split_tabs::SplitTabLayout::kStacked;
  }
  return std::nullopt;
}

SplitPaneOperationResult SuccessForPair(const SplitPanePairTarget& pair) {
  return {
      .status = SplitPaneOperationStatus::kSuccess,
      .pair_identifier = MakePairIdentifier(pair.first_tab_identifier,
                                            pair.second_tab_identifier),
  };
}

}  // namespace

SplitPaneController::SplitPaneController(
    BrowserWindowInterface* browser_window,
    base::RepeatingClosure state_changed,
    FocusControllerResolver focus_controller_resolver)
    : browser_window_(browser_window),
      tab_list_(browser_window ? TabListInterface::From(browser_window)
                               : nullptr),
      tab_strip_model_(browser_window ? browser_window->GetTabStripModel()
                                      : nullptr),
      state_changed_(std::move(state_changed)),
      focus_controller_resolver_(std::move(focus_controller_resolver)) {
  CHECK(browser_window_);
  CHECK(tab_list_);
  CHECK(tab_strip_model_);
  CHECK(!GetControllers().contains(browser_window));
  GetControllers().emplace(browser_window, this);
  tab_strip_model_->AddObserver(this);
}

SplitPaneController::~SplitPaneController() {
  Stop();
}

SplitPaneWindowSnapshot SplitPaneController::GetSnapshot() const {
  SplitPaneWindowSnapshot snapshot;
  WorkspaceWindowSnapshot workspace_snapshot;
  snapshot.availability_status = ValidateMutableWindow(&workspace_snapshot);
  if (stopped_ || !browser_window_ || !tab_list_ || !tab_strip_model_) {
    return snapshot;
  }

  snapshot.window_identifier = workspace_snapshot.window_identifier;
  if (!workspace_snapshot.enabled ||
      !workspace_snapshot.active_workspace_identifier) {
    return snapshot;
  }
  if (snapshot.availability_status != SplitPaneOperationStatus::kSuccess &&
      snapshot.availability_status !=
          SplitPaneOperationStatus::kTabListNotEditable) {
    return snapshot;
  }

  std::map<int32_t, std::string> visible_identifiers;
  for (const WorkspaceTabProjection& tab :
       workspace_snapshot.visible_tab_projections) {
    visible_identifiers.emplace(tab.handle.raw_value(), tab.identifier);
  }

  tabs::TabInterface* active_tab = tab_strip_model_->GetActiveTab();
  std::set<split_tabs::SplitTabId> live_split_ids;
  for (const split_tabs::SplitTabId split_id : tab_strip_model_->ListSplits()) {
    live_split_ids.insert(split_id);
    const split_tabs::SplitTabData* split_data =
        tab_strip_model_->GetSplitData(split_id);
    if (!split_data) {
      continue;
    }
    const std::vector<tabs::TabInterface*> members = split_data->ListTabs();
    if (members.size() != 2u || !members[0] || !members[1]) {
      continue;
    }
    const int32_t first_member = members[0]->GetHandle().raw_value();
    const int32_t second_member = members[1]->GetHandle().raw_value();
    const auto canonical_members =
        std::make_pair(std::min(first_member, second_member),
                       std::max(first_member, second_member));
    const auto first =
        visible_identifiers.find(members[0]->GetHandle().raw_value());
    const auto second =
        visible_identifiers.find(members[1]->GetHandle().raw_value());
    if (first == visible_identifiers.end() ||
        second == visible_identifiers.end()) {
      continue;
    }

    const split_tabs::SplitTabVisualData* visual_data =
        split_data->visual_data();
    if (!visual_data) {
      continue;
    }
    SplitPaneProjection projection{
        .split_instance_identifier =
            GetOrCreateSplitInstanceIdentifier(split_id, canonical_members),
        .pair_identifier = MakePairIdentifier(first->second, second->second),
        .first_tab_identifier = first->second,
        .second_tab_identifier = second->second,
        .layout = ToProductLayout(visual_data->split_layout()),
        .ratio = visual_data->split_ratio(),
    };
    if (active_tab == members[0]) {
      projection.active_tab_identifier = first->second;
    } else if (active_tab == members[1]) {
      projection.active_tab_identifier = second->second;
    }
    snapshot.splits.push_back(std::move(projection));
  }
  for (auto it = split_instance_identifiers_.begin();
       it != split_instance_identifiers_.end();) {
    if (!live_split_ids.contains(it->first)) {
      it = split_instance_identifiers_.erase(it);
    } else {
      ++it;
    }
  }

  return snapshot;
}

std::string SplitPaneController::GetOrCreateSplitInstanceIdentifier(
    split_tabs::SplitTabId split_id,
    std::pair<int32_t, int32_t> canonical_members) const {
  auto found = split_instance_identifiers_.find(split_id);
  if (found != split_instance_identifiers_.end() &&
      found->second.canonical_members == canonical_members) {
    return found->second.identifier;
  }
  std::string identifier = base::Uuid::GenerateRandomV4().AsLowercaseString();
  split_instance_identifiers_[split_id] =
      SplitInstanceEntry{identifier, canonical_members};
  return identifier;
}

std::map<std::string, SplitPaneOperationStatus>
SplitPaneController::GetCreateSplitStatuses(
    const std::string& window_identifier,
    const std::string& tab_identifier,
    const std::vector<std::string>& destination_tab_identifiers) const {
  std::map<std::string, SplitPaneOperationStatus> statuses;
  for (const std::string& destination : destination_tab_identifiers) {
    if (IsCanonicalIdentifier(destination)) {
      statuses.try_emplace(destination,
                           SplitPaneOperationStatus::kTabUnavailable);
    }
  }
  if (statuses.empty()) {
    return statuses;
  }

  WorkspaceWindowSnapshot workspace_snapshot;
  const SplitPaneOperationStatus window_status =
      ValidateMutableWindow(&workspace_snapshot);
  if (window_status != SplitPaneOperationStatus::kSuccess) {
    for (auto& [destination, status] : statuses) {
      status = window_status;
    }
    return statuses;
  }
  if (!IsCanonicalIdentifier(window_identifier) ||
      !IsCanonicalIdentifier(tab_identifier)) {
    for (auto& [destination, status] : statuses) {
      status = SplitPaneOperationStatus::kInvalidIdentifier;
    }
    return statuses;
  }
  if (workspace_snapshot.window_identifier != window_identifier) {
    for (auto& [destination, status] : statuses) {
      status = SplitPaneOperationStatus::kForeignWindow;
    }
    return statuses;
  }

  std::unordered_map<std::string, tabs::TabHandle> visible_tabs;
  visible_tabs.reserve(workspace_snapshot.visible_tab_projections.size());
  for (const WorkspaceTabProjection& projection :
       workspace_snapshot.visible_tab_projections) {
    visible_tabs.try_emplace(projection.identifier, projection.handle);
  }

  const auto source = visible_tabs.find(tab_identifier);
  SplitPaneOperationStatus source_status = SplitPaneOperationStatus::kSuccess;
  tabs::TabInterface* source_tab = nullptr;
  if (source == visible_tabs.end()) {
    source_status = SplitPaneOperationStatus::kTabUnavailable;
  } else {
    source_tab = source->second.Get();
    if (!source_tab) {
      source_status = SplitPaneOperationStatus::kTabUnavailable;
    } else if (source_tab->GetBrowserWindowInterface() != browser_window_) {
      source_status = SplitPaneOperationStatus::kForeignWindow;
    } else if (source_tab->GetProfile() != browser_window_->GetProfile()) {
      source_status = SplitPaneOperationStatus::kProfileMismatch;
    }
  }

  for (auto& [destination, status] : statuses) {
    if (source_status != SplitPaneOperationStatus::kSuccess) {
      status = source_status;
      continue;
    }
    if (destination == tab_identifier) {
      status = SplitPaneOperationStatus::kSameTab;
      continue;
    }
    const auto peer = visible_tabs.find(destination);
    if (peer == visible_tabs.end()) {
      status = SplitPaneOperationStatus::kTabUnavailable;
      continue;
    }
    tabs::TabInterface* peer_tab = peer->second.Get();
    if (!peer_tab) {
      status = SplitPaneOperationStatus::kTabUnavailable;
    } else if (peer_tab->GetBrowserWindowInterface() != browser_window_) {
      status = SplitPaneOperationStatus::kForeignWindow;
    } else if (peer_tab->GetProfile() != browser_window_->GetProfile()) {
      status = SplitPaneOperationStatus::kProfileMismatch;
    } else if (source_tab->IsSplit() || peer_tab->IsSplit()) {
      status = SplitPaneOperationStatus::kAlreadySplit;
    } else if (source_tab->IsPinned() != peer_tab->IsPinned()) {
      status = SplitPaneOperationStatus::kPinnedStateMismatch;
    } else if (source_tab->GetGroup() != peer_tab->GetGroup()) {
      status = SplitPaneOperationStatus::kGroupMismatch;
    } else {
      status = SplitPaneOperationStatus::kSuccess;
    }
  }
  return statuses;
}

SplitPaneOperationStatus SplitPaneController::GetCreateSplitStatus(
    const std::string& window_identifier,
    const std::string& tab_identifier,
    const std::string& other_tab_identifier) const {
  tabs::TabHandle tab;
  tabs::TabHandle other_tab;
  return ValidateCreateSplit(window_identifier, tab_identifier,
                             other_tab_identifier, &tab, &other_tab);
}

SplitPaneOperationStatus SplitPaneController::GetPairStatus(
    const SplitPanePairTarget& pair) const {
  tabs::TabHandle first;
  tabs::TabHandle second;
  return ResolvePair(pair, &first, &second);
}

SplitPaneOperationResult SplitPaneController::CreateSplit(
    const std::string& window_identifier,
    const std::string& tab_identifier,
    const std::string& other_tab_identifier) {
  tabs::TabHandle first;
  tabs::TabHandle second;
  const SplitPaneOperationStatus status = ValidateCreateSplit(
      window_identifier, tab_identifier, other_tab_identifier, &first, &second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return {.status = status};
  }

  tabs::TabInterface* first_tab = first.Get();
  tabs::TabInterface* second_tab = second.Get();
  CHECK(first_tab);
  CHECK(second_tab);

  base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
  const std::optional<split_tabs::SplitTabId> split_id =
      tab_list_->CreateSplit({first, second});
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  if (!split_id || !tab_strip_model_->ContainsSplit(*split_id)) {
    return {.status = SplitPaneOperationStatus::kTabUnavailable};
  }
  const split_tabs::SplitTabData* split_data =
      tab_strip_model_->GetSplitData(*split_id);
  const std::vector<tabs::TabInterface*> members =
      split_data ? split_data->ListTabs() : std::vector<tabs::TabInterface*>();
  const bool contains_exact_members =
      members.size() == 2u &&
      ((members[0] == first_tab && members[1] == second_tab) ||
       (members[0] == second_tab && members[1] == first_tab));
  if (!contains_exact_members) {
    tab_list_->Unsplit(*split_id);
    if (!alive) {
      return {.status = SplitPaneOperationStatus::kWindowUnavailable};
    }
    return {.status = SplitPaneOperationStatus::kPairChanged};
  }

  return {
      .status = SplitPaneOperationStatus::kSuccess,
      .pair_identifier =
          MakePairIdentifier(tab_identifier, other_tab_identifier),
  };
}

SplitPaneOperationResult SplitPaneController::FocusPane(
    const SplitPanePairTarget& pair,
    const std::string& member_tab_identifier) {
  tabs::TabHandle first;
  tabs::TabHandle second;
  const SplitPaneOperationStatus status = ResolvePair(pair, &first, &second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return {.status = status};
  }
  if (!IsCanonicalIdentifier(member_tab_identifier)) {
    return {.status = SplitPaneOperationStatus::kInvalidIdentifier};
  }
  if (member_tab_identifier != pair.first_tab_identifier &&
      member_tab_identifier != pair.second_tab_identifier) {
    return {.status = SplitPaneOperationStatus::kPairChanged};
  }

  base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
  BrowserFocusController* preflight_focus_controller =
      alive->focus_controller_resolver_
          ? alive->focus_controller_resolver_.Run()
          : BrowserFocusController::From(alive->browser_window_);
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  if (!preflight_focus_controller) {
    return {.status = SplitPaneOperationStatus::kFocusUnavailable};
  }
  WorkspaceController* workspace =
      WorkspaceController::From(alive->browser_window_);
  if (!workspace) {
    return {.status = SplitPaneOperationStatus::kTabUnavailable};
  }
  const WorkspaceOperationResult activation =
      workspace->ActivateTabByIdentifier(pair.window_identifier,
                                         member_tab_identifier);
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  if (!activation.succeeded()) {
    return {.status = SplitPaneOperationStatus::kTabUnavailable};
  }
  const SplitPaneOperationStatus revalidated_status =
      alive->ResolvePair(pair, &first, &second);
  if (revalidated_status != SplitPaneOperationStatus::kSuccess) {
    return {.status = revalidated_status};
  }
  tabs::TabInterface* activated_member =
      member_tab_identifier == pair.first_tab_identifier ? first.Get()
                                                         : second.Get();
  if (!activated_member ||
      alive->tab_list_->GetActiveTab() != activated_member) {
    return {.status = SplitPaneOperationStatus::kPairChanged};
  }
  BrowserFocusController* focus_controller =
      alive->focus_controller_resolver_
          ? alive->focus_controller_resolver_.Run()
          : BrowserFocusController::From(alive->browser_window_);
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  if (!focus_controller) {
    return {.status = SplitPaneOperationStatus::kFocusUnavailable};
  }
  focus_controller->FocusWebContentsPane();
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  return SuccessForPair(pair);
}

SplitPaneOperationResult SplitPaneController::SetLayout(
    const SplitPanePairTarget& pair,
    SplitPaneLayout layout) {
  tabs::TabHandle first;
  tabs::TabHandle second;
  const SplitPaneOperationStatus status = ResolvePair(pair, &first, &second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return {.status = status};
  }

  const std::optional<split_tabs::SplitTabLayout> chromium_layout =
      ToChromiumLayout(layout);
  if (!chromium_layout) {
    return {.status = SplitPaneOperationStatus::kInvalidLayout};
  }
  const split_tabs::SplitTabId split_id = first.Get()->GetSplit().value();
  base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
  tab_strip_model_->UpdateSplitLayout(split_id, *chromium_layout);
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  tab_strip_model_->UpdateSplitRatio(split_id, 0.5,
                                     /*is_intermediate=*/false);
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  return SuccessForPair(pair);
}

SplitPaneOperationResult SplitPaneController::SetRatio(
    const SplitPanePairTarget& pair,
    double ratio) {
  tabs::TabHandle first;
  tabs::TabHandle second;
  const SplitPaneOperationStatus status = ResolvePair(pair, &first, &second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return {.status = status};
  }
  if (!std::isfinite(ratio) || ratio < 0.25 || ratio > 0.75) {
    return {.status = SplitPaneOperationStatus::kInvalidRatio};
  }

  base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
  tab_strip_model_->UpdateSplitRatio(first.Get()->GetSplit().value(), ratio,
                                     /*is_intermediate=*/false);
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  return SuccessForPair(pair);
}

SplitPaneOperationResult SplitPaneController::Swap(
    const SplitPanePairTarget& pair) {
  tabs::TabHandle first;
  tabs::TabHandle second;
  const SplitPaneOperationStatus status = ResolvePair(pair, &first, &second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return {.status = status};
  }

  base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
  tab_strip_model_->ReverseTabsInSplit(first.Get()->GetSplit().value());
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  return SuccessForPair(pair);
}

SplitPaneOperationResult SplitPaneController::Unsplit(
    const SplitPanePairTarget& pair) {
  tabs::TabHandle first;
  tabs::TabHandle second;
  const SplitPaneOperationStatus status = ResolvePair(pair, &first, &second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return {.status = status};
  }

  base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
  tab_list_->Unsplit(first.Get()->GetSplit().value());
  if (!alive) {
    return {.status = SplitPaneOperationStatus::kWindowUnavailable};
  }
  return SuccessForPair(pair);
}

void SplitPaneController::ReconcileWorkspaceState() {
  ScheduleReconcile();
}

void SplitPaneController::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  ++reconcile_generation_;
  weak_ptr_factory_.InvalidateWeakPtrs();
  reconcile_scheduled_ = false;
  if (tab_strip_model_) {
    tab_strip_model_->RemoveObserver(this);
  }
  if (browser_window_) {
    GetControllers().erase(browser_window_);
  }
  tab_strip_model_ = nullptr;
  tab_list_ = nullptr;
  browser_window_ = nullptr;
  state_changed_.Reset();
  focus_controller_resolver_.Reset();
  split_instance_identifiers_.clear();
}

SplitPaneController* SplitPaneController::From(
    BrowserWindowInterface* browser_window) {
  auto found = GetControllers().find(browser_window);
  return found == GetControllers().end() ? nullptr : found->second;
}

void SplitPaneController::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange&,
    const TabStripSelectionChange&) {
  if (tab_strip_model == tab_strip_model_) {
    ScheduleReconcile();
  }
}

void SplitPaneController::OnTabStripEditabilityChanged(
    TabStripModel* tab_strip_model,
    bool /*is_editable*/) {
  if (tab_strip_model == tab_strip_model_) {
    ScheduleReconcile();
  }
}

void SplitPaneController::OnSplitTabChanged(const SplitTabChange& change) {
  if (change.model == tab_strip_model_) {
    if (change.type == SplitTabChange::Type::kRemoved) {
      split_instance_identifiers_.erase(change.split_id);
    }
    ScheduleReconcile();
  }
}

void SplitPaneController::OnTabStripModelDestroyed(
    TabStripModel* tab_strip_model) {
  if (tab_strip_model != tab_strip_model_) {
    return;
  }
  ++reconcile_generation_;
  weak_ptr_factory_.InvalidateWeakPtrs();
  reconcile_scheduled_ = false;
  split_instance_identifiers_.clear();
  tab_strip_model_ = nullptr;
  tab_list_ = nullptr;
  base::RepeatingClosure state_changed = state_changed_;
  if (state_changed) {
    state_changed.Run();
  }
}

SplitPaneOperationStatus SplitPaneController::ValidateMutableWindow(
    WorkspaceWindowSnapshot* workspace_snapshot) const {
  if (stopped_ || !browser_window_ || browser_window_->IsDeleteScheduled()) {
    return SplitPaneOperationStatus::kWindowUnavailable;
  }
  if (browser_window_->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return SplitPaneOperationStatus::kUnsupportedWindowType;
  }
  Profile* profile = browser_window_->GetProfile();
  if (!profile) {
    return SplitPaneOperationStatus::kWindowUnavailable;
  }
  if (profile->IsOffTheRecord()) {
    return SplitPaneOperationStatus::kPrivateProfileUnsupported;
  }
  WorkspaceController* workspace = WorkspaceController::From(browser_window_);
  if (!workspace) {
    return SplitPaneOperationStatus::kWorkspaceUnavailable;
  }
  WorkspaceWindowSnapshot current_workspace_snapshot = workspace->GetSnapshot();
  if (workspace_snapshot) {
    *workspace_snapshot = current_workspace_snapshot;
  }
  if (!current_workspace_snapshot.enabled) {
    return SplitPaneOperationStatus::kPolicyDisabled;
  }
  if (current_workspace_snapshot.session_restore_in_progress) {
    return SplitPaneOperationStatus::kWorkspaceRestoreInProgress;
  }
  if (!tab_list_ || !tab_strip_model_ ||
      browser_window_->GetTabStripModel() != tab_strip_model_ ||
      tab_list_->IsClosingAllTabs()) {
    return SplitPaneOperationStatus::kTabListUnavailable;
  }
  if (!TabListInterface::CanEditTabList(*profile)) {
    return SplitPaneOperationStatus::kTabListNotEditable;
  }
  return SplitPaneOperationStatus::kSuccess;
}

SplitPaneOperationStatus SplitPaneController::ResolveVisibleTab(
    const std::string& window_identifier,
    const std::string& tab_identifier,
    tabs::TabHandle* resolved) const {
  CHECK(resolved);
  const SplitPaneOperationStatus window_status = ValidateMutableWindow();
  if (window_status != SplitPaneOperationStatus::kSuccess) {
    return window_status;
  }
  if (!IsCanonicalIdentifier(window_identifier) ||
      !IsCanonicalIdentifier(tab_identifier)) {
    return SplitPaneOperationStatus::kInvalidIdentifier;
  }

  WorkspaceController* workspace = WorkspaceController::From(browser_window_);
  if (!workspace) {
    return SplitPaneOperationStatus::kWorkspaceUnavailable;
  }
  const WorkspaceWindowSnapshot snapshot = workspace->GetSnapshot();
  if (snapshot.window_identifier != window_identifier) {
    return SplitPaneOperationStatus::kForeignWindow;
  }
  const auto found =
      std::ranges::find(snapshot.visible_tab_projections, tab_identifier,
                        &WorkspaceTabProjection::identifier);
  if (found == snapshot.visible_tab_projections.end()) {
    return SplitPaneOperationStatus::kTabUnavailable;
  }

  tabs::TabInterface* tab = found->handle.Get();
  if (!tab) {
    return SplitPaneOperationStatus::kTabUnavailable;
  }
  if (tab->GetBrowserWindowInterface() != browser_window_) {
    return SplitPaneOperationStatus::kForeignWindow;
  }
  if (tab->GetProfile() != browser_window_->GetProfile()) {
    return SplitPaneOperationStatus::kProfileMismatch;
  }
  if (tab_strip_model_->GetIndexOfTab(tab) == TabStripModel::kNoTab) {
    return SplitPaneOperationStatus::kTabUnavailable;
  }
  if (!workspace->IsTabInActiveWorkspace(found->handle)) {
    return SplitPaneOperationStatus::kInactiveWorkspace;
  }
  *resolved = found->handle;
  return SplitPaneOperationStatus::kSuccess;
}

SplitPaneOperationStatus SplitPaneController::ValidateCreateSplit(
    const std::string& window_identifier,
    const std::string& tab_identifier,
    const std::string& other_tab_identifier,
    tabs::TabHandle* tab,
    tabs::TabHandle* other_tab) const {
  CHECK(tab);
  CHECK(other_tab);
  const SplitPaneOperationStatus window_status = ValidateMutableWindow();
  if (window_status != SplitPaneOperationStatus::kSuccess) {
    return window_status;
  }
  if (!IsCanonicalIdentifier(window_identifier) ||
      !IsCanonicalIdentifier(tab_identifier) ||
      !IsCanonicalIdentifier(other_tab_identifier)) {
    return SplitPaneOperationStatus::kInvalidIdentifier;
  }
  if (tab_identifier == other_tab_identifier) {
    return SplitPaneOperationStatus::kSameTab;
  }

  SplitPaneOperationStatus status =
      ResolveVisibleTab(window_identifier, tab_identifier, tab);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return status;
  }
  status =
      ResolveVisibleTab(window_identifier, other_tab_identifier, other_tab);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return status;
  }
  tabs::TabInterface* first = tab->Get();
  tabs::TabInterface* second = other_tab->Get();
  if (!first || !second) {
    return SplitPaneOperationStatus::kTabUnavailable;
  }
  if (first->IsSplit() || second->IsSplit()) {
    return SplitPaneOperationStatus::kAlreadySplit;
  }
  if (first->IsPinned() != second->IsPinned()) {
    return SplitPaneOperationStatus::kPinnedStateMismatch;
  }
  if (first->GetGroup() != second->GetGroup()) {
    return SplitPaneOperationStatus::kGroupMismatch;
  }
  return SplitPaneOperationStatus::kSuccess;
}

SplitPaneOperationStatus SplitPaneController::ResolvePair(
    const SplitPanePairTarget& pair,
    tabs::TabHandle* first,
    tabs::TabHandle* second) const {
  CHECK(first);
  CHECK(second);
  const SplitPaneOperationStatus window_status = ValidateMutableWindow();
  if (window_status != SplitPaneOperationStatus::kSuccess) {
    return window_status;
  }
  if (!IsCanonicalIdentifier(pair.window_identifier) ||
      !IsCanonicalIdentifier(pair.first_tab_identifier) ||
      !IsCanonicalIdentifier(pair.second_tab_identifier)) {
    return SplitPaneOperationStatus::kInvalidIdentifier;
  }
  if (pair.first_tab_identifier == pair.second_tab_identifier) {
    return SplitPaneOperationStatus::kSameTab;
  }
  SplitPaneOperationStatus status = ResolveVisibleTab(
      pair.window_identifier, pair.first_tab_identifier, first);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return status;
  }
  status = ResolveVisibleTab(pair.window_identifier, pair.second_tab_identifier,
                             second);
  if (status != SplitPaneOperationStatus::kSuccess) {
    return status;
  }

  tabs::TabInterface* first_tab = first->Get();
  tabs::TabInterface* second_tab = second->Get();
  if (!first_tab || !second_tab || !first_tab->GetSplit() ||
      !second_tab->GetSplit() ||
      first_tab->GetSplit() != second_tab->GetSplit()) {
    return SplitPaneOperationStatus::kPairNotFound;
  }
  const split_tabs::SplitTabId split_id = first_tab->GetSplit().value();
  if (!tab_strip_model_->ContainsSplit(split_id)) {
    return SplitPaneOperationStatus::kPairNotFound;
  }
  const std::vector<tabs::TabInterface*> members =
      tab_strip_model_->GetSplitData(split_id)->ListTabs();
  if (members.size() != 2u || members[0] != first_tab ||
      members[1] != second_tab) {
    return SplitPaneOperationStatus::kPairChanged;
  }
  return SplitPaneOperationStatus::kSuccess;
}

void SplitPaneController::ScheduleReconcile() {
  if (stopped_ || !tab_strip_model_) {
    return;
  }
  if (reconciling_) {
    reconcile_requested_ = true;
    return;
  }
  const uint64_t generation = ++reconcile_generation_;
  if (reconcile_scheduled_) {
    return;
  }
  reconcile_scheduled_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SplitPaneController::ReconcileAndPublish,
                                weak_ptr_factory_.GetWeakPtr(), generation));
}

void SplitPaneController::ReconcileAndPublish(uint64_t generation) {
  reconcile_scheduled_ = false;
  if (stopped_ || !tab_strip_model_) {
    return;
  }
  if (generation != reconcile_generation_) {
    reconcile_scheduled_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&SplitPaneController::ReconcileAndPublish,
                       weak_ptr_factory_.GetWeakPtr(), reconcile_generation_));
    return;
  }

  WorkspaceController* workspace = WorkspaceController::From(browser_window_);
  if (ValidateMutableWindow() == SplitPaneOperationStatus::kSuccess &&
      workspace) {
    reconciling_ = true;
    base::WeakPtr<SplitPaneController> alive = weak_ptr_factory_.GetWeakPtr();
    base::WeakPtr<WorkspaceController> workspace_alive =
        workspace->GetWeakPtr();
    for (const split_tabs::SplitTabId split_id :
         tab_strip_model_->ListSplits()) {
      if (!workspace_alive) {
        break;
      }
      const split_tabs::SplitTabData* split_data =
          tab_strip_model_->GetSplitData(split_id);
      if (!split_data) {
        tab_list_->Unsplit(split_id);
        if (!alive) {
          return;
        }
        if (!workspace_alive) {
          break;
        }
        continue;
      }
      const std::vector<tabs::TabInterface*> members = split_data->ListTabs();
      if (members.size() != 2u || !members[0] || !members[1]) {
        tab_list_->Unsplit(split_id);
        if (!alive) {
          return;
        }
        if (!workspace_alive) {
          break;
        }
        continue;
      }
      const bool exact_window_and_profile =
          members[0]->GetBrowserWindowInterface() == browser_window_ &&
          members[1]->GetBrowserWindowInterface() == browser_window_ &&
          members[0]->GetProfile() == browser_window_->GetProfile() &&
          members[1]->GetProfile() == browser_window_->GetProfile();
      const std::optional<std::string> first_workspace =
          workspace_alive->GetWorkspaceIdentifierForTab(
              members[0]->GetHandle());
      const std::optional<std::string> second_workspace =
          workspace_alive->GetWorkspaceIdentifierForTab(
              members[1]->GetHandle());
      if (!exact_window_and_profile || !first_workspace || !second_workspace ||
          first_workspace != second_workspace) {
        tab_list_->Unsplit(split_id);
        if (!alive) {
          return;
        }
        if (!workspace_alive) {
          break;
        }
      }
    }
    reconciling_ = false;
  }

  if (reconcile_requested_) {
    reconcile_requested_ = false;
    ScheduleReconcile();
  }
  base::RepeatingClosure state_changed = state_changed_;
  if (state_changed) {
    state_changed.Run();
  }
}

}  // namespace limitless
