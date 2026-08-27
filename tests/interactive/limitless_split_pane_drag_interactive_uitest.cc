// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <memory>
#include <string>

#include "base/check.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/test/run_until.h"
#include "build/build_config.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/tabs/dragging/tab_drag_controller.h"
#include "chrome/browser/ui/views/tabs/dragging/tab_drag_controller_interactive_test_mixin.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/test/browser_test.h"
#include "limitless_split_pane_controller.h"
#include "limitless_workspace_controller.h"
#include "url/gurl.h"

namespace limitless {
namespace {

class LimitlessSplitPaneDragInteractiveUiTest
    : public TabDragControllerInteractiveTestMixin<InProcessBrowserTest> {
 public:
  void SetUpOnMainThread() override {
    TabDragControllerInteractiveTestMixin<
        InProcessBrowserTest>::SetUpOnMainThread();
    ASSERT_EQ(nullptr, WorkspaceController::From(browser()));
    workspace_controller_ = std::make_unique<WorkspaceController>(
        browser(), WorkspacePersistenceCallbacks{}, base::DoNothing());
    ASSERT_TRUE(workspace_controller_->GetSnapshot().enabled);
  }

  void TearDownOnMainThread() override {
    workspace_controller_->Stop();
    workspace_controller_.reset();
    TabDragControllerInteractiveTestMixin<
        InProcessBrowserTest>::TearDownOnMainThread();
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

  std::unique_ptr<WorkspaceController> workspace_controller_;
};

#if BUILDFLAG(IS_MAC) && defined(ARCH_CPU_ARM64)
#define MAYBE_RealViewsWholeSplitDropPreservesAuthoritativePair \
  DISABLED_RealViewsWholeSplitDropPreservesAuthoritativePair
#else
#define MAYBE_RealViewsWholeSplitDropPreservesAuthoritativePair \
  RealViewsWholeSplitDropPreservesAuthoritativePair
#endif

IN_PROC_BROWSER_TEST_F(
    LimitlessSplitPaneDragInteractiveUiTest,
    MAYBE_RealViewsWholeSplitDropPreservesAuthoritativePair) {
  tabs::TabInterface* initial = browser()->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(initial);
  const tabs::TabHandle first = initial->GetHandle();
  const tabs::TabHandle second = AddTab();
  AddTab();
  ASSERT_TRUE(workspace_controller_->ActivateTab(first).succeeded());
  const WorkspaceWindowSnapshot source_workspace_snapshot =
      workspace_controller_->GetSnapshot();
  auto source_controller =
      std::make_unique<SplitPaneController>(browser(), base::DoNothing());
  ASSERT_TRUE(source_controller
                  ->CreateSplit(source_workspace_snapshot.window_identifier,
                                IdentifierFor(first), IdentifierFor(second))
                  .succeeded());
  ASSERT_EQ(first.Get()->GetSplit(), second.Get()->GetSplit());
  ASSERT_TRUE(first.Get()->GetSplit());
  const split_tabs::SplitTabId original_split_identifier =
      *first.Get()->GetSplit();
  const SplitPaneWindowSnapshot original_snapshot =
      source_controller->GetSnapshot();
  ASSERT_EQ(1u, original_snapshot.splits.size());
  const std::string first_product_identifier =
      original_snapshot.splits[0].first_tab_identifier;
  const std::string second_product_identifier =
      original_snapshot.splits[0].second_tab_identifier;

  Browser* destination = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination);
  destination->GetWindow()->Show();
  ASSERT_EQ(nullptr, WorkspaceController::From(destination));
  auto destination_workspace = std::make_unique<WorkspaceController>(
      destination, WorkspacePersistenceCallbacks{}, base::DoNothing());
  auto destination_controller =
      std::make_unique<SplitPaneController>(destination, base::DoNothing());

  const gfx::Rect source_bounds = browser()->GetWindow()->GetBounds();
  gfx::Rect destination_bounds = source_bounds;
  destination_bounds.Offset(source_bounds.width() + 20, 0);
  destination->GetWindow()->SetBounds(destination_bounds);
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()));
  BrowserView* source_view = BrowserView::GetBrowserViewForBrowser(browser());
  BrowserView* destination_view =
      BrowserView::GetBrowserViewForBrowser(destination);
  ASSERT_TRUE(source_view);
  ASSERT_TRUE(destination_view);
  TabStrip* source_tab_strip = source_view->horizontal_tab_strip_for_testing();
  TabStrip* destination_tab_strip =
      destination_view->horizontal_tab_strip_for_testing();
  ASSERT_TRUE(source_tab_strip);
  ASSERT_TRUE(destination_tab_strip);
  const int dragged_index =
      browser()->GetTabStripModel()->GetIndexOfTab(second.Get());
  ASSERT_NE(TabStripModel::kNoTab, dragged_index);
  ASSERT_TRUE(PressInputAtCenter(source_tab_strip->tab_at(dragged_index)));
  ASSERT_TRUE(DragInputToCenter(destination_tab_strip->tab_at(0)));
  ASSERT_TRUE(ReleaseInput(/*id=*/0, /*async=*/true));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return !TabDragController::IsActive() && second.Get() &&
           second.Get()->GetBrowserWindowInterface() == destination &&
           TabListInterface::CanEditTabList(*browser()->GetProfile());
  }));
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(TabListInterface::CanEditTabList(*browser()->GetProfile()));
  const SplitPaneWindowSnapshot source_snapshot =
      source_controller->GetSnapshot();
  const SplitPaneWindowSnapshot destination_snapshot =
      destination_controller->GetSnapshot();
  EXPECT_EQ(destination, first.Get()->GetBrowserWindowInterface());
  EXPECT_TRUE(first.Get()->IsSplit());
  EXPECT_TRUE(second.Get()->IsSplit());
  EXPECT_EQ(original_split_identifier, first.Get()->GetSplit());
  EXPECT_EQ(original_split_identifier, second.Get()->GetSplit());
  EXPECT_TRUE(source_snapshot.splits.empty());
  ASSERT_EQ(1u, destination_snapshot.splits.size());
  EXPECT_EQ(first_product_identifier,
            destination_snapshot.splits[0].first_tab_identifier);
  EXPECT_EQ(second_product_identifier,
            destination_snapshot.splits[0].second_tab_identifier);

  destination_controller.reset();
  destination_workspace->Stop();
  destination_workspace.reset();
  CloseBrowserSynchronously(destination);
}

}  // namespace
}  // namespace limitless
