// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_split_pane_controller.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/test/run_until.h"
#include "base/uuid.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/tab_list/tab_list_interface_observer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/focus/browser_focus_controller.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/omnibox/omnibox_view.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/omnibox/omnibox_view_views.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/test/browser_test.h"
#include "limitless_workspace_controller.h"
#include "url/gurl.h"

namespace limitless {
namespace {

using SplitPaneBrowserTestBase = InProcessBrowserTest;

class LimitlessSplitPaneControllerBrowserTest
    : public SplitPaneBrowserTestBase {
 public:
  void SetUpOnMainThread() override {
    SplitPaneBrowserTestBase::SetUpOnMainThread();
    ASSERT_EQ(nullptr, WorkspaceController::From(browser()));
    workspace_controller_ = std::make_unique<WorkspaceController>(
        browser(), WorkspacePersistenceCallbacks{}, base::DoNothing());
    ASSERT_TRUE(workspace_controller_->GetSnapshot().enabled);
  }

  void TearDownOnMainThread() override {
    if (workspace_controller_) {
      workspace_controller_->Stop();
      workspace_controller_.reset();
    }
    SplitPaneBrowserTestBase::TearDownOnMainThread();
  }

 protected:
  tabs::TabHandle AddTab() {
    chrome::AddTabAt(browser(), GURL("about:blank"), -1,
                     /*foreground=*/true);
    tabs::TabInterface* active = browser()->GetTabStripModel()->GetActiveTab();
    CHECK(active);
    return active->GetHandle();
  }

  std::string IdentifierFor(tabs::TabHandle handle) const {
    const WorkspaceWindowSnapshot snapshot =
        workspace_controller_->GetSnapshot();
    const auto found =
        std::ranges::find(snapshot.visible_tab_projections, handle,
                          &WorkspaceTabProjection::handle);
    CHECK(found != snapshot.visible_tab_projections.end());
    return found->identifier;
  }

  SplitPanePairTarget OnlyPairTarget(
      const SplitPaneController& controller) const {
    const SplitPaneWindowSnapshot snapshot = controller.GetSnapshot();
    CHECK_EQ(1u, snapshot.splits.size());
    return {
        .window_identifier = snapshot.window_identifier,
        .first_tab_identifier = snapshot.splits[0].first_tab_identifier,
        .second_tab_identifier = snapshot.splits[0].second_tab_identifier,
    };
  }

  std::unique_ptr<WorkspaceController> workspace_controller_;
};

class StopAndDestroySplitPaneControllerObserver final
    : public TabStripModelObserver {
 public:
  explicit StopAndDestroySplitPaneControllerObserver(TabStripModel* model)
      : model_(model) {
    CHECK(model_);
    model_->AddObserver(this);
  }

  ~StopAndDestroySplitPaneControllerObserver() override {
    if (model_) {
      model_->RemoveObserver(this);
    }
  }

  void Arm(std::unique_ptr<SplitPaneController>* owner) {
    CHECK(owner);
    CHECK(*owner);
    owner_ = owner;
  }

  void OnSplitTabChanged(const SplitTabChange&) override {
    if (!owner_) {
      return;
    }
    std::unique_ptr<SplitPaneController>* owner =
        std::exchange(owner_, nullptr);
    (*owner)->Stop();
    owner->reset();
  }

  void OnTabStripModelDestroyed(TabStripModel* model) override {
    if (model == model_) {
      model_ = nullptr;
    }
  }

 private:
  raw_ptr<TabStripModel> model_;
  raw_ptr<std::unique_ptr<SplitPaneController>> owner_ = nullptr;
};

class StopAndDestroyWorkspaceControllerObserver final
    : public TabStripModelObserver {
 public:
  explicit StopAndDestroyWorkspaceControllerObserver(TabStripModel* model)
      : model_(model) {
    CHECK(model_);
    model_->AddObserver(this);
  }

  ~StopAndDestroyWorkspaceControllerObserver() override {
    if (model_) {
      model_->RemoveObserver(this);
    }
  }

  void Arm(std::unique_ptr<WorkspaceController>* owner) {
    CHECK(owner);
    CHECK(*owner);
    owner_ = owner;
  }

  void OnSplitTabChanged(const SplitTabChange&) override {
    if (!owner_) {
      return;
    }
    ++invocation_count_;
    std::unique_ptr<WorkspaceController>* owner =
        std::exchange(owner_, nullptr);
    (*owner)->Stop();
    owner->reset();
  }

  void OnTabStripModelDestroyed(TabStripModel* model) override {
    if (model == model_) {
      model_ = nullptr;
    }
  }

  int invocation_count() const { return invocation_count_; }

 private:
  raw_ptr<TabStripModel> model_;
  raw_ptr<std::unique_ptr<WorkspaceController>> owner_ = nullptr;
  int invocation_count_ = 0;
};

class RunOnceOnActiveTabChangedObserver final
    : public TabListInterfaceObserver {
 public:
  RunOnceOnActiveTabChangedObserver(TabListInterface* tab_list,
                                    base::OnceClosure callback)
      : callback_(std::move(callback)) {
    CHECK(tab_list);
    observation_.Observe(tab_list);
  }

  void OnActiveTabChanged(TabListInterface&, tabs::TabInterface*) override {
    if (callback_) {
      std::move(callback_).Run();
    }
  }

  void OnTabListDestroyed(TabListInterface& tab_list) override {
    if (observation_.IsObservingSource(&tab_list)) {
      observation_.Reset();
    }
  }

 private:
  base::OnceClosure callback_;
  base::ScopedObservation<TabListInterface, TabListInterfaceObserver>
      observation_{this};
};

std::string BuildWorkspaceTabSessionData(
    const std::string& tab_identifier,
    const std::string& workspace_identifier) {
  return base::StrCat({R"({"version":1,"tab_id":")", tab_identifier,
                       R"(","workspace_id":")", workspace_identifier, R"("})"});
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       ReconcilesSplitAcrossDistinctInactiveWorkspaces) {
  const std::string default_workspace =
      *workspace_controller_->GetSnapshot().active_workspace_identifier;
  const WorkspaceOperationResult first_workspace =
      workspace_controller_->CreateWorkspace(u"First inactive");
  ASSERT_TRUE(first_workspace.succeeded());
  const tabs::TabHandle first =
      workspace_controller_->GetSnapshot().active_tab.value();
  const WorkspaceOperationResult second_workspace =
      workspace_controller_->CreateWorkspace(u"Second inactive");
  ASSERT_TRUE(second_workspace.succeeded());
  const tabs::TabHandle second =
      workspace_controller_->GetSnapshot().active_tab.value();
  ASSERT_NE(first_workspace.workspace_identifier,
            second_workspace.workspace_identifier);
  ASSERT_TRUE(
      workspace_controller_->SwitchWorkspace(default_workspace).succeeded());

  TabStripModel* model = browser()->GetTabStripModel();
  const int first_index = model->GetIndexOfTab(first.Get());
  const int second_index = model->GetIndexOfTab(second.Get());
  ASSERT_NE(TabStripModel::kNoTab, first_index);
  ASSERT_NE(TabStripModel::kNoTab, second_index);
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  ASSERT_TRUE(first.Get()->IsSplit());
  ASSERT_TRUE(second.Get()->IsSplit());

  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  controller->ReconcileWorkspaceState();
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(first.Get()->IsSplit());
  EXPECT_FALSE(second.Get()->IsSplit());
  EXPECT_TRUE(model->ListSplits().empty());
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       StateChangedCallbackMayDestroyController) {
  std::unique_ptr<SplitPaneController> controller;
  controller = std::make_unique<SplitPaneController>(
      browser(),
      base::BindRepeating(
          [](std::unique_ptr<SplitPaneController>* owner) { owner->reset(); },
          &controller));

  controller->ReconcileWorkspaceState();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(controller);
  EXPECT_EQ(nullptr, SplitPaneController::From(browser()));
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       SplitInstanceIdentifierIsCanonicalAndStable) {
  const tabs::TabHandle first = AddTab();
  const tabs::TabHandle second = AddTab();
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  const SplitPaneWindowSnapshot first_snapshot = controller->GetSnapshot();
  ASSERT_EQ(1u, first_snapshot.splits.size());
  const std::string identifier =
      first_snapshot.splits[0].split_instance_identifier;
  const base::Uuid uuid = base::Uuid::ParseLowercase(identifier);
  EXPECT_TRUE(uuid.is_valid());
  EXPECT_EQ(identifier, uuid.AsLowercaseString());
  const SplitPaneWindowSnapshot second_snapshot = controller->GetSnapshot();
  ASSERT_EQ(1u, second_snapshot.splits.size());
  EXPECT_EQ(identifier, second_snapshot.splits[0].split_instance_identifier);

  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  ASSERT_TRUE(
      controller->SetLayout(pair, SplitPaneLayout::kStacked).succeeded());
  const SplitPaneWindowSnapshot stacked = controller->GetSnapshot();
  ASSERT_EQ(1u, stacked.splits.size());
  EXPECT_EQ(identifier, stacked.splits[0].split_instance_identifier);
  ASSERT_TRUE(controller->SetRatio(pair, 0.25).succeeded());
  const SplitPaneWindowSnapshot ratio = controller->GetSnapshot();
  ASSERT_EQ(1u, ratio.splits.size());
  EXPECT_EQ(identifier, ratio.splits[0].split_instance_identifier);
  ASSERT_TRUE(controller->Swap(pair).succeeded());
  const SplitPaneWindowSnapshot swapped = controller->GetSnapshot();
  ASSERT_EQ(1u, swapped.splits.size());
  EXPECT_EQ(identifier, swapped.splits[0].split_instance_identifier);

  const SplitPanePairTarget swapped_pair = OnlyPairTarget(*controller);
  ASSERT_TRUE(controller->Unsplit(swapped_pair).succeeded());
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  const SplitPaneWindowSnapshot recreated = controller->GetSnapshot();
  ASSERT_EQ(1u, recreated.splits.size());
  EXPECT_NE(identifier, recreated.splits[0].split_instance_identifier);
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       ReusedNativeSplitIdGetsFreshProductIdentity) {
  const tabs::TabHandle first = AddTab();
  const tabs::TabHandle second = AddTab();
  TabListInterface* tab_list = TabListInterface::From(browser());
  ASSERT_TRUE(tab_list->CreateSplit({first, second}));
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  const SplitPaneWindowSnapshot before = controller->GetSnapshot();
  ASSERT_EQ(1u, before.splits.size());
  const std::string old_identifier = before.splits[0].split_instance_identifier;
  const split_tabs::SplitTabId native_id = first.Get()->GetSplit().value();
  TabStripModel* model = browser()->GetTabStripModel();
  ASSERT_TRUE(controller->Unsplit(OnlyPairTarget(*controller)).succeeded());
  const int first_index = model->GetIndexOfTab(first.Get());
  const int second_index = model->GetIndexOfTab(second.Get());
  ASSERT_GE(first_index, 0);
  ASSERT_GE(second_index, 0);
  std::vector<int> indices = {first_index, second_index};
  std::ranges::sort(indices);
  model->RestoreSplit(native_id, indices, split_tabs::SplitTabVisualData());
  const SplitPaneWindowSnapshot after = controller->GetSnapshot();
  ASSERT_EQ(1u, after.splits.size());
  EXPECT_NE(old_identifier, after.splits[0].split_instance_identifier);
  const base::Uuid new_uuid =
      base::Uuid::ParseLowercase(after.splits[0].split_instance_identifier);
  EXPECT_TRUE(new_uuid.is_valid());
  EXPECT_EQ(after.splits[0].split_instance_identifier,
            new_uuid.AsLowercaseString());
  EXPECT_EQ(native_id, first.Get()->GetSplit().value());
  EXPECT_EQ(native_id, second.Get()->GetSplit().value());
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       SplitInstanceIdentifierSurvivesWorkspaceRoundTrip) {
  const tabs::TabHandle first = AddTab();
  const tabs::TabHandle second = AddTab();
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  const SplitPaneWindowSnapshot before = controller->GetSnapshot();
  ASSERT_EQ(1u, before.splits.size());
  const std::string identifier = before.splits[0].split_instance_identifier;
  const std::string workspace =
      *workspace_controller_->GetSnapshot().active_workspace_identifier;
  const WorkspaceOperationResult other =
      workspace_controller_->CreateWorkspace(u"Other");
  ASSERT_TRUE(other.succeeded());
  const SplitPaneWindowSnapshot away = controller->GetSnapshot();
  EXPECT_TRUE(away.splits.empty());
  ASSERT_TRUE(workspace_controller_->SwitchWorkspace(workspace).succeeded());
  const SplitPaneWindowSnapshot after = controller->GetSnapshot();
  ASSERT_EQ(1u, after.splits.size());
  EXPECT_EQ(identifier, after.splits[0].split_instance_identifier);
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       MemberReplacementRegeneratesProductIdentity) {
  const tabs::TabHandle first = AddTab();
  const tabs::TabHandle second = AddTab();
  const tabs::TabHandle third = AddTab();
  TabListInterface* tab_list = TabListInterface::From(browser());
  ASSERT_TRUE(tab_list->CreateSplit({first, second}));
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  const SplitPaneWindowSnapshot before = controller->GetSnapshot();
  ASSERT_EQ(1u, before.splits.size());
  const std::string identifier = before.splits[0].split_instance_identifier;
  const split_tabs::SplitTabId native_id = first.Get()->GetSplit().value();
  TabStripModel* model = browser()->GetTabStripModel();
  model->ActivateTabAt(model->GetIndexOfTab(second.Get()));
  model->UpdateTabInSplit(first.Get(), model->GetIndexOfTab(third.Get()),
                          TabStripModel::SplitUpdateType::kSwap);
  const SplitPaneWindowSnapshot after = controller->GetSnapshot();
  ASSERT_EQ(1u, after.splits.size());
  EXPECT_NE(identifier, after.splits[0].split_instance_identifier);
  EXPECT_EQ(native_id, second.Get()->GetSplit().value());
  EXPECT_EQ(native_id, third.Get()->GetSplit().value());
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       CreateSplitObserverMayDestroyController) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  StopAndDestroySplitPaneControllerObserver observer(
      browser()->GetTabStripModel());
  observer.Arm(&controller);

