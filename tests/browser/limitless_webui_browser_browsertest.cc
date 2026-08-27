// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui_browser/browser.mojom.h"
#include "chrome/browser/ui/webui_browser/webui_browser_ui.h"
#include "chrome/browser/ui/webui_browser/webui_browser_window.h"
#include "chrome/common/chrome_features.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/guest_contents/browser/guest_contents_handle.h"
#include "components/surface_embed/common/features.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/test_support/test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "ui/base/base_window.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/point.h"
#include "url/gurl.h"

namespace {

constexpr char kExpectedBuildId[] = "limitless-phase1-macos-arm64-2b4613f0";
constexpr char kExpectedChromiumRevision[] =
    "2b4613f0a24d234b139f1246028239a2f5dde58c";

std::unique_ptr<net::test_server::HttpResponse> HandleGateTwoRequest(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/html");
  if (request.relative_url == "/page-a") {
    response->set_content(R"(
      <!doctype html>
      <meta charset="utf-8">
      <title>Limitless Page A</title>
      <h1 id="page-a">Page A</h1>
      <output id="reload-count"></output>
      <script>
        const reloadCount =
            Number(sessionStorage.getItem('reload-count') || '0') + 1;
        sessionStorage.setItem('reload-count', String(reloadCount));
        document.querySelector('#reload-count').textContent = reloadCount;
        window.limitlessPageState = {value: reloadCount};
      </script>
    )");
  } else if (request.relative_url == "/page-a-next") {
    response->set_content(R"(
      <!doctype html>
      <meta charset="utf-8">
      <title>Limitless Page A Next</title>
      <h1 id="page-a-next">Page A Next</h1>
    )");
  } else if (request.relative_url == "/page-b") {
    response->set_content(R"(
      <!doctype html>
      <meta charset="utf-8">
      <title>Limitless Page B</title>
      <h1 id="page-b">Page B</h1>
    )");
  } else {
    response->set_code(net::HTTP_NOT_FOUND);
  }
  return response;
}

struct ShellContentIdentity {
  std::string tab_id;
  std::string webview_identity;
  std::string embed_identity;
  std::string content_id;
  bool active = false;
  bool embed_connected = false;
  std::string visibility;
};

class TargetNavigationStartObserver : public content::WebContentsObserver {
 public:
  TargetNavigationStartObserver(content::WebContents* contents,
                                const GURL& target_url)
      : content::WebContentsObserver(contents), target_url_(target_url) {}

  bool target_started() const { return target_started_; }

 private:
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override {
    if (navigation_handle->GetURL() == target_url_) {
      target_started_ = true;
    }
  }

  const GURL target_url_;
  bool target_started_ = false;
};

class FakeWebUIBrowserPage : public webui_browser::mojom::Page {
 public:
  mojo::PendingRemote<webui_browser::mojom::Page> BindAndGetRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  void SetFocusToLocationBar(bool is_user_initiated) override {}
  void SetReloadStopState(bool is_loading) override {}
  void ShowSidePanel(const base::UnguessableToken& guest_contents_id,
                     const std::string& title) override {}
  void CloseSidePanel() override {}
  void OnFullscreenModeChanged(
      bool is_fullscreen,
      std::optional<webui_browser::mojom::FullscreenContext> context) override {
  }
  void OnPaintAsActiveChanged(bool active) override {}

