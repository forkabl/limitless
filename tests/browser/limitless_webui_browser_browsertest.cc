// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>

#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui_browser/webui_browser_ui.h"
#include "chrome/browser/ui/webui_browser/webui_browser_window.h"
#include "chrome/common/chrome_features.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/surface_embed/common/features.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "ui/base/base_window.h"
#include "url/gurl.h"

namespace {

constexpr char kExpectedBuildId[] = "limitless-phase1-macos-arm64-2b4613f0";
constexpr char kExpectedChromiumRevision[] =
    "2b4613f0a24d234b139f1246028239a2f5dde58c";

class LimitlessWebUIBrowserTest : public InProcessBrowserTest {
 public:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kWebium, surface_embed::features::kSurfaceEmbed}, {});
    InProcessBrowserTest::SetUp();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       LoadsSemanticLimitlessResources) {
  WebUIBrowserWindow* window = WebUIBrowserWindow::FromBrowser(browser());
  ASSERT_TRUE(window);

  content::WebContents* shell_contents =
      window->GetWebUIBrowserUI()->web_ui()->GetWebContents();
  ASSERT_TRUE(shell_contents);
  ASSERT_TRUE(content::WaitForLoadStop(shell_contents));

  base::test::ScopedRunLoopTimeout readiness_timeout(FROM_HERE,
                                                     base::Seconds(20));
  ASSERT_TRUE(base::test::RunUntil([shell_contents]() {
    return content::EvalJs(
               shell_contents,
               "document.querySelector('#limitless-shell')?.dataset.ready "
               "=== 'true'")
        .ExtractBool();
  })) << "Limitless shell did not publish its semantic readiness marker";

  EXPECT_TRUE(
      content::EvalJs(shell_contents,
                      "document.querySelector('#limitless-shell') !== null")
          .ExtractBool());
  EXPECT_EQ(kExpectedBuildId,
            content::EvalJs(
                shell_contents,
                "document.querySelector('#limitless-build-marker')?.dataset."
                "buildId ?? ''")
                .ExtractString());
  EXPECT_EQ(kExpectedChromiumRevision,
            content::EvalJs(
                shell_contents,
                "document.querySelector('#limitless-build-marker')?.dataset."
                "chromiumRevision ?? ''")
                .ExtractString());
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       NormalWindowUsesWebUIBrowserAndSurfaceEmbed) {
  WebUIBrowserWindow* window = WebUIBrowserWindow::FromBrowser(browser());
  ASSERT_TRUE(window);
  EXPECT_EQ(nullptr, BrowserView::GetBrowserViewForBrowser(browser()));

  content::WebContents* shell_contents =
      window->GetWebUIBrowserUI()->web_ui()->GetWebContents();
  ASSERT_TRUE(shell_contents);
  ASSERT_TRUE(content::WaitForLoadStop(shell_contents));

  base::test::ScopedRunLoopTimeout readiness_timeout(FROM_HERE,
                                                     base::Seconds(20));
  ASSERT_TRUE(base::test::RunUntil([shell_contents]() {
    return content::EvalJs(
               shell_contents,
               "document.querySelector('#limitless-shell')?.dataset.ready "
               "=== 'true'")
        .ExtractBool();
  })) << "Limitless shell did not become ready before SurfaceEmbed checking";

  content::WebContents* tab_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(tab_contents);
  EXPECT_NE(shell_contents, tab_contents);
  base::test::ScopedRunLoopTimeout surface_embed_timeout(FROM_HERE,
                                                         base::Seconds(20));
  EXPECT_TRUE(base::test::RunUntil([tab_contents]() {
    return tab_contents->GetSurfaceEmbedConnector() != nullptr;
  })) << "Active tab did not attach through SurfaceEmbed after the Limitless "
         "shell became ready";
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       PopupWindowRetainsBrowserViewFallback) {
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP,
                                   browser()->GetProfile(),
                                   /*from_user_gesture=*/true);
  params.omit_from_session_restore = true;
  BrowserWindowInterface* popup = CreateBrowserWindow(std::move(params));
  ASSERT_TRUE(popup);

  chrome::AddTabAt(popup, GURL("about:blank"), -1, true);
  popup->GetWindow()->Show();

  EXPECT_EQ(nullptr, WebUIBrowserWindow::FromBrowser(popup));
  EXPECT_NE(nullptr, BrowserView::GetBrowserViewForBrowser(popup));

  CloseBrowserSynchronously(popup);
}

}  // namespace