  const SplitPaneOperationResult result =
      controller->CreateSplit(workspace_snapshot.window_identifier,
                              IdentifierFor(first), IdentifierFor(second));

  EXPECT_EQ(SplitPaneOperationStatus::kWindowUnavailable, result.status);
  EXPECT_FALSE(controller);
  ASSERT_TRUE(first.Get()->GetSplit());
  EXPECT_EQ(first.Get()->GetSplit(), second.Get()->GetSplit());
  TabListInterface::From(browser())->Unsplit(first.Get()->GetSplit().value());
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       SetLayoutObserverMayDestroyController) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(controller
                  ->CreateSplit(workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  ASSERT_TRUE(controller->SetRatio(pair, 0.25).succeeded());
  const split_tabs::SplitTabId split_id = first.Get()->GetSplit().value();
  StopAndDestroySplitPaneControllerObserver observer(
      browser()->GetTabStripModel());
  observer.Arm(&controller);

  const SplitPaneOperationResult result =
      controller->SetLayout(pair, SplitPaneLayout::kStacked);

  EXPECT_EQ(SplitPaneOperationStatus::kWindowUnavailable, result.status);
  EXPECT_FALSE(controller);
  const split_tabs::SplitTabVisualData* visual_data =
      browser()->GetTabStripModel()->GetSplitData(split_id)->visual_data();
  ASSERT_TRUE(visual_data);
  EXPECT_EQ(split_tabs::SplitTabLayout::kStacked, visual_data->split_layout());
  EXPECT_EQ(0.25, visual_data->split_ratio());
  TabListInterface::From(browser())->Unsplit(split_id);
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       UnsplitObserverMayDestroyController) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(controller
                  ->CreateSplit(workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  StopAndDestroySplitPaneControllerObserver observer(
      browser()->GetTabStripModel());
  observer.Arm(&controller);

  const SplitPaneOperationResult result = controller->Unsplit(pair);

  EXPECT_EQ(SplitPaneOperationStatus::kWindowUnavailable, result.status);
  EXPECT_FALSE(controller);
  EXPECT_FALSE(first.Get()->IsSplit());
  EXPECT_FALSE(second.Get()->IsSplit());
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       ReconcileObserverMayDestroyController) {
  const std::string default_workspace =
      *workspace_controller_->GetSnapshot().active_workspace_identifier;
  const WorkspaceOperationResult first_workspace =
      workspace_controller_->CreateWorkspace(u"First inactive");
  ASSERT_TRUE(first_workspace.succeeded());
  const tabs::TabHandle first =
      workspace_controller_->GetSnapshot().active_tab.value();
  const WorkspaceOperationResult second_workspace =
      workspace_controller_->CreateWorkspace(u"Second inactive");
  ASSERT_TRUE(second_workspace.succeeded());
  const tabs::TabHandle second =
      workspace_controller_->GetSnapshot().active_tab.value();
  ASSERT_TRUE(
      workspace_controller_->SwitchWorkspace(default_workspace).succeeded());
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  StopAndDestroySplitPaneControllerObserver observer(
      browser()->GetTabStripModel());
  observer.Arm(&controller);

  controller->ReconcileWorkspaceState();
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(controller);
  EXPECT_FALSE(first.Get()->IsSplit());
  EXPECT_FALSE(second.Get()->IsSplit());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    ReconcileWorkspaceOwnerDestructionDoesNotUseStaleController) {
  const std::string default_workspace =
      *workspace_controller_->GetSnapshot().active_workspace_identifier;
  std::vector<tabs::TabHandle> members;
  for (const std::u16string& name :
       {u"First", u"Second", u"Third", u"Fourth"}) {
    ASSERT_TRUE(workspace_controller_->CreateWorkspace(name).succeeded());
    members.push_back(workspace_controller_->GetSnapshot().active_tab.value());
  }
  ASSERT_TRUE(
      workspace_controller_->SwitchWorkspace(default_workspace).succeeded());

  TabListInterface* tab_list = TabListInterface::From(browser());
  TabStripModel* model = browser()->GetTabStripModel();
  ASSERT_TRUE(tab_list->CreateSplit({members[0], members[1]}));
  ASSERT_TRUE(tab_list->CreateSplit({members[2], members[3]}));
  const auto split_ids = model->ListSplits();
  ASSERT_EQ(2u, split_ids.size());
  const auto split_id = split_ids.begin();
  const split_tabs::SplitTabId first_reconciled = *split_id;
  const split_tabs::SplitTabId remaining = *std::next(split_id);

  int state_change_count = 0;
  auto controller = std::make_unique<SplitPaneController>(
      browser(),
      base::BindRepeating([](int* count) { ++*count; }, &state_change_count));
  StopAndDestroyWorkspaceControllerObserver observer(model);
  observer.Arm(&workspace_controller_);

  controller->ReconcileWorkspaceState();
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(1, observer.invocation_count());
  EXPECT_EQ(nullptr, WorkspaceController::From(browser()));
  EXPECT_FALSE(model->ContainsSplit(first_reconciled));
  ASSERT_TRUE(model->ContainsSplit(remaining));
  const std::vector<tabs::TabInterface*> remaining_members =
      model->GetSplitData(remaining)->ListTabs();
  ASSERT_EQ(2u, remaining_members.size());
  EXPECT_EQ(remaining, remaining_members[0]->GetSplit());
  EXPECT_EQ(remaining, remaining_members[1]->GetSplit());
  EXPECT_EQ(SplitPaneOperationStatus::kWorkspaceUnavailable,
            controller->GetSnapshot().availability_status);
  EXPECT_GT(state_change_count, 0);

  controller->Stop();
  controller.reset();
  tab_list->Unsplit(remaining);
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       MissingFocusCapabilityDoesNotActivateMember) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller = std::make_unique<SplitPaneController>(
      browser(), base::DoNothing(),
      base::BindRepeating([]() -> BrowserFocusController* { return nullptr; }));
  ASSERT_TRUE(controller
                  ->CreateSplit(workspace_snapshot.window_identifier,
                                IdentifierFor(second), IdentifierFor(first))
                  .succeeded());
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  tabs::TabInterface* active_before =
      browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(active_before);
  const std::string inactive_member = active_before == first.Get()
                                          ? IdentifierFor(second)
                                          : IdentifierFor(first);

  EXPECT_EQ(SplitPaneOperationStatus::kFocusUnavailable,
            controller->FocusPane(pair, inactive_member).status);
  EXPECT_EQ(active_before, browser()->GetTabStripModel()->GetActiveTab());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    FocusPanePreflightResolverMayDestroyControllerWithoutActivatingMember) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  std::unique_ptr<SplitPaneController> controller;
  controller = std::make_unique<SplitPaneController>(
      browser(), base::DoNothing(),
      base::BindRepeating(
          [](std::unique_ptr<SplitPaneController>* owner,
             Browser* target_browser) {
            owner->reset();
            return BrowserFocusController::From(target_browser);
          },
          &controller, browser()));
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  ASSERT_EQ(second.Get(), browser()->GetTabStripModel()->GetActiveTab());

  const SplitPaneOperationResult result =
      controller->FocusPane(pair, IdentifierFor(first));

  EXPECT_EQ(SplitPaneOperationStatus::kWindowUnavailable, result.status);
  EXPECT_FALSE(controller);
  EXPECT_EQ(second.Get(), browser()->GetTabStripModel()->GetActiveTab());
  ASSERT_TRUE(first.Get()->GetSplit());
  TabListInterface::From(browser())->Unsplit(first.Get()->GetSplit().value());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    FocusPanePostActivationResolverMayDestroyControllerWithoutContinuing) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  ASSERT_TRUE(TabListInterface::From(browser())->CreateSplit({first, second}));
  int resolver_calls = 0;
  std::unique_ptr<SplitPaneController> controller;
  controller = std::make_unique<SplitPaneController>(
      browser(), base::DoNothing(),
      base::BindRepeating(
          [](int* calls, std::unique_ptr<SplitPaneController>* owner,
             Browser* target_browser) {
            ++*calls;
            if (*calls == 2) {
              BrowserWindow::FromBrowser(target_browser)
                  ->GetLocationBar()
                  ->FocusLocation(/*is_user_initiated=*/false,
                                  /*clear_focus_if_failed=*/true);
              owner->reset();
            }
            return BrowserFocusController::From(target_browser);
          },
          &resolver_calls, &controller, browser()));
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  ASSERT_EQ(second.Get(), browser()->GetTabStripModel()->GetActiveTab());

  const SplitPaneOperationResult result =
      controller->FocusPane(pair, IdentifierFor(first));

  EXPECT_EQ(2, resolver_calls);
  EXPECT_EQ(SplitPaneOperationStatus::kWindowUnavailable, result.status);
  EXPECT_FALSE(controller);
  EXPECT_EQ(first.Get(), browser()->GetTabStripModel()->GetActiveTab());
  auto* omnibox =
      static_cast<OmniboxViewViews*>(BrowserWindow::FromBrowser(browser())
                                         ->GetLocationBar()
                                         ->GetOmniboxView());
  EXPECT_TRUE(omnibox->HasFocus());
  ASSERT_TRUE(first.Get()->GetSplit());
  TabListInterface::From(browser())->Unsplit(first.Get()->GetSplit().value());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    FocusPaneActivationMayDestroyControllerWithoutContinuingFocusRoute) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(controller
                  ->CreateSplit(workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  ASSERT_EQ(second.Get(), browser()->GetTabStripModel()->GetActiveTab());
  RunOnceOnActiveTabChangedObserver observer(
      TabListInterface::From(browser()),
      base::BindOnce(
          [](std::unique_ptr<SplitPaneController>* owner) { owner->reset(); },
          &controller));

  const SplitPaneOperationResult result =
      controller->FocusPane(pair, IdentifierFor(first));

  EXPECT_EQ(SplitPaneOperationStatus::kWindowUnavailable, result.status);
  EXPECT_FALSE(controller);
  EXPECT_EQ(first.Get(), browser()->GetTabStripModel()->GetActiveTab());
  ASSERT_TRUE(first.Get()->GetSplit());
  EXPECT_EQ(first.Get()->GetSplit(), second.Get()->GetSplit());
  TabListInterface::From(browser())->Unsplit(first.Get()->GetSplit().value());
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       StalePairTargetCannotUnsplitAfterSwap) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(controller
                  ->CreateSplit(workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());
  const SplitPanePairTarget stale_target = OnlyPairTarget(*controller);
  const std::optional<split_tabs::SplitTabId> authoritative_split =
      first.Get()->GetSplit();
  ASSERT_TRUE(authoritative_split);

  ASSERT_TRUE(controller->Swap(stale_target).succeeded());
  const SplitPanePairTarget current_target = OnlyPairTarget(*controller);
  EXPECT_EQ(stale_target.first_tab_identifier,
            current_target.second_tab_identifier);
  EXPECT_EQ(stale_target.second_tab_identifier,
            current_target.first_tab_identifier);
  EXPECT_EQ(SplitPaneOperationStatus::kPairChanged,
            controller->Unsplit(stale_target).status);
  EXPECT_EQ(authoritative_split, first.Get()->GetSplit());
  EXPECT_EQ(authoritative_split, second.Get()->GetSplit());
  EXPECT_TRUE(browser()->GetTabStripModel()->ContainsSplit(
      authoritative_split.value()));
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       BatchCreateEligibilityReportsPinAndGroupMismatch) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle source = initial->GetHandle();
  const tabs::TabHandle pinned_peer = AddTab();
  const tabs::TabHandle grouped_peer = AddTab();
  std::vector<tabs::TabHandle> ordinary_peers;
  for (int index = 0; index < 40; ++index) {
    ordinary_peers.push_back(AddTab());
  }
  TabStripModel* model = browser()->GetTabStripModel();
  model->SetTabPinned(model->GetIndexOfTab(pinned_peer.Get()), true);
  model->AddToNewGroup({model->GetIndexOfTab(grouped_peer.Get())});

  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  const std::string source_identifier = IdentifierFor(source);
  const std::string pinned_identifier = IdentifierFor(pinned_peer);
  const std::string grouped_identifier = IdentifierFor(grouped_peer);
  std::vector<std::string> ordinary_identifiers;
  ordinary_identifiers.reserve(ordinary_peers.size());
  for (tabs::TabHandle peer : ordinary_peers) {
    ordinary_identifiers.push_back(IdentifierFor(peer));
  }
  std::vector<std::string> destination_identifiers{
      source_identifier, pinned_identifier, grouped_identifier,
      "not-a-canonical-identifier"};
  destination_identifiers.insert(destination_identifiers.end(),
                                 ordinary_identifiers.begin(),
                                 ordinary_identifiers.end());
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  const auto statuses = controller->GetCreateSplitStatuses(
      workspace_snapshot.window_identifier, source_identifier,
      destination_identifiers);

  ASSERT_EQ(43u, statuses.size());
  EXPECT_EQ(SplitPaneOperationStatus::kSameTab, statuses.at(source_identifier));
  EXPECT_EQ(SplitPaneOperationStatus::kPinnedStateMismatch,
            statuses.at(pinned_identifier));
  EXPECT_EQ(SplitPaneOperationStatus::kGroupMismatch,
            statuses.at(grouped_identifier));
  for (const std::string& identifier : ordinary_identifiers) {
    EXPECT_EQ(SplitPaneOperationStatus::kSuccess, statuses.at(identifier));
  }
}