 private:
  mojo::Receiver<webui_browser::mojom::Page> receiver_{this};
};

template <typename Callback>
void WithPageHandler(Browser* handler_browser, Callback callback) {
  WebUIBrowserWindow* window = WebUIBrowserWindow::FromBrowser(handler_browser);
  ASSERT_TRUE(window);
  WebUIBrowserUI* browser_ui = window->GetWebUIBrowserUI();
  ASSERT_TRUE(browser_ui);
  ASSERT_TRUE(content::WaitForLoadStop(browser_ui->web_ui()->GetWebContents()));
  ASSERT_TRUE(base::test::RunUntil(
      [browser_ui]() { return browser_ui->page() != nullptr; }));

  FakeWebUIBrowserPage page;
  mojo::Remote<webui_browser::mojom::PageHandlerFactory> factory;
  browser_ui->BindInterface(factory.BindNewPipeAndPassReceiver());
  mojo::Remote<webui_browser::mojom::PageHandler> page_handler;
  factory->CreatePageHandler(page.BindAndGetRemote(),
                             page_handler.BindNewPipeAndPassReceiver());
  factory.FlushForTesting();
  std::optional<int32_t> tab_strip_inset;
  page_handler->GetTabStripInset(base::BindLambdaForTesting(
      [&tab_strip_inset](int32_t width) { tab_strip_inset = width; }));
  ASSERT_TRUE(base::test::RunUntil(
      [&tab_strip_inset]() { return tab_strip_inset.has_value(); }));
  ASSERT_TRUE(factory.is_connected());
  ASSERT_TRUE(page_handler.is_connected());

  callback(page_handler);
}

void ExpectTabIdRejected(Browser* handler_browser,
                         const tabs_api::NodeId& tab_id,
                         std::string_view expected_error) {
  WithPageHandler(handler_browser, [&](auto& page_handler) {
    bool callback_called = false;
    mojo::Remote<webui_browser::mojom::GuestHandler> guest_handler;
    mojo::test::BadMessageObserver bad_message_observer;
    base::test::ScopedRunLoopTimeout rejection_timeout(FROM_HERE,
                                                       base::Seconds(5));
    page_handler->GetGuestIdForTabId(
        tab_id, guest_handler.BindNewPipeAndPassReceiver(),
        base::BindLambdaForTesting(
            [&callback_called](const base::UnguessableToken&) {
              callback_called = true;
            }));
    page_handler.FlushForTesting();

    EXPECT_EQ(expected_error, bad_message_observer.WaitForBadMessage());
    EXPECT_FALSE(callback_called);
    EXPECT_TRUE(base::test::RunUntil([&page_handler, &guest_handler]() {
      return !page_handler.is_connected() && !guest_handler.is_connected();
    }));
  });
}

void ExpectTabHandleAccepted(Browser* handler_browser,
                             tabs::TabInterface* tab) {
  WithPageHandler(handler_browser, [&](auto& page_handler) {
    bool callback_called = false;
    mojo::Remote<webui_browser::mojom::GuestHandler> guest_handler;
    page_handler->GetGuestIdForTabId(
        tabs_api::NodeId::FromTabHandle(tab->GetHandle()),
        guest_handler.BindNewPipeAndPassReceiver(),
        base::BindLambdaForTesting(
            [&callback_called](const base::UnguessableToken&) {
              callback_called = true;
            }));
    page_handler.FlushForTesting();

    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(page_handler.is_connected());
    EXPECT_TRUE(guest_handler.is_connected());
  });
}

template <typename Callback>
void WithGuestHandler(Browser* handler_browser,
                      tabs::TabInterface* tab,
                      Callback callback) {
  WithPageHandler(handler_browser, [&](auto& page_handler) {
    bool callback_called = false;
    mojo::Remote<webui_browser::mojom::GuestHandler> guest_handler;
    page_handler->GetGuestIdForTabId(
        tabs_api::NodeId::FromTabHandle(tab->GetHandle()),
        guest_handler.BindNewPipeAndPassReceiver(),
        base::BindLambdaForTesting(
            [&callback_called](const base::UnguessableToken&) {
              callback_called = true;
            }));
    page_handler.FlushForTesting();

    ASSERT_TRUE(callback_called);
    ASSERT_TRUE(page_handler.is_connected());
    ASSERT_TRUE(guest_handler.is_connected());
    callback(page_handler, guest_handler);
  });
}

void ExpectForeignTabHandleRejected(Browser* handler_browser,
                                    Browser* foreign_browser) {
  TabStripModel* foreign_tab_strip = foreign_browser->GetTabStripModel();
  tabs::TabInterface* foreign_tab =
      foreign_tab_strip->GetTabAtIndex(foreign_tab_strip->active_index());
  ASSERT_TRUE(foreign_tab);
  ASSERT_EQ(TabStripModel::kNoTab,
            handler_browser->GetTabStripModel()->GetIndexOfTab(foreign_tab));

  ExpectTabIdRejected(handler_browser,
                      tabs_api::NodeId::FromTabHandle(foreign_tab->GetHandle()),
                      "Tab id is not owned by this browser");
}

class LimitlessWebUIBrowserTest : public InProcessBrowserTest {
 public:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kWebium, surface_embed::features::kSurfaceEmbed}, {});
    InProcessBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    https_server_.SetSSLConfig(net::EmbeddedTestServer::CERT_TEST_NAMES);
    https_server_.RegisterRequestHandler(
        base::BindRepeating(&HandleGateTwoRequest));
    ASSERT_TRUE(https_server_.Start());
  }

 protected:
  content::WebContents* GetShellContents() {
    WebUIBrowserWindow* window = WebUIBrowserWindow::FromBrowser(browser());
    if (!window) {
      return nullptr;
    }
    return window->GetWebUIBrowserUI()->web_ui()->GetWebContents();
  }

  bool AddressHasFocus() {
    return content::EvalJs(GetShellContents(), R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return input instanceof HTMLInputElement &&
                   document.activeElement === app &&
                   app.shadowRoot.activeElement === address &&
                   address.shadowRoot.activeElement === searchboxInput &&
                   searchboxInput.shadowRoot.activeElement === input;
             })()
           )")
        .ExtractBool();
  }

  std::optional<gfx::Point> GetAddressInputCenter() {
    auto eval_result = content::EvalJs(GetShellContents(), R"(
      (() => {
        const app = document.querySelector('webui-browser-app');
        const address = app?.shadowRoot?.querySelector('#address');
        const searchboxInput = address?.shadowRoot?.querySelector('#input');
        const input = searchboxInput?.shadowRoot?.querySelector('#input');
        if (!(input instanceof HTMLInputElement)) {
          return {ready: false};
        }
        const bounds = input.getBoundingClientRect();
        return {
          ready: bounds.width > 0 && bounds.height > 0,
          x: Math.floor(bounds.x + bounds.width / 2),
          y: Math.floor(bounds.y + bounds.height / 2),
        };
      })()
    )");
    const base::DictValue& result = eval_result.ExtractDict();
    if (!result.FindBool("ready").value_or(false)) {
      return std::nullopt;
    }
    std::optional<int> x = result.FindInt("x");
    std::optional<int> y = result.FindInt("y");
    if (!x.has_value() || !y.has_value()) {
      return std::nullopt;
    }
    return gfx::Point(*x, *y);
  }

  int AddressValueLength() {
    return content::EvalJs(GetShellContents(), R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return input instanceof HTMLInputElement ? input.value.length : -1;
             })()
           )")
        .ExtractInt();
  }

  bool AddressValueIsFullySelected(int expected_length) {
    return content::EvalJs(GetShellContents(),
                           content::JsReplace(R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return input instanceof HTMLInputElement &&
                   input.value.length === $1 && input.selectionStart === 0 &&
                   input.selectionEnd === $1;
             })()
           )",
                                              expected_length))
        .ExtractBool();
  }

  bool AddressValueEquals(const std::string& expected) {
    return content::EvalJs(GetShellContents(), content::JsReplace(R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return input instanceof HTMLInputElement && input.value === $1;
             })()
           )",
                                                                  expected))
        .ExtractBool();
  }

  bool AddressEmptyStateSettled() {
    return content::EvalJs(GetShellContents(), R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return input instanceof HTMLInputElement && input.value === '' &&
                   address.activeQueryId === -1 &&
                   (address.lastQueriedInput === null ||
                    address.lastQueriedInput === '') &&
                   address.selectedMatchIndex === -1 &&
                   address.selectedMatch === null;
             })()
           )")
        .ExtractBool();
  }

  bool AddressHasCurrentDefaultMatch(const GURL& expected) {
    return content::EvalJs(GetShellContents(),
                           content::JsReplace(R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return input instanceof HTMLInputElement && input.value === $1 &&
                   address.activeQueryId >= 0 &&
                   address.result?.queryId === address.activeQueryId &&
                   address.lastQueriedInput === $1 &&
                   address.selectedMatch?.allowedToBeDefaultMatch === true &&
                   address.selectedMatch.destinationUrl === $1;
             })()
           )",
                                              expected.spec()))
        .ExtractBool();
  }

  std::string AddressStateDescription() {
    return content::EvalJs(GetShellContents(), R"(
             (() => {
               const app = document.querySelector('webui-browser-app');
               const address = app?.shadowRoot?.querySelector('#address');
               const searchboxInput =
                   address?.shadowRoot?.querySelector('#input');
               const input = searchboxInput?.shadowRoot?.querySelector('#input');
               return JSON.stringify({
                 inputValue: input?.value ?? null,
                 activeQueryId: address?.activeQueryId ?? null,
                 resultQueryId: address?.result?.queryId ?? null,
                 lastQueriedInput: address?.lastQueriedInput ?? null,
                 selectedMatchIndex: address?.selectedMatchIndex ?? null,
                 selectedDestination:
                     address?.selectedMatch?.destinationUrl ?? null,
                 selectedAllowed:
                     address?.selectedMatch?.allowedToBeDefaultMatch ?? null,
               });
             })()
           )")
        .ExtractString();
  }

  void NavigateThroughVisibleAddress(content::WebContents* tab_contents,
                                     const GURL& url) {
    content::WebContents* shell_contents = GetShellContents();
    ASSERT_TRUE(shell_contents);
    ASSERT_TRUE(content::WaitForLoadStop(tab_contents))
        << "The active tab's previous navigation did not settle before address "
           "input";

    std::optional<gfx::Point> address_center;
    ASSERT_TRUE(base::test::RunUntil([this, &address_center]() {
      address_center = GetAddressInputCenter();
      return address_center.has_value();
    })) << "The live shell address input did not expose clickable bounds";
    content::SimulateMouseClickAt(shell_contents, /*modifiers=*/0,
                                  blink::WebMouseEvent::Button::kLeft,
                                  *address_center);
    ASSERT_TRUE(base::test::RunUntil([this]() { return AddressHasFocus(); }))
        << "A visible click did not focus the live shell address control";

    const int current_length = AddressValueLength();
    ASSERT_GE(current_length, 0);
    content::SimulateKeyPressWithoutChar(
        shell_contents, ui::DomKey::END, ui::DomCode::END, ui::VKEY_END,
        /*control=*/false, /*shift=*/false, /*alt=*/false, /*command=*/false);
    if (current_length > 0) {
      content::SimulateKeyPressWithoutChar(
          shell_contents, ui::DomKey::HOME, ui::DomCode::HOME, ui::VKEY_HOME,
          /*control=*/false, /*shift=*/true, /*alt=*/false,
          /*command=*/false);
      ASSERT_TRUE(base::test::RunUntil([this, current_length]() {
        return AddressValueIsFullySelected(current_length);
      })) << "Shift-Home did not select the live shell address value";
      content::SimulateKeyPress(shell_contents, ui::DomKey::BACKSPACE,
                                ui::DomCode::BACKSPACE, ui::VKEY_BACK,
                                /*control=*/false, /*shift=*/false,
                                /*alt=*/false, /*command=*/false);
    }
    ASSERT_TRUE(base::test::RunUntil([this]() {
      return AddressEmptyStateSettled();
    })) << "The empty address did not retire its prior autocomplete state: "
        << AddressStateDescription();

    for (char character : url.spec()) {
      content::SimulateCharTyped(shell_contents,
                                 static_cast<unsigned char>(character));
    }
    ASSERT_TRUE(base::test::RunUntil([this, &url]() {
      return AddressValueEquals(url.spec());
    })) << "Keyboard input did not reach the live shell address control";
    ASSERT_TRUE(base::test::RunUntil([this, &url]() {
      return AddressHasCurrentDefaultMatch(url);
    })) << "The live shell did not publish a current default address match: "
        << AddressStateDescription();

    // Filter for the address submission so that no preceding navigation can
    // satisfy this observer and hide a dropped Enter event.
    content::TestNavigationObserver navigation(url);
    navigation.WatchWebContents(tab_contents);
    content::SimulateKeyPress(shell_contents, ui::DomKey::ENTER,
                              ui::DomCode::ENTER, ui::VKEY_RETURN,
                              /*control=*/false, /*shift=*/false,
                              /*alt=*/false, /*command=*/false);
    navigation.Wait();
    ASSERT_TRUE(navigation.last_navigation_succeeded());
    ASSERT_EQ(url, navigation.last_navigation_url());
    ASSERT_EQ(url, tab_contents->GetLastCommittedURL());
    ASSERT_TRUE(content::WaitForLoadStop(tab_contents));
  }

  void NavigateThroughBrowserCommand(content::WebContents* tab_contents,
                                     int command_id,
                                     const GURL& expected_url) {
    content::TestNavigationObserver navigation(tab_contents, 1);
    ASSERT_TRUE(chrome::ExecuteCommand(browser(), command_id));
    navigation.Wait();
    ASSERT_TRUE(navigation.last_navigation_succeeded());
    ASSERT_EQ(expected_url, tab_contents->GetLastCommittedURL());
    ASSERT_TRUE(content::WaitForLoadStop(tab_contents));
  }

  bool ShellStateMatches(int tab_count,
                         int active_index,
                         const GURL& active_url) {
    return content::EvalJs(
               GetShellContents(),
               content::JsReplace(R"(
                 (() => {
                   const app =
                       document.querySelector('webui-browser-app');
                   const tabstrip =
                       app?.shadowRoot?.querySelector('#tabstrip');
                   const tabs = [...(tabstrip?.shadowRoot?.querySelectorAll(
                       'webui-browser-tab') ?? [])];
                   return tabs.length === $1 &&
                       tabs.filter(tab => tab.tabData.isActive).length === 1 &&
                       tabs[$2]?.tabData.isActive === true &&
                       tabs[$2]?.tabData.url === $3;
                 })()
               )",
                                  tab_count, active_index, active_url.spec()))
        .ExtractBool();
  }

  std::optional<ShellContentIdentity> GetShellContentIdentity(int index) {
    auto eval_result =
        content::EvalJs(GetShellContents(), content::JsReplace(R"(
          (() => {
            const app = document.querySelector('webui-browser-app');
            const tabstrip = app?.shadowRoot?.querySelector('#tabstrip');
            const contentRegion =
                app?.shadowRoot?.querySelector('#contentRegion');
            const tabs = tabstrip?.shadowRoot?.querySelectorAll(
                'webui-browser-tab');
            const tab = tabs?.item($1);
            const webviews = [...(contentRegion?.shadowRoot?.querySelectorAll(
                'cr-tab-webview') ?? [])];
            const webview = webviews.find(
                candidate => candidate.tabId === tab?.tabData?.id);
            const embed = webview?.shadowRoot?.querySelector(
                'embed[type="application/x-chromium-surface-embed"]');
            if (!tab || !webview || !embed) {
              return {ready: false};
            }

            const identities = globalThis.__limitlessGateTwoIdentities ??=
                {nodes: new WeakMap(), next: 1};
            const identityFor = node => {
              let identity = identities.nodes.get(node);
              if (!identity) {
                identity = `node-${identities.next++}`;
                identities.nodes.set(node, identity);
              }
              return identity;
            };

            return {
              ready: true,
              tabId: tab.tabData.id,
              webviewIdentity: identityFor(webview),
              embedIdentity: identityFor(embed),
              contentId: embed.getAttribute('data-content-id') ?? '',
              active: webview.classList.contains('active'),
              embedConnected: embed.isConnected,
              visibility: getComputedStyle(webview).visibility,
            };
          })()
        )",
                                                               index));
    const base::DictValue& result = eval_result.ExtractDict();
    if (!result.FindBool("ready").value_or(false)) {
      return std::nullopt;
    }

    const std::string* tab_id = result.FindString("tabId");
    const std::string* webview_identity = result.FindString("webviewIdentity");
    const std::string* embed_identity = result.FindString("embedIdentity");
    const std::string* content_id = result.FindString("contentId");
    const std::string* visibility = result.FindString("visibility");
    std::optional<bool> active = result.FindBool("active");
    std::optional<bool> embed_connected = result.FindBool("embedConnected");
    if (!tab_id || !webview_identity || !embed_identity || !content_id ||
        !visibility || !active.has_value() || !embed_connected.has_value()) {
      return std::nullopt;
    }

    return ShellContentIdentity{
        .tab_id = *tab_id,
        .webview_identity = *webview_identity,
        .embed_identity = *embed_identity,
        .content_id = *content_id,
        .active = *active,
        .embed_connected = *embed_connected,
        .visibility = *visibility,
    };
  }

  void ExpectSameShellContent(const ShellContentIdentity& expected,
                              const ShellContentIdentity& actual) {
    EXPECT_EQ(expected.tab_id, actual.tab_id);
    EXPECT_EQ(expected.webview_identity, actual.webview_identity);
    EXPECT_EQ(expected.embed_identity, actual.embed_identity);
    EXPECT_EQ(expected.content_id, actual.content_id);
    EXPECT_TRUE(actual.embed_connected);
  }

  net::EmbeddedTestServer https_server_{net::EmbeddedTestServer::TYPE_HTTPS};

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
                       RejectsTabHandleFromAnotherWindow) {
  Browser* other_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(WebUIBrowserWindow::FromBrowser(other_browser));

  ASSERT_NO_FATAL_FAILURE(
      ExpectForeignTabHandleRejected(browser(), other_browser));
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       RejectingForeignTabDoesNotCreateGuestHandle) {
  Browser* foreign_browser = CreateBrowserForPopup(browser()->GetProfile());
  ASSERT_TRUE(foreign_browser);
  ASSERT_EQ(nullptr, WebUIBrowserWindow::FromBrowser(foreign_browser));

  TabStripModel* foreign_model = foreign_browser->GetTabStripModel();
  content::WebContents::CreateParams create_params(
      foreign_browser->GetProfile());
  foreign_model->AppendWebContents(content::WebContents::Create(create_params),
                                   /*foreground=*/false);
  ASSERT_EQ(2, foreign_model->count());
  ASSERT_EQ(0, foreign_model->active_index());
  tabs::TabInterface* foreign_tab = foreign_model->GetTabAtIndex(1);
  ASSERT_TRUE(foreign_tab);
  ASSERT_FALSE(foreign_tab->IsActivated());
  content::WebContents* foreign_contents = foreign_tab->GetContents();
  ASSERT_TRUE(foreign_contents);
  ASSERT_EQ(nullptr, guest_contents::GuestContentsHandle::FromWebContents(
                         foreign_contents));
  ASSERT_EQ(TabStripModel::kNoTab,
            browser()->GetTabStripModel()->GetIndexOfTab(foreign_tab));

  ASSERT_NO_FATAL_FAILURE(ExpectTabIdRejected(
      browser(), tabs_api::NodeId::FromTabHandle(foreign_tab->GetHandle()),
      "Tab id is not owned by this browser"));

  EXPECT_EQ(nullptr, guest_contents::GuestContentsHandle::FromWebContents(
                         foreign_contents));
  CloseBrowserSynchronously(foreign_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       RejectsTabHandlesAcrossProfileBoundary) {
  Browser* incognito_browser = CreateIncognitoBrowser(browser()->GetProfile());
  ASSERT_NE(browser()->GetProfile(), incognito_browser->GetProfile());
  ASSERT_TRUE(WebUIBrowserWindow::FromBrowser(incognito_browser));

  ASSERT_NO_FATAL_FAILURE(
      ExpectForeignTabHandleRejected(browser(), incognito_browser));
  ASSERT_NO_FATAL_FAILURE(
      ExpectForeignTabHandleRejected(incognito_browser, browser()));
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       AcceptsInactiveTabHandleFromSameWindow) {
  TabStripModel* model = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/false);
  ASSERT_EQ(2, model->count());
  ASSERT_EQ(0, model->active_index());
  tabs::TabInterface* inactive_tab = model->GetTabAtIndex(1);
  ASSERT_TRUE(inactive_tab);
  ASSERT_EQ(1, model->GetIndexOfTab(inactive_tab));

  ASSERT_NO_FATAL_FAILURE(ExpectTabHandleAccepted(browser(), inactive_tab));
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       InactiveTabSecurityQueryPreservesGuestHandler) {
  const GURL active_url = https_server_.GetURL("a.test", "/page-a");
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), active_url, WindowOpenDisposition::CURRENT_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* model = browser()->GetTabStripModel();
  tabs::TabInterface* active_tab = model->GetActiveTab();
  ASSERT_TRUE(active_tab);
  std::optional<webui_browser::mojom::SecurityIcon> active_security_icon;
  ASSERT_NO_FATAL_FAILURE(WithGuestHandler(
      browser(), active_tab, [&](auto& page_handler, auto& guest_handler) {
        guest_handler->GetSecurityIcon(base::BindLambdaForTesting(
            [&active_security_icon](
                webui_browser::mojom::SecurityIcon security_icon) {
              active_security_icon = security_icon;
            }));
        guest_handler.FlushForTesting();

        ASSERT_TRUE(active_security_icon.has_value());
        EXPECT_EQ(
            webui_browser::mojom::SecurityIcon::SecurePageInfoChromeRefresh,
            *active_security_icon);
        EXPECT_TRUE(page_handler.is_connected());
        EXPECT_TRUE(guest_handler.is_connected());
      }));
  ASSERT_TRUE(active_security_icon.has_value());

  chrome::AddTabAt(browser(), GURL("about:blank"), -1,
                   /*foreground=*/false);
  ASSERT_EQ(2, model->count());
  ASSERT_EQ(0, model->active_index());
  tabs::TabInterface* inactive_tab = model->GetTabAtIndex(1);
  ASSERT_TRUE(inactive_tab);
  ASSERT_FALSE(inactive_tab->IsActivated());
  content::WebContents* inactive_contents = inactive_tab->GetContents();
  ASSERT_TRUE(inactive_contents);

  ASSERT_NO_FATAL_FAILURE(WithGuestHandler(
      browser(), inactive_tab, [&](auto& page_handler, auto& guest_handler) {
        mojo::test::BadMessageObserver bad_message_observer;
        std::optional<webui_browser::mojom::SecurityIcon>
            inactive_security_icon;
        guest_handler->GetSecurityIcon(base::BindLambdaForTesting(
            [&inactive_security_icon](
                webui_browser::mojom::SecurityIcon security_icon) {
              inactive_security_icon = security_icon;
            }));
        guest_handler.FlushForTesting();

        ASSERT_TRUE(inactive_security_icon.has_value());
        EXPECT_EQ(
            webui_browser::mojom::SecurityIcon::NotSecureWarningChromeRefresh,
            *inactive_security_icon);
        EXPECT_NE(*active_security_icon, *inactive_security_icon);
        EXPECT_FALSE(bad_message_observer.got_bad_message());
        EXPECT_TRUE(page_handler.is_connected());
        EXPECT_TRUE(guest_handler.is_connected());

        ASSERT_TRUE(chrome::ExecuteCommand(browser(), IDC_SELECT_TAB_1));
        ASSERT_TRUE(base::test::RunUntil([model, inactive_tab]() {
          return model->GetActiveTab() == inactive_tab &&
                 inactive_tab->IsActivated();
        }));

        const GURL navigation_url =
            https_server_.GetURL("a.test", "/page-a-next");
        content::TestNavigationObserver navigation(navigation_url);
        navigation.WatchWebContents(inactive_contents);
        guest_handler->Navigate(navigation_url);
        navigation.Wait();

        EXPECT_TRUE(navigation.last_navigation_succeeded());
        EXPECT_EQ(navigation_url, navigation.last_navigation_url());
        EXPECT_EQ(navigation_url, inactive_contents->GetLastCommittedURL());
        EXPECT_FALSE(bad_message_observer.got_bad_message());
        EXPECT_TRUE(page_handler.is_connected());
        EXPECT_TRUE(guest_handler.is_connected());
      }));
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest, RejectsClosedTabHandle) {
  TabStripModel* model = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/false);
  ASSERT_EQ(2, model->count());
  tabs::TabInterface* closing_tab = model->GetTabAtIndex(1);
  ASSERT_TRUE(closing_tab);
  const tabs::TabHandle stale_handle = closing_tab->GetHandle();
  const tabs_api::NodeId stale_id =
      tabs_api::NodeId::FromTabHandle(stale_handle);

  model->CloseWebContentsAt(1, TabCloseTypes::CLOSE_NONE);
  ASSERT_EQ(1, model->count());
  ASSERT_TRUE(base::test::RunUntil(
      [&stale_handle]() { return stale_handle.Get() == nullptr; }));

  ASSERT_NO_FATAL_FAILURE(
      ExpectTabIdRejected(browser(), stale_id, "Invalid tab id"));
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       RevokesGuestHandlerWhenTabMovesToAnotherWindow) {
  const GURL initial_url = https_server_.GetURL("a.test", "/page-a");
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), initial_url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* source_model = browser()->GetTabStripModel();
  ASSERT_EQ(2, source_model->count());
  const int moving_index = source_model->active_index();
  tabs::TabInterface* moving_tab = source_model->GetTabAtIndex(moving_index);
  ASSERT_TRUE(moving_tab);
  const tabs::TabHandle moving_handle = moving_tab->GetHandle();
  content::WebContents* moving_contents = moving_tab->GetContents();
  ASSERT_TRUE(moving_contents);
  ASSERT_EQ(initial_url, moving_contents->GetLastCommittedURL());

  Browser* destination = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination);
  ASSERT_TRUE(WebUIBrowserWindow::FromBrowser(destination));
  TabStripModel* destination_model = destination->GetTabStripModel();
  ASSERT_TRUE(destination_model);

  ASSERT_NO_FATAL_FAILURE(WithGuestHandler(
      browser(), moving_tab, [&](auto& page_handler, auto& guest_handler) {
        bool guest_disconnected = false;
        guest_handler.set_disconnect_handler(base::BindLambdaForTesting(
            [&guest_disconnected]() { guest_disconnected = true; }));
        mojo::test::BadMessageObserver bad_message_observer;

        const GURL stale_url = https_server_.GetURL("b.test", "/page-b");
        TargetNavigationStartObserver stale_navigation_start(moving_contents,
                                                             stale_url);
        chrome::MoveTabsToExistingWindow(browser(), destination,
                                         {moving_index});
        guest_handler->Navigate(stale_url);

        base::test::ScopedRunLoopTimeout disconnect_timeout(FROM_HERE,
                                                            base::Seconds(5));
        ASSERT_TRUE(base::test::RunUntil(
            [&guest_disconnected]() { return guest_disconnected; }));
        base::RunLoop().RunUntilIdle();

        EXPECT_FALSE(bad_message_observer.got_bad_message());
        EXPECT_TRUE(page_handler.is_connected());
        EXPECT_FALSE(guest_handler.is_connected());
        EXPECT_EQ(TabStripModel::kNoTab,
                  source_model->GetIndexOfTab(moving_tab));
        ASSERT_NE(TabStripModel::kNoTab,
                  destination_model->GetIndexOfTab(moving_tab));
        EXPECT_EQ(moving_tab, moving_handle.Get());
        EXPECT_EQ(moving_contents, moving_tab->GetContents());
        EXPECT_FALSE(stale_navigation_start.target_started());
        EXPECT_EQ(nullptr, moving_contents->GetController().GetPendingEntry());
        EXPECT_EQ(initial_url, moving_contents->GetVisibleURL());
        EXPECT_EQ(initial_url, moving_contents->GetLastCommittedURL());

        const GURL destination_url =
            https_server_.GetURL("a.test", "/page-a-next");
        ASSERT_NO_FATAL_FAILURE(WithGuestHandler(
            destination, moving_tab,
            [&](auto& destination_page_handler,
                auto& destination_guest_handler) {
              content::TestNavigationObserver navigation(destination_url);
              navigation.WatchWebContents(moving_contents);
              destination_guest_handler->Navigate(destination_url);
              navigation.Wait();

              EXPECT_TRUE(navigation.last_navigation_succeeded());
              EXPECT_EQ(destination_url, navigation.last_navigation_url());
              EXPECT_EQ(destination_url,
                        moving_contents->GetLastCommittedURL());
              EXPECT_TRUE(destination_page_handler.is_connected());
              EXPECT_TRUE(destination_guest_handler.is_connected());
            }));
      }));
}

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       RevokesGuestHandlerWhenTabCloses) {
  const GURL closing_url = https_server_.GetURL("a.test", "/page-a");
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), closing_url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* model = browser()->GetTabStripModel();
  ASSERT_EQ(2, model->count());
  tabs::TabInterface* closing_tab = model->GetActiveTab();
  ASSERT_TRUE(closing_tab);
  const tabs::TabHandle closing_handle = closing_tab->GetHandle();

  ASSERT_NO_FATAL_FAILURE(WithGuestHandler(
      browser(), closing_tab, [&](auto& page_handler, auto& guest_handler) {
        bool guest_disconnected = false;
        guest_handler.set_disconnect_handler(base::BindLambdaForTesting(
            [&guest_disconnected]() { guest_disconnected = true; }));
        mojo::test::BadMessageObserver bad_message_observer;

        chrome::CloseTab(browser());

        base::test::ScopedRunLoopTimeout disconnect_timeout(FROM_HERE,
                                                            base::Seconds(5));
        ASSERT_TRUE(base::test::RunUntil(
            [&guest_disconnected]() { return guest_disconnected; }));
        base::RunLoop().RunUntilIdle();

        EXPECT_FALSE(bad_message_observer.got_bad_message());
        EXPECT_TRUE(page_handler.is_connected());
        EXPECT_FALSE(guest_handler.is_connected());
        EXPECT_EQ(1, model->count());
        EXPECT_EQ(nullptr, closing_handle.Get());
      }));
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