IN_PROC_BROWSER_TEST_F(LimitlessSplitPaneControllerBrowserTest,
                       ExistingPairProjectsWhileTabListIsNotEditable) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(controller
                  ->CreateSplit(workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());
  const SplitPaneWindowSnapshot editable_snapshot = controller->GetSnapshot();
  ASSERT_EQ(1u, editable_snapshot.splits.size());

  BrowserWindow::FromBrowser(browser())->DisableTabStripEditingForTesting();
  const SplitPaneWindowSnapshot blocked_snapshot = controller->GetSnapshot();
  EXPECT_EQ(SplitPaneOperationStatus::kTabListNotEditable,
            blocked_snapshot.availability_status);
  ASSERT_EQ(1u, blocked_snapshot.splits.size());
  EXPECT_EQ(editable_snapshot.splits[0].pair_identifier,
            blocked_snapshot.splits[0].pair_identifier);
  EXPECT_EQ(editable_snapshot.splits[0].first_tab_identifier,
            blocked_snapshot.splits[0].first_tab_identifier);
  EXPECT_EQ(editable_snapshot.splits[0].second_tab_identifier,
            blocked_snapshot.splits[0].second_tab_identifier);
  EXPECT_EQ(SplitPaneOperationStatus::kTabListNotEditable,
            controller->Unsplit(OnlyPairTarget(*controller)).status);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    SessionRestoreDoesNotReconcileTemporaryWorkspaceMismatch) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  const std::string default_workspace =
      *workspace_controller_->GetSnapshot().active_workspace_identifier;
  const WorkspaceOperationResult restored_workspace =
      workspace_controller_->CreateWorkspace(u"Restored");
  ASSERT_TRUE(restored_workspace.succeeded());
  ASSERT_TRUE(restored_workspace.workspace_identifier);
  ASSERT_TRUE(
      workspace_controller_->SwitchWorkspace(default_workspace).succeeded());

  const std::string first_identifier = IdentifierFor(first);
  const std::string second_identifier = IdentifierFor(second);
  const std::optional<std::string> window_session_data =
      workspace_controller_->SerializeWindowForSession();
  ASSERT_TRUE(window_session_data);
  const std::optional<split_tabs::SplitTabId> original_split =
      TabListInterface::From(browser())->CreateSplit({first, second});
  ASSERT_TRUE(original_split);

  auto controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  const SplitPanePairTarget pair = OnlyPairTarget(*controller);
  workspace_controller_->BeginSessionRestore();
  ASSERT_TRUE(
      workspace_controller_->RestoreWindowFromSession(*window_session_data));
  ASSERT_TRUE(workspace_controller_->RestoreTabFromSession(
      first, BuildWorkspaceTabSessionData(
                 first_identifier, *restored_workspace.workspace_identifier)));

  EXPECT_EQ(SplitPaneOperationStatus::kWorkspaceRestoreInProgress,
            controller->GetSnapshot().availability_status);
  EXPECT_EQ(SplitPaneOperationStatus::kWorkspaceRestoreInProgress,
            controller->Unsplit(pair).status);
  EXPECT_EQ(WorkspaceOperationStatus::kSessionRestoreInProgress,
            workspace_controller_->SwitchWorkspace(default_workspace).status);

  controller->ReconcileWorkspaceState();
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(original_split, first.Get()->GetSplit());
  EXPECT_EQ(original_split, second.Get()->GetSplit());
  ASSERT_TRUE(workspace_controller_->RestoreTabFromSession(
      second,
      BuildWorkspaceTabSessionData(second_identifier,
                                   *restored_workspace.workspace_identifier)));
  workspace_controller_->FinalizeSessionRestore();
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(original_split, first.Get()->GetSplit());
  EXPECT_EQ(original_split, second.Get()->GetSplit());
  ASSERT_TRUE(workspace_controller_
                  ->SwitchWorkspace(*restored_workspace.workspace_identifier)
                  .succeeded());
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(1u, controller->GetSnapshot().splits.size());
  EXPECT_EQ(first_identifier,
            controller->GetSnapshot().splits[0].first_tab_identifier);
  EXPECT_EQ(second_identifier,
            controller->GetSnapshot().splits[0].second_tab_identifier);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    DefensiveSingleMemberTransferDissolvesPairAndLeavesSurvivorInSource) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle survivor = initial->GetHandle();
  const tabs::TabHandle transferred = AddTab();
  AddTab();
  const WorkspaceWindowSnapshot source_workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto source_controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(source_controller
                  ->CreateSplit(source_workspace_snapshot.window_identifier,
                                IdentifierFor(survivor),
                                IdentifierFor(transferred))
                  .succeeded());

  Browser* destination = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination);
  destination->GetWindow()->Show();
  auto destination_workspace = std::make_unique<WorkspaceController>(
      destination, WorkspacePersistenceCallbacks{}, base::DoNothing());
  auto destination_controller =
      std::make_unique<SplitPaneController>(destination, base::DoNothing());

  TabListInterface::From(browser())->MoveTabToWindow(
      transferred, destination->GetSessionID(), /*destination_index=*/0);
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(browser(), survivor.Get()->GetBrowserWindowInterface());
  EXPECT_EQ(destination, transferred.Get()->GetBrowserWindowInterface());
  EXPECT_FALSE(survivor.Get()->IsSplit());
  EXPECT_FALSE(transferred.Get()->IsSplit());
  EXPECT_TRUE(source_controller->GetSnapshot().splits.empty());
  EXPECT_TRUE(destination_controller->GetSnapshot().splits.empty());

  destination_controller.reset();
  destination_workspace->Stop();
  destination_workspace.reset();
  CloseBrowserSynchronously(destination);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneControllerBrowserTest,
    WorkspaceMoveAfterTransferredPairUsesUnsplitFirstPolicy) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  AddTab();
  const WorkspaceWindowSnapshot source_workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto source_controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(source_controller
                  ->CreateSplit(source_workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());

  Browser* destination = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination);
  destination->GetWindow()->Show();
  auto destination_workspace = std::make_unique<WorkspaceController>(
      destination, WorkspacePersistenceCallbacks{}, base::DoNothing());
  auto destination_controller =
      std::make_unique<SplitPaneController>(destination, base::DoNothing());
  TabStripModel* source_model = browser()->GetTabStripModel();
  std::vector<int> transferred_indices = {
      source_model->GetIndexOfTab(first.Get()),
      source_model->GetIndexOfTab(second.Get())};
  std::ranges::sort(transferred_indices);
  chrome::MoveTabsToExistingWindow(browser(), destination, transferred_indices);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(first.Get()->IsSplit());
  ASSERT_TRUE(second.Get()->IsSplit());
  ASSERT_EQ(1u, destination_controller->GetSnapshot().splits.size());

  const WorkspaceOperationResult ineligible_workspace =
      destination_workspace->CreateWorkspace(u"Ineligible split mate");
  ASSERT_TRUE(ineligible_workspace.succeeded());
  ASSERT_TRUE(destination_workspace
                  ->MoveTabToWorkspace(
                      first, *ineligible_workspace.workspace_identifier)
                  .succeeded());
  destination_controller->ReconcileWorkspaceState();
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(first.Get()->IsSplit());
  EXPECT_FALSE(second.Get()->IsSplit());
  EXPECT_TRUE(destination_controller->GetSnapshot().splits.empty());

  destination_controller.reset();
  destination_workspace->Stop();
  destination_workspace.reset();
  CloseBrowserSynchronously(destination);
}

}  // namespace
}  // namespace limitless