IN_PROC_BROWSER_TEST_F(LimitlessWebUIBrowserTest,
                       GateTwoUpstreamShellSeamPreservesEmbeddedTabState) {
  WebUIBrowserWindow* window = WebUIBrowserWindow::FromBrowser(browser());
  ASSERT_TRUE(window);

  content::WebContents* shell_contents = GetShellContents();
  ASSERT_TRUE(shell_contents);
  ASSERT_TRUE(content::WaitForLoadStop(shell_contents));
  content::WebContentsConsoleObserver assertion_observer(shell_contents);
  assertion_observer.SetPattern("*Assertion failed*");

  base::test::ScopedRunLoopTimeout gate_timeout(FROM_HERE, base::Seconds(30));
  ASSERT_TRUE(base::test::RunUntil([this]() {
    return ShellStateMatches(/*tab_count=*/1, /*active_index=*/0,
                             browser()
                                 ->tab_strip_model()
                                 ->GetActiveWebContents()
                                 ->GetLastCommittedURL());
  })) << "Live shell did not expose the initial authoritative tab state";

  TabStripModel* model = browser()->tab_strip_model();
  content::WebContents* page_a_contents = model->GetActiveWebContents();
  ASSERT_TRUE(page_a_contents);
  const GURL page_a_url = https_server_.GetURL("a.test", "/page-a");
  ASSERT_NO_FATAL_FAILURE(
      NavigateThroughVisibleAddress(page_a_contents, page_a_url));
  ASSERT_TRUE(content::EvalJs(page_a_contents,
                              "document.querySelector('#page-a') !== null")
                  .ExtractBool());
  EXPECT_EQ(1,
            content::EvalJs(page_a_contents, "window.limitlessPageState.value")
                .ExtractInt());

  const GURL page_a_next_url = https_server_.GetURL("a.test", "/page-a-next");
  ASSERT_NO_FATAL_FAILURE(
      NavigateThroughVisibleAddress(page_a_contents, page_a_next_url));
  ASSERT_TRUE(content::EvalJs(page_a_contents,
                              "document.querySelector('#page-a-next') !== null")
                  .ExtractBool());

  ASSERT_NO_FATAL_FAILURE(
      NavigateThroughBrowserCommand(page_a_contents, IDC_BACK, page_a_url));
  ASSERT_TRUE(content::EvalJs(page_a_contents,
                              "document.querySelector('#page-a') !== null")
                  .ExtractBool());
  ASSERT_TRUE(base::test::RunUntil([this, &page_a_url]() {
    return ShellStateMatches(/*tab_count=*/1, /*active_index=*/0, page_a_url);
  })) << "The back command did not project page A into the shell";

  ASSERT_NO_FATAL_FAILURE(NavigateThroughBrowserCommand(
      page_a_contents, IDC_FORWARD, page_a_next_url));
  ASSERT_TRUE(content::EvalJs(page_a_contents,
                              "document.querySelector('#page-a-next') !== null")
                  .ExtractBool());
  ASSERT_TRUE(base::test::RunUntil([this, &page_a_next_url]() {
    return ShellStateMatches(/*tab_count=*/1, /*active_index=*/0,
                             page_a_next_url);
  })) << "The forward command did not project page A Next into the shell";

  ASSERT_NO_FATAL_FAILURE(
      NavigateThroughBrowserCommand(page_a_contents, IDC_BACK, page_a_url));
  ASSERT_TRUE(content::EvalJs(page_a_contents,
                              "document.querySelector('#page-a') !== null")
                  .ExtractBool());
  ASSERT_TRUE(base::test::RunUntil([this, &page_a_url]() {
    return ShellStateMatches(/*tab_count=*/1, /*active_index=*/0, page_a_url);
  })) << "The final back command did not restore page A before reload";

  ASSERT_NO_FATAL_FAILURE(
      NavigateThroughBrowserCommand(page_a_contents, IDC_RELOAD, page_a_url));
  EXPECT_EQ(2,
            content::EvalJs(page_a_contents, "window.limitlessPageState.value")
                .ExtractInt());
  ASSERT_TRUE(
      content::ExecJs(page_a_contents, "window.limitlessPageState.value = 42"));

  std::optional<ShellContentIdentity> page_a_identity;
  ASSERT_TRUE(base::test::RunUntil([this, page_a_contents, &page_a_identity]() {
    page_a_identity = GetShellContentIdentity(/*index=*/0);
    return page_a_contents->GetSurfaceEmbedConnector() != nullptr &&
           page_a_identity.has_value() && page_a_identity->active &&
           page_a_identity->embed_connected &&
           !page_a_identity->content_id.empty() &&
           page_a_identity->visibility == "visible";
  })) << "Page A did not expose an active persistent SurfaceEmbed element";
  auto* const page_a_connector = page_a_contents->GetSurfaceEmbedConnector();

  ASSERT_TRUE(chrome::ExecuteCommand(browser(), IDC_NEW_TAB));
  ASSERT_TRUE(base::test::RunUntil([this, model]() {
    content::WebContents* active = model->GetActiveWebContents();
    return model->count() == 2 && model->active_index() == 1 && active &&
           ShellStateMatches(/*tab_count=*/2, /*active_index=*/1,
                             active->GetLastCommittedURL());
  })) << "The browser new-tab command did not project into the live shell";
  content::WebContents* page_b_contents = model->GetActiveWebContents();
  ASSERT_TRUE(page_b_contents);
  EXPECT_NE(page_a_contents, page_b_contents);
  const GURL page_b_url = https_server_.GetURL("b.test", "/page-b");
  ASSERT_NO_FATAL_FAILURE(
      NavigateThroughVisibleAddress(page_b_contents, page_b_url));
  ASSERT_TRUE(content::EvalJs(page_b_contents,
                              "document.querySelector('#page-b') !== null")
                  .ExtractBool());

  std::optional<ShellContentIdentity> page_a_inactive_identity;
  std::optional<ShellContentIdentity> page_b_identity;
  ASSERT_TRUE(base::test::RunUntil([this, model, page_a_contents,
                                    page_b_contents, &page_a_inactive_identity,
                                    &page_b_identity, &page_b_url]() {
    page_a_inactive_identity = GetShellContentIdentity(/*index=*/0);
    page_b_identity = GetShellContentIdentity(/*index=*/1);
    return model->active_index() == 1 &&
           model->GetActiveWebContents() == page_b_contents &&
           page_a_contents->GetSurfaceEmbedConnector() != nullptr &&
           page_b_contents->GetSurfaceEmbedConnector() != nullptr &&
           page_a_inactive_identity.has_value() &&
           page_b_identity.has_value() && !page_a_inactive_identity->active &&
           page_a_inactive_identity->embed_connected &&
           page_a_inactive_identity->visibility == "hidden" &&
           page_b_identity->active && page_b_identity->embed_connected &&
           !page_b_identity->content_id.empty() &&
           page_b_identity->visibility == "visible" &&
           ShellStateMatches(/*tab_count=*/2, /*active_index=*/1, page_b_url);
  })) << "Page B did not become authoritative while page A's embed stayed "
         "connected";
  auto* const page_b_connector = page_b_contents->GetSurfaceEmbedConnector();
  EXPECT_EQ(page_a_connector, page_a_contents->GetSurfaceEmbedConnector());
  ExpectSameShellContent(*page_a_identity, *page_a_inactive_identity);

  ASSERT_TRUE(chrome::ExecuteCommand(browser(), IDC_SELECT_TAB_0));
  std::optional<ShellContentIdentity> page_a_reactivated_identity;
  std::optional<ShellContentIdentity> page_b_inactive_identity;
  ASSERT_TRUE(base::test::RunUntil([this, model, page_a_contents,
                                    page_b_contents, page_a_connector,
                                    page_b_connector,
                                    &page_a_reactivated_identity,
                                    &page_b_inactive_identity, &page_a_url]() {
    page_a_reactivated_identity = GetShellContentIdentity(/*index=*/0);
    page_b_inactive_identity = GetShellContentIdentity(/*index=*/1);
    return model->active_index() == 0 &&
           model->GetActiveWebContents() == page_a_contents &&
           page_a_contents->GetSurfaceEmbedConnector() == page_a_connector &&
           page_b_contents->GetSurfaceEmbedConnector() == page_b_connector &&
           page_a_reactivated_identity.has_value() &&
           page_b_inactive_identity.has_value() &&
           page_a_reactivated_identity->active &&
           page_a_reactivated_identity->visibility == "visible" &&
           !page_b_inactive_identity->active &&
           page_b_inactive_identity->embed_connected &&
           page_b_inactive_identity->visibility == "hidden" &&
           ShellStateMatches(/*tab_count=*/2, /*active_index=*/0, page_a_url);
  })) << "The browser select-tab command did not reactivate page A with both "
         "embeds intact";
  ExpectSameShellContent(*page_a_identity, *page_a_reactivated_identity);
  ExpectSameShellContent(*page_b_identity, *page_b_inactive_identity);
  EXPECT_EQ(42,
            content::EvalJs(page_a_contents, "window.limitlessPageState.value")
                .ExtractInt());

  ASSERT_TRUE(chrome::ExecuteCommand(browser(), IDC_SELECT_TAB_1));
  std::optional<ShellContentIdentity> page_a_inactive_again_identity;
  std::optional<ShellContentIdentity> page_b_reactivated_identity;
  ASSERT_TRUE(base::test::RunUntil([this, model, page_a_contents,
                                    page_b_contents, page_a_connector,
                                    page_b_connector,
                                    &page_a_inactive_again_identity,
                                    &page_b_reactivated_identity,
                                    &page_b_url]() {
    page_a_inactive_again_identity = GetShellContentIdentity(/*index=*/0);
    page_b_reactivated_identity = GetShellContentIdentity(/*index=*/1);
    return model->active_index() == 1 &&
           model->GetActiveWebContents() == page_b_contents &&
           page_a_contents->GetSurfaceEmbedConnector() == page_a_connector &&
           page_b_contents->GetSurfaceEmbedConnector() == page_b_connector &&
           page_a_inactive_again_identity.has_value() &&
           page_b_reactivated_identity.has_value() &&
           !page_a_inactive_again_identity->active &&
           page_a_inactive_again_identity->embed_connected &&
           page_a_inactive_again_identity->visibility == "hidden" &&
           page_b_reactivated_identity->active &&
           page_b_reactivated_identity->visibility == "visible" &&
           ShellStateMatches(/*tab_count=*/2, /*active_index=*/1, page_b_url);
  })) << "The browser select-tab command did not reactivate page B with both "
         "embeds intact";
  ExpectSameShellContent(*page_a_identity, *page_a_inactive_again_identity);
  ExpectSameShellContent(*page_b_identity, *page_b_reactivated_identity);

  ASSERT_TRUE(chrome::ExecuteCommand(browser(), IDC_CLOSE_TAB));
  std::optional<ShellContentIdentity> page_a_final_identity;
  ASSERT_TRUE(base::test::RunUntil([this, model, page_a_contents,
                                    page_a_connector, &page_a_final_identity,
                                    &page_a_url]() {
    page_a_final_identity = GetShellContentIdentity(/*index=*/0);
    return model->count() == 1 && model->active_index() == 0 &&
           model->GetActiveWebContents() == page_a_contents &&
           page_a_contents->GetSurfaceEmbedConnector() == page_a_connector &&
           page_a_final_identity.has_value() && page_a_final_identity->active &&
           page_a_final_identity->embed_connected &&
           page_a_final_identity->visibility == "visible" &&
           ShellStateMatches(/*tab_count=*/1, /*active_index=*/0, page_a_url);
  })) << "The browser close-tab command did not restore page A's original "
         "embed";
  ExpectSameShellContent(*page_a_identity, *page_a_final_identity);

  EXPECT_EQ(page_a_url, page_a_contents->GetLastCommittedURL());
  EXPECT_EQ(42,
            content::EvalJs(page_a_contents, "window.limitlessPageState.value")
                .ExtractInt());
  if (!assertion_observer.messages().empty()) {
    ADD_FAILURE() << "The upstream shell emitted a console assertion during "
                     "the seam integration journey: "
                  << assertion_observer.GetMessageAt(/*index=*/0);
  }
}

}  // namespace
