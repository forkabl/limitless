// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/apple/owned_objc.h"
#include "base/callback_list.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/function_ref.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/sessions/session_service_test_helper.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/tab_list/tab_list_interface_observer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/sad_tab_helper.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/omnibox/omnibox_view_views.h"
#include "chrome/browser/ui/views/sad_tab_view.h"
#include "chrome/browser/ui/webui_browser/webui_browser_window.h"
#include "chrome/common/chrome_features.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/interactive_test_utils.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/javascript_dialogs/app_modal_dialog_controller.h"
#include "components/javascript_dialogs/app_modal_dialog_queue.h"
#include "components/javascript_dialogs/app_modal_dialog_view.h"
#include "components/javascript_dialogs/tab_modal_dialog_manager.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/session_id.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_interface.h"
#include "components/web_modal/web_contents_modal_dialog_host.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/web_modal/web_contents_modal_dialog_manager_delegate.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/result_codes.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "limitless_split_pane_controller.h"
#include "limitless_swiftui_rail_host.h"
#include "limitless_workspace_controller.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "shell/agents/herdr_agent_provider.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/test_accelerator_target.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/test/cocoa_test_event_utils.h"
#include "ui/events/test/event_generator.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view_utils.h"

namespace {

std::unique_ptr<net::test_server::HttpResponse> HandleTabRailRequest(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/html");
  if (request.relative_url == "/one") {
    response->set_content("<title>Rail One</title><h1>One</h1>");
  } else if (request.relative_url == "/two") {
    response->set_content("<title>Rail Two</title><h1>Two</h1>");
  } else if (request.relative_url == "/three") {
    response->set_content("<title>Rail Three</title><h1>Three</h1>");
  } else if (request.relative_url == "/four") {
    response->set_content("<title>Rail Four</title><h1>Four</h1>");
  } else if (request.relative_url == "/five") {
    response->set_content("<title>Rail Five</title><h1>Five</h1>");
  } else if (request.relative_url == "/six") {
    response->set_content("<title>Rail Six</title><h1>Six</h1>");
  } else if (request.relative_url == "/keys") {
    response->set_content(
        "<title>Leader Key Evidence</title>"
        "<input id='key-input' aria-label='Keyboard evidence'>");
  } else {
    response->set_code(net::HTTP_NOT_FOUND);
  }
  return response;
}

views::View* GetSwiftUITabRail(Browser* target_browser) {
  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(target_browser);
  return browser_view ? browser_view->limitless_swiftui_rail_for_testing()
                      : nullptr;
}

content::RenderWidgetHost* GetActiveRenderWidgetHost(Browser* target_browser) {
  content::WebContents* contents =
      target_browser->GetTabStripModel()->GetActiveWebContents();
  CHECK(contents);
  content::RenderFrameHost* focused_frame = contents->GetFocusedFrame();
  if (!focused_frame) {
    focused_frame = contents->GetPrimaryMainFrame();
  }
  CHECK(focused_frame);
  content::RenderWidgetHost* render_widget_host =
      focused_frame->GetRenderWidgetHost();
  CHECK(render_widget_host);
  return render_widget_host;
}

void ForwardRendererKeyEvent(int32_t process_id,
                             int32_t routing_id,
                             const ui::KeyEvent& key_event) {
  input::NativeWebKeyboardEvent web_event(key_event);
  CHECK(!web_event.skip_if_unhandled);
  if (content::RenderWidgetHost* target =
          content::RenderWidgetHost::FromID(process_id, routing_id)) {
    target->ForwardKeyboardEvent(web_event);
  }
}

void ForwardRendererNativeKeyEvent(int32_t process_id,
                                   int32_t routing_id,
                                   NSEvent* native_event) {
  CHECK(native_event);
  input::NativeWebKeyboardEvent web_event{
      base::apple::OwnedNSEvent(native_event)};
  CHECK(!web_event.skip_if_unhandled);
  if (content::RenderWidgetHost* target =
          content::RenderWidgetHost::FromID(process_id, routing_id)) {
    target->ForwardKeyboardEvent(web_event);
  }
}

NSUInteger MacModifierFlagsFromEventFlags(int flags) {
  NSUInteger mac_flags = 0;
  if (flags & ui::EF_SHIFT_DOWN) {
    mac_flags |= NSEventModifierFlagShift;
  }
  if (flags & ui::EF_CONTROL_DOWN) {
    mac_flags |= NSEventModifierFlagControl;
  }
  if (flags & ui::EF_ALT_DOWN) {
    mac_flags |= NSEventModifierFlagOption;
  }
  if (flags & ui::EF_COMMAND_DOWN) {
    mac_flags |= NSEventModifierFlagCommand;
  }
  return mac_flags;
}

// Exercises the real renderer-originating route through RenderWidgetHost,
// WebContentsDelegate, and BrowserView. A printable character event is sent
// when requested so a handled RawKeyDown must suppress it before the page.
void SendRendererKey(Browser* target_browser,
                     ui::KeyboardCode key_code,
                     int flags = ui::EF_NONE,
                     std::optional<char16_t> character = std::nullopt,
                     bool attach_native_event = false,
                     bool send_key_up = true) {
  content::RenderWidgetHost* target = GetActiveRenderWidgetHost(target_browser);
  const int32_t process_id = target->GetProcess()->GetID().GetUnsafeValue();
  const int32_t routing_id = target->GetRoutingID();
  const ui::DomCode dom_code = ui::UsLayoutKeyboardCodeToDomCode(key_code);
  if (attach_native_event) {
    NSWindow* native_window =
        target_browser->GetWindow()->GetNativeWindow().GetNativeNSWindow();
    CHECK(native_window);
    ForwardRendererNativeKeyEvent(process_id, routing_id,
                                  cocoa_test_event_utils::SynthesizeKeyEvent(
                                      native_window, true, key_code,
                                      MacModifierFlagsFromEventFlags(flags)));
  } else {
    ForwardRendererKeyEvent(
        process_id, routing_id,
        ui::KeyEvent(ui::EventType::kKeyPressed, key_code, dom_code, flags));
  }
  if (character) {
    ForwardRendererKeyEvent(
        process_id, routing_id,
        ui::KeyEvent::FromCharacter(*character, key_code, dom_code, flags));
  }
  if (!send_key_up) {
    return;
  }
  if (attach_native_event) {
    NSWindow* native_window =
        target_browser->GetWindow()->GetNativeWindow().GetNativeNSWindow();
    CHECK(native_window);
    ForwardRendererNativeKeyEvent(process_id, routing_id,
                                  cocoa_test_event_utils::SynthesizeKeyEvent(
                                      native_window, false, key_code,
                                      MacModifierFlagsFromEventFlags(flags)));
  } else {
    ForwardRendererKeyEvent(
        process_id, routing_id,
        ui::KeyEvent(ui::EventType::kKeyReleased, key_code, dom_code, flags));
  }
}

void SendRendererNativeKeyUp(Browser* target_browser,
                             ui::KeyboardCode key_code,
                             int flags) {
  content::RenderWidgetHost* target = GetActiveRenderWidgetHost(target_browser);
  const int32_t process_id = target->GetProcess()->GetID().GetUnsafeValue();
  const int32_t routing_id = target->GetRoutingID();
  NSWindow* native_window =
      target_browser->GetWindow()->GetNativeWindow().GetNativeNSWindow();
  CHECK(native_window);
  ForwardRendererNativeKeyEvent(process_id, routing_id,
                                cocoa_test_event_utils::SynthesizeKeyEvent(
                                    native_window, false, key_code,
                                    MacModifierFlagsFromEventFlags(flags)));
}

// This is a deterministic noninteractive Views-control route. It deliberately
// makes no native AppKit input or macOS first-responder claim.
bool SendViewsControlKey(Browser* target_browser,
                         views::View* focused_view,
                         ui::KeyboardCode key_code,
                         int flags = ui::EF_NONE) {
  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(target_browser);
  CHECK(browser_view);
  CHECK(focused_view);
  focused_view->RequestFocus();
  CHECK_EQ(focused_view,
           browser_view->GetWidget()->GetFocusManager()->GetFocusedView());
  ui::KeyEvent key_event(ui::EventType::kKeyPressed, key_code, flags);
  browser_view->GetWidget()->OnKeyEvent(&key_event);
  return key_event.handled();
}

class ScopedAcceleratorRegistration {
 public:
  ScopedAcceleratorRegistration(
      views::FocusManager* focus_manager,
      const ui::Accelerator& accelerator,
      ui::AcceleratorManager::HandlerPriority priority,
      ui::AcceleratorTarget* target)
      : focus_manager_(focus_manager),
        accelerator_(accelerator),
        target_(target) {
    CHECK(focus_manager_);
    CHECK(target_);
    focus_manager_->RegisterAccelerator(accelerator_, priority, target_);
  }

  ScopedAcceleratorRegistration(const ScopedAcceleratorRegistration&) = delete;
  ScopedAcceleratorRegistration& operator=(
      const ScopedAcceleratorRegistration&) = delete;

  ~ScopedAcceleratorRegistration() {
    focus_manager_->UnregisterAccelerator(accelerator_, target_);
  }

 private:
  raw_ptr<views::FocusManager> focus_manager_;
  ui::Accelerator accelerator_;
  raw_ptr<ui::AcceleratorTarget> target_;
};

bool ResetKeyEvidence(content::WebContents* contents, bool focus_input) {
  const std::string script = std::string(R"js(
    if (!window.keyEvidenceInstalled) {
      window.keyEvidenceInstalled = true;
      document.addEventListener('keydown', event => {
        window.keyEvents.push({
          keyCode: event.keyCode,
          ctrl: event.ctrlKey,
          shift: event.shiftKey,
          alt: event.altKey,
          meta: event.metaKey
        });
      }, true);
    }
    window.keyEvents = [];
    const input = document.querySelector('#key-input');
    input.value = '';
  )js") + (focus_input ? "input.focus();"
                       : "input.blur(); document.body.tabIndex = -1; "
                         "document.body.focus();");
  if (!content::ExecJs(contents, script)) {
    return false;
  }
  return base::test::RunUntil(
      [&]() { return contents->IsFocusedElementEditable() == focus_input; });
}

bool FocusKeyEvidenceInput(content::WebContents* contents) {
  return content::ExecJs(contents,
                         "document.querySelector('#key-input').focus()") &&
         base::test::RunUntil(
             [&]() { return contents->IsFocusedElementEditable(); });
}

int KeyEventCount(content::WebContents* contents,
                  ui::KeyboardCode key_code,
                  int flags) {
  const auto js_boolean = [](bool value) { return value ? "true" : "false"; };
  return content::EvalJs(
             contents,
             "window.keyEvents.filter(event => event.keyCode === " +
                 base::NumberToString(static_cast<int>(key_code)) +
                 " && event.ctrl === " +
                 js_boolean(flags & ui::EF_CONTROL_DOWN) +
                 " && event.shift === " +
                 js_boolean(flags & ui::EF_SHIFT_DOWN) + " && event.alt === " +
                 js_boolean(flags & ui::EF_ALT_DOWN) + " && event.meta === " +
                 js_boolean(flags & ui::EF_COMMAND_DOWN) + ").length")
      .ExtractInt();
}

std::string KeyInputValue(content::WebContents* contents) {
  return content::EvalJs(contents, "document.querySelector('#key-input').value")
      .ExtractString();
}

std::optional<limitless::SwiftUITabRailSnapshotForTesting> GetLeaderSnapshot(
    views::View* rail) {
  return limitless::GetSwiftUITabRailSnapshotForTesting(rail);
}

bool IsLeaderArmed(views::View* rail) {
  const auto snapshot = GetLeaderSnapshot(rail);
  return snapshot && snapshot->leader_armed;
}

bool WaitForCondition(base::FunctionRef<bool()> condition,
                      base::TimeDelta timeout = base::Seconds(5)) {
  if (condition()) {
    return true;
  }
  base::RunLoop run_loop;
  base::RepeatingTimer poll_timer;
  poll_timer.Start(FROM_HERE, base::Milliseconds(5),
                   base::BindLambdaForTesting([&]() {
                     if (condition()) {
                       run_loop.Quit();
                     }
                   }));
  base::OneShotTimer timeout_timer;
  timeout_timer.Start(FROM_HERE, timeout, run_loop.QuitClosure());
  run_loop.Run();
  return condition();
}

std::optional<limitless::SwiftUITabRailSnapshotForTesting> WaitForRailTabs(
    views::View* rail,
    size_t expected_count) {
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> snapshot;
  if (!base::test::RunUntil([&]() {
        snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
        return snapshot && snapshot->tab_identifiers.size() == expected_count;
      })) {
    return std::nullopt;
  }
  return snapshot;
}

tabs::TabInterface* FindTabWithPath(Browser* target_browser,
                                    std::string_view path) {
  for (tabs::TabInterface* tab :
       TabListInterface::From(target_browser)->GetAllTabs()) {
    if (tab->GetContents()->GetVisibleURL().path() == path) {
      return tab;
    }
  }
  return nullptr;
}

Browser* FindBrowserWithWorkspaceWindowIdentifier(
    const std::string& window_identifier) {
  Browser* match = nullptr;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser_window) {
        Browser* candidate = browser_window->GetBrowserForMigrationOnly();
        views::View* rail = candidate ? GetSwiftUITabRail(candidate) : nullptr;
        const auto snapshot =
            rail ? limitless::GetSwiftUITabRailSnapshotForTesting(rail)
                 : std::nullopt;
        if (snapshot && snapshot->window_identifier == window_identifier) {
          match = candidate;
        }
        return !match;
      });
  return match;
}

std::string BuildWorkspaceTabSessionData(
    const std::string& tab_identifier,
    const std::string& workspace_identifier) {
  base::DictValue data;
  data.Set("version", 1);
  data.Set("tab_id", tab_identifier);
  data.Set("workspace_id", workspace_identifier);
  std::string serialized;
  CHECK(base::JSONWriter::Write(data, &serialized));
  return serialized;
}

std::pair<std::string, std::string> ReadWorkspaceTabSessionData(
    const std::string& serialized) {
  std::optional<base::Value> value =
      base::JSONReader::Read(serialized, base::JSON_PARSE_RFC);
  CHECK(value && value->is_dict());
  const std::string* tab_identifier = value->GetDict().FindString("tab_id");
  const std::string* workspace_identifier =
      value->GetDict().FindString("workspace_id");
  CHECK(tab_identifier);
  CHECK(workspace_identifier);
  return {*tab_identifier, *workspace_identifier};
}

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

std::string ProductIdentifierForTab(Browser* target_browser,
                                    tabs::TabInterface* tab) {
  CHECK(target_browser);
  CHECK(tab);
  limitless::WorkspaceController* controller =
      limitless::WorkspaceController::From(target_browser);
  CHECK(controller);
  const std::optional<std::string> serialized =
      controller->SerializeTabForSession(tab->GetHandle());
  CHECK(serialized);
  return ReadWorkspaceTabSessionData(*serialized).first;
}

bool CreateNativeSplitThroughRail(views::View* rail,
                                  Browser* target_browser,
                                  tabs::TabInterface* first,
                                  tabs::TabInterface* second) {
  CHECK(rail);
  CHECK(target_browser);
  CHECK(first);
  CHECK(second);
  const std::string first_identifier =
      ProductIdentifierForTab(target_browser, first);
  const std::string second_identifier =
      ProductIdentifierForTab(target_browser, second);
  const std::string menu_identifier = "limitless-tab-split-" + first_identifier;
  const auto action = limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
      rail, menu_identifier, menu_identifier + "-with-" + second_identifier);
  return action && action->press_invoked && WaitForCondition([&]() {
           if (!first->GetSplit().has_value() ||
               first->GetSplit() != second->GetSplit()) {
             return false;
           }
           const auto snapshot =
               limitless::GetSwiftUITabRailSnapshotForTesting(rail);
           if (!snapshot) {
             return false;
           }
           for (size_t index = 0;
                index < snapshot->split_first_tab_identifiers.size() &&
                index < snapshot->split_second_tab_identifiers.size();
                ++index) {
             const std::string& projected_first =
                 snapshot->split_first_tab_identifiers[index];
             const std::string& projected_second =
                 snapshot->split_second_tab_identifiers[index];
             if ((projected_first == first_identifier &&
                  projected_second == second_identifier) ||
                 (projected_first == second_identifier &&
                  projected_second == first_identifier)) {
               return true;
             }
           }
           return false;
         });
}

bool RailProjectsExactSplit(views::View* rail,
                            const std::string& first_identifier,
                            const std::string& second_identifier,
                            const std::string& active_identifier,
                            const std::string& layout,
                            double ratio) {
  return WaitForCondition([&]() {
    const auto snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->split_pair_identifiers.size() == 1 &&
           snapshot->split_first_tab_identifiers ==
               std::vector<std::string>{first_identifier} &&
           snapshot->split_second_tab_identifiers ==
               std::vector<std::string>{second_identifier} &&
           snapshot->split_active_tab_identifiers ==
               std::vector<std::string>{active_identifier} &&
           snapshot->split_layouts == std::vector<std::string>{layout} &&
           snapshot->split_ratios == std::vector<double>{ratio};
  });
}

SadTabView* FindSadTabViewForContents(BrowserView* browser_view,
                                      content::WebContents* contents) {
  CHECK(browser_view);
  CHECK(contents);
  for (ContentsWebView* contents_view :
       browser_view->GetAllVisibleContentsWebViews()) {
    if (contents_view->web_contents() != contents) {
      continue;
    }
    for (views::View* child : contents_view->children()) {
      if (SadTabView* sad_tab_view = views::AsViewClass<SadTabView>(child)) {
        return sad_tab_view;
      }
    }
  }
  return nullptr;
}

tabs::TabInterface* FindTabWithProductIdentifier(
    Browser* target_browser,
    std::string_view product_identifier) {
  for (tabs::TabInterface* tab :
       TabListInterface::From(target_browser)->GetAllTabs()) {
    if (ProductIdentifierForTab(target_browser, tab) == product_identifier) {
      return tab;
    }
  }
  return nullptr;
}

std::optional<limitless::SwiftUIAgentRailSnapshotForTesting> WaitForRailAgents(
    views::View* rail,
    size_t expected_count) {
  std::optional<limitless::SwiftUIAgentRailSnapshotForTesting> snapshot;
  if (!base::test::RunUntil([&]() {
        snapshot = limitless::GetSwiftUIAgentRailSnapshotForTesting(rail);
        return snapshot && snapshot->source_state == "ready" &&
               snapshot->pane_identifiers.size() == expected_count;
      })) {
    return std::nullopt;
  }
  return snapshot;
}

std::optional<limitless::SwiftUIAgentRailSnapshotForTesting>
WaitForRailAgentState(views::View* rail,
                      std::string_view expected_state,
                      size_t expected_count) {
  std::optional<limitless::SwiftUIAgentRailSnapshotForTesting> snapshot;
  if (!base::test::RunUntil([&]() {
        snapshot = limitless::GetSwiftUIAgentRailSnapshotForTesting(rail);
        return snapshot && snapshot->source_state == expected_state &&
               snapshot->pane_identifiers.size() == expected_count;
      })) {
    return std::nullopt;
  }
  return snapshot;
}

void ExpectNoAgentRows(
    const limitless::SwiftUIAgentRailSnapshotForTesting& snapshot) {
  EXPECT_TRUE(snapshot.pane_identifiers.empty());
  EXPECT_TRUE(snapshot.workspace_identifiers.empty());
  EXPECT_TRUE(snapshot.workspace_labels.empty());
  EXPECT_TRUE(snapshot.agent_kinds.empty());
  EXPECT_TRUE(snapshot.statuses.empty());
  EXPECT_TRUE(snapshot.terminal_titles.empty());
  EXPECT_TRUE(snapshot.focused.empty());
}

std::string BuildHerdrSnapshot(size_t agent_count, bool updated) {
  std::string snapshot =
      R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","workspaces":[{"workspace_id":"w1","label":"Alpha Workspace"},{"workspace_id":"w2","label":"Beta Workspace"}],"agents":[)json";
  for (size_t index = 0; index < agent_count; ++index) {
    if (index) {
      snapshot += ',';
    }
    const bool first = index == 0;
    const bool second = index == 1;
    const std::string workspace_id = index % 2 == 0 ? "w1" : "w2";
    const std::string pane_id = workspace_id + ":p" + std::to_string(index + 1);
    const std::string status = first && updated ? "done"
                               : second         ? "blocked"
                                                : "working";
    const bool focused = first ? updated : second && !updated;
    snapshot += "{\"pane_id\":\"" + pane_id + "\",\"workspace_id\":\"" +
                workspace_id + "\",\"agent\":\"codex\",\"agent_status\":\"" +
                status + "\",\"terminal_title_stripped\":\"Agent " +
                std::to_string(index + 1) +
                "\",\"focused\":" + (focused ? "true" : "false") + "}";
  }
  snapshot += "]}}}";
  return snapshot;
}

class LimitlessSwiftUIBrowserTest : public InProcessBrowserTest {
 public:
  LimitlessSwiftUIBrowserTest()
      : https_server_(net::EmbeddedTestServer::TYPE_HTTPS) {
    https_server_.SetSSLConfig(net::EmbeddedTestServer::CERT_TEST_NAMES);
  }

  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {}, {features::kWebium, tabs::kTabStripUnification});
    InProcessBrowserTest::SetUp();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(limitless::kSwiftUIBoundaryProbeSwitch);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    https_server_.RegisterRequestHandler(
        base::BindRepeating(&HandleTabRailRequest));
    ASSERT_TRUE(https_server_.Start());
  }

 protected:
  net::EmbeddedTestServer https_server_;

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

class LimitlessSwiftUIBrowserUnifiedTest : public InProcessBrowserTest {
 public:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {tabs::kTabStripUnification, tabs::kVerticalTabs}, {features::kWebium});
    InProcessBrowserTest::SetUp();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(limitless::kSwiftUIBoundaryProbeSwitch);
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

class LimitlessSwiftUIBrowserDisabledTest : public InProcessBrowserTest {
 public:
  void SetUp() override {
    scoped_feature_list_.InitAndDisableFeature(features::kWebium);
    InProcessBrowserTest::SetUp();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

class LimitlessSwiftUILeaderDisabledBrowserTest
    : public LimitlessSwiftUIBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    LimitlessSwiftUIBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(limitless::kLeaderShortcutsSwitch,
                                    "disabled");
  }
};

class LimitlessSwiftUIWorkspacesDisabledBrowserTest
    : public LimitlessSwiftUIBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    LimitlessSwiftUIBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(limitless::kWorkspacesSwitch, "disabled");
  }
};

class LimitlessSwiftUICustomWorkspaceBrowserTest
    : public LimitlessSwiftUIBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    LimitlessSwiftUIBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchUTF8(limitless::kDefaultWorkspaceNameSwitch,
                                   "D\xC3\xA9j\xC3\xA0 Vu");
  }
};

class LimitlessSwiftUIHerdrBrowserTest : public LimitlessSwiftUIBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    CHECK(temp_dir_.CreateUniqueTempDir());
    response_path_ = temp_dir_.GetPath().Append("snapshot.json");
    executable_path_ = temp_dir_.GetPath().Append("fake-herdr");
    CHECK(ReplaceResponseAtomically(BuildHerdrSnapshot(2, false)));
    const std::string executable =
        "#!/bin/sh\n"
        "test \"$#\" -eq 2 || exit 90\n"
        "test \"$1\" = api || exit 91\n"
        "test \"$2\" = snapshot || exit 92\n"
        "while IFS= read -r line || test -n \"$line\"; do\n"
        "  printf '%s\\n' \"$line\"\n"
        "done < '" +
        response_path_.value() + "'\n";
    CHECK(base::WriteFile(executable_path_, executable));
    CHECK(base::SetPosixFilePermissions(executable_path_, 0700));
    LimitlessSwiftUIBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchPath(limitless::kHerdrExecutableSwitch,
                                   executable_path_);
  }

 protected:
  bool WriteResponse(std::string_view response) {
    return ReplaceResponseAtomically(response);
  }

  bool WriteSnapshot(size_t agent_count, bool updated) {
    return ReplaceResponseAtomically(BuildHerdrSnapshot(agent_count, updated));
  }

 private:
  bool ReplaceResponseAtomically(std::string_view response) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::FilePath temporary_path;
    if (!base::CreateTemporaryFileInDir(temp_dir_.GetPath(), &temporary_path)) {
      return false;
    }
    if (!base::WriteFile(temporary_path, response)) {
      base::DeleteFile(temporary_path);
      return false;
    }
    base::File::Error error = base::File::FILE_OK;
    if (!base::ReplaceFile(temporary_path, response_path_, &error)) {
      base::DeleteFile(temporary_path);
      return false;
    }
    return true;
  }

  base::ScopedTempDir temp_dir_;
  base::FilePath response_path_;
  base::FilePath executable_path_;
};

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       HostsInExistingWindowAndReservesContentSpace) {
  EXPECT_EQ(nullptr, WebUIBrowserWindow::FromBrowser(browser()));

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  EXPECT_EQ(nullptr, browser_view->horizontal_tab_strip_for_testing());
  ASSERT_TRUE(browser_view->tab_strip_view());
  EXPECT_EQ(nullptr, browser_view->tab_strip_view()->GetTabStripView());
  views::NativeViewHost* host = views::AsViewClass<views::NativeViewHost>(
      browser_view->limitless_swiftui_rail_for_testing());
  ASSERT_TRUE(host);
  ASSERT_TRUE(base::test::RunUntil([host]() { return !!host->native_view(); }));

  NSView* native_view = host->native_view().GetNativeNSView();
  NSWindow* browser_window =
      browser()->GetWindow()->GetNativeWindow().GetNativeNSWindow();
  ASSERT_TRUE(native_view);
  ASSERT_TRUE(browser_window);
  EXPECT_EQ(browser_window, native_view.window);
  EXPECT_TRUE([native_view isDescendantOf:browser_window.contentView]);
  EXPECT_EQ(limitless::kSwiftUITabRailWidth, host->width());
  EXPECT_GT(host->height(), 0);
  EXPECT_EQ(host->width(), NSWidth(native_view.frame));
  EXPECT_EQ(host->height(), NSHeight(native_view.frame));
  EXPECT_GE(browser_view->multi_contents_view()->x(), host->bounds().right());
  EXPECT_EQ(1, limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       ExposesViewsFocusAndAccessibilityPane) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  views::NativeViewHost* host = views::AsViewClass<views::NativeViewHost>(
      browser_view->limitless_swiftui_rail_for_testing());
  ASSERT_TRUE(host);
  ASSERT_TRUE(base::test::RunUntil([host]() { return !!host->native_view(); }));

  browser_view->GetWidget()->Activate();
  host->RequestFocus();
  ASSERT_TRUE(host->GetFocusManager());
  EXPECT_EQ(host, host->GetFocusManager()->GetFocusedView());

  std::vector<views::View*> panes;
  browser_view->GetAccessiblePanes(&panes);
  EXPECT_NE(panes.end(), std::ranges::find(panes, host));
  EXPECT_EQ(ax::mojom::Role::kPane,
            host->GetViewAccessibility().GetCachedRole());
  EXPECT_EQ(u"Limitless sidebar", host->GetViewAccessibility().GetCachedName());

  auto snapshot = WaitForRailTabs(host, 1);
  ASSERT_TRUE(snapshot);
  ASSERT_EQ(1u, snapshot->workspace_identifiers.size());
  ASSERT_TRUE(snapshot->active_workspace_identifier);
  const std::string default_workspace = snapshot->workspace_identifiers[0];
  const int32_t default_tab = snapshot->tab_identifiers[0];

  auto create_control = limitless::GetSwiftUIAccessibilityControlForTesting(
      host, "limitless-workspace-create");
  ASSERT_TRUE(create_control);
  EXPECT_EQ(base::SysNSStringToUTF8(NSAccessibilityButtonRole),
            create_control->role);
  EXPECT_EQ(u"Create workspace", create_control->label);
  EXPECT_TRUE(create_control->enabled);
  EXPECT_NE(
      create_control->actions.end(),
      std::ranges::find(create_control->actions,
                        base::SysNSStringToUTF8(NSAccessibilityPressAction)));
  EXPECT_TRUE(limitless::FocusSwiftUIAccessibilityControlForTesting(
      host, "limitless-workspace-create"));
  ASSERT_TRUE(WaitForCondition([&]() {
    const auto focused = limitless::GetSwiftUIAccessibilityControlForTesting(
        host, "limitless-workspace-create");
    return focused && focused->focused;
  }));
  EXPECT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      host, "limitless-workspace-create",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(host);
    return snapshot && snapshot->workspace_identifiers.size() == 2 &&
           snapshot->active_workspace_identifier != default_workspace &&
           snapshot->tab_identifiers.size() == 1 &&
           snapshot->active_tab_identifier == snapshot->tab_identifiers[0];
  }));
  const std::string created_workspace = *snapshot->active_workspace_identifier;

  const std::string default_switch_identifier =
      "limitless-workspace-" + default_workspace;
  auto switch_control = limitless::GetSwiftUIAccessibilityControlForTesting(
      host, default_switch_identifier);
  ASSERT_TRUE(switch_control);
  EXPECT_EQ(u"Workspace", switch_control->label);
  EXPECT_TRUE(limitless::FocusSwiftUIAccessibilityControlForTesting(
      host, default_switch_identifier));
  ASSERT_TRUE(WaitForCondition([&]() {
    const auto focused = limitless::GetSwiftUIAccessibilityControlForTesting(
        host, default_switch_identifier);
    return focused && focused->focused;
  }));
  EXPECT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      host, default_switch_identifier,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(host);
    return snapshot &&
           snapshot->active_workspace_identifier == default_workspace &&
           snapshot->active_tab_identifier == default_tab;
  }));

  const std::string move_control_identifier =
      "limitless-tab-move-" + base::NumberToString(default_tab);
  auto move_control = limitless::GetSwiftUIAccessibilityControlForTesting(
      host, move_control_identifier);
  ASSERT_TRUE(move_control);
  EXPECT_TRUE(move_control->role ==
                  base::SysNSStringToUTF8(NSAccessibilityMenuButtonRole) ||
              move_control->role ==
                  base::SysNSStringToUTF8(NSAccessibilityPopUpButtonRole));
  const std::u16string moved_tab_title = snapshot->tab_titles[0];
  EXPECT_EQ(u"Move " + moved_tab_title + u" to another workspace",
            move_control->label);
  EXPECT_TRUE(move_control->enabled);
  EXPECT_TRUE(
      std::ranges::find(
          move_control->actions,
          base::SysNSStringToUTF8(NSAccessibilityShowMenuAction)) !=
          move_control->actions.end() ||
      std::ranges::find(move_control->actions,
                        base::SysNSStringToUTF8(NSAccessibilityPressAction)) !=
          move_control->actions.end());
  EXPECT_TRUE(limitless::FocusSwiftUIAccessibilityControlForTesting(
      host, move_control_identifier));
  ASSERT_TRUE(WaitForCondition([&]() {
    const auto focused = limitless::GetSwiftUIAccessibilityControlForTesting(
        host, move_control_identifier);
    return focused && focused->focused;
  }));
  const std::string move_destination_identifier =
      "limitless-tab-move-" + base::NumberToString(default_tab) + "-to-" +
      created_workspace;
  const auto menu_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          host, move_control_identifier, move_destination_identifier);
  ASSERT_TRUE(menu_action);
  EXPECT_FALSE(menu_action->inventory.hit_node_cap);
  EXPECT_LE(menu_action->inventory.node_count, 6u);
  EXPECT_EQ(1, std::ranges::count(menu_action->inventory.identifiers,
                                  move_destination_identifier));
  EXPECT_EQ(base::SysNSStringToUTF8(NSAccessibilityMenuItemRole),
            menu_action->destination.role);
  EXPECT_EQ(u"Move " + moved_tab_title + u" to Workspace 2",
            menu_action->destination.label);
  EXPECT_TRUE(menu_action->destination.enabled);
  EXPECT_TRUE(menu_action->destination.focused);
  EXPECT_NE(
      menu_action->destination.actions.end(),
      std::ranges::find(menu_action->destination.actions,
                        base::SysNSStringToUTF8(NSAccessibilityPressAction)));
  EXPECT_TRUE(menu_action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    limitless::WorkspaceController* controller =
        limitless::WorkspaceController::From(browser());
    if (!controller) {
      return false;
    }
    const std::optional<std::string> serialized =
        controller->SerializeTabForSession(tabs::TabHandle(default_tab));
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(host);
    return serialized &&
           ReadWorkspaceTabSessionData(*serialized).second ==
               created_workspace &&
           snapshot &&
           snapshot->active_workspace_identifier == created_workspace &&
           std::ranges::find(snapshot->tab_identifiers, default_tab) !=
               snapshot->tab_identifiers.end();
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       ProjectsAuthoritativeTabsAndRoutesRailIntents) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(3, tab_strip->count());
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  tabs::TabInterface* three = tab_strip->GetTabAtIndex(2);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  const tabs::TabHandle one_handle = one->GetHandle();
  const tabs::TabHandle two_handle = two->GetHandle();
  const tabs::TabHandle three_handle = three->GetHandle();
  content::WebContents* one_contents = one->GetContents();
  content::WebContents* two_contents = two->GetContents();
  content::WebContents* three_contents = three->GetContents();

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> snapshot =
      WaitForRailTabs(rail, 3);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(
      (std::vector<int32_t>{one_handle.raw_value(), two_handle.raw_value(),
                            three_handle.raw_value()}),
      snapshot->tab_identifiers);
  EXPECT_EQ(
      (std::vector<std::u16string>{u"Rail One", u"Rail Two", u"Rail Three"}),
      snapshot->tab_titles);
  EXPECT_EQ(one_handle.raw_value(), snapshot->active_tab_identifier);

  ASSERT_TRUE(
      content::ExecJs(two_contents, "document.title = 'Rail Two Updated'"));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_titles.size() == 3 &&
           snapshot->tab_titles[1] == u"Rail Two Updated";
  }));

  EXPECT_TRUE(
      limitless::ActivateSwiftUITabForTesting(rail, three_handle.raw_value()));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->GetActiveTab() == three && snapshot &&
           snapshot->active_tab_identifier == three_handle.raw_value();
  }));
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(two_contents, two->GetContents());
  EXPECT_EQ(three_contents, three->GetContents());

  EXPECT_TRUE(
      limitless::CloseSwiftUITabForTesting(rail, two_handle.raw_value()));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->count() == 2 && snapshot &&
           snapshot->tab_identifiers.size() == 2;
  }));
  EXPECT_EQ(nullptr, two_handle.Get());
  EXPECT_FALSE(
      limitless::ActivateSwiftUITabForTesting(rail, two_handle.raw_value()));
  EXPECT_FALSE(
      limitless::CloseSwiftUITabForTesting(rail, two_handle.raw_value()));
  EXPECT_EQ(
      (std::vector<int32_t>{one_handle.raw_value(), three_handle.raw_value()}),
      snapshot->tab_identifiers);
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(three_contents, three->GetContents());

  EXPECT_EQ(0, tab_strip->MoveWebContentsAt(/*index=*/1, /*to_position=*/0,
                                            /*select_after_move=*/false));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers ==
                           std::vector<int32_t>{three_handle.raw_value(),
                                                one_handle.raw_value()};
  }));
  EXPECT_EQ(three, tab_strip->GetTabAtIndex(0));
  EXPECT_EQ(one, tab_strip->GetTabAtIndex(1));
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(three_contents, three->GetContents());
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  const std::u16string accessible_window_title =
      browser_view->GetAccessibleWindowTitle();
  EXPECT_NE(std::u16string::npos, accessible_window_title.find(u"Rail Three"));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       ExposesNativeSplitAffordanceForRealWorkspaceTabs) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  content::WebContents* one_contents = one->GetContents();
  content::WebContents* two_contents = two->GetContents();
  ASSERT_TRUE(content::ExecJs(one_contents, R"JS(
        window.limitlessSplitState = 'one';
        window.limitlessSplitKeys = [];
        const input = document.createElement('input');
        input.id = 'limitless-split-input';
        input.setAttribute('aria-label', 'First split input');
        input.addEventListener('keydown', event => {
          window.limitlessSplitKeys.push(event.key);
        });
        document.body.appendChild(input);
        input.focus();
      )JS"));
  ASSERT_TRUE(content::ExecJs(two_contents, R"JS(
        window.limitlessSplitState = 'two';
        window.limitlessSplitKeys = [];
        const input = document.createElement('input');
        input.id = 'limitless-split-input';
        input.setAttribute('aria-label', 'Second split input');
        input.addEventListener('keydown', event => {
          window.limitlessSplitKeys.push(event.key);
        });
        document.body.appendChild(input);
        input.focus();
      )JS"));

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  const auto initial_rail_snapshot = WaitForRailTabs(rail, 2);
  ASSERT_TRUE(initial_rail_snapshot);
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  const limitless::WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller->GetSnapshot();
  ASSERT_EQ(2u, workspace_snapshot.visible_tab_projections.size());
  ASSERT_EQ(one->GetHandle(),
            workspace_snapshot.visible_tab_projections[0].handle);
  ASSERT_EQ(two->GetHandle(),
            workspace_snapshot.visible_tab_projections[1].handle);
  EXPECT_EQ((std::vector<std::string>{
                workspace_snapshot.visible_tab_projections[0].identifier,
                workspace_snapshot.visible_tab_projections[1].identifier}),
            initial_rail_snapshot->tab_product_identifiers);
  EXPECT_EQ(0u,
            initial_rail_snapshot->split_eligibility_batch_invocation_count);
  for (const std::string& identifier :
       initial_rail_snapshot->tab_product_identifiers) {
    const base::Uuid parsed = base::Uuid::ParseLowercase(identifier);
    EXPECT_TRUE(parsed.is_valid());
    EXPECT_EQ(identifier, parsed.AsLowercaseString());
    EXPECT_FALSE(base::StartsWith(identifier, "transient-"));
    EXPECT_NE(identifier, std::to_string(one->GetHandle().raw_value()));
    EXPECT_NE(identifier, std::to_string(two->GetHandle().raw_value()));
  }

  const std::string split_control_identifier =
      "limitless-tab-split-" +
      workspace_snapshot.visible_tab_projections[0].identifier;
  const std::string split_destination_identifier =
      split_control_identifier + "-with-" +
      workspace_snapshot.visible_tab_projections[1].identifier;
  const auto closed_inventory =
      limitless::GetSwiftUIAccessibilityInventoryForTesting(rail);
  EXPECT_FALSE(closed_inventory.hit_node_cap);
  EXPECT_EQ(0, std::ranges::count(closed_inventory.identifiers,
                                  split_destination_identifier));
  EXPECT_EQ(closed_inventory.identifiers.end(),
            std::ranges::find_if(
                closed_inventory.identifiers, [](const std::string& value) {
                  return value.find("transient-") != std::string::npos;
                }));
  const auto split_control =
      limitless::GetSwiftUIAccessibilityControlForTesting(
          rail, split_control_identifier);
  ASSERT_TRUE(split_control);
  EXPECT_TRUE(split_control->enabled);
  EXPECT_EQ(u"Split Rail One with another tab", split_control->label);
  EXPECT_TRUE(split_control->role ==
                  base::SysNSStringToUTF8(NSAccessibilityMenuButtonRole) ||
              split_control->role ==
                  base::SysNSStringToUTF8(NSAccessibilityPopUpButtonRole));
  ASSERT_TRUE(limitless::FocusSwiftUIAccessibilityControlForTesting(
      rail, split_control_identifier));
  const auto closed_projection =
      limitless::GetSwiftUITabRailSnapshotForTesting(rail);
  ASSERT_TRUE(closed_projection);
  EXPECT_EQ(0u, closed_projection->split_eligibility_batch_invocation_count);
  const auto split_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, split_control_identifier, split_destination_identifier);
  ASSERT_TRUE(split_action);
  EXPECT_FALSE(split_action->inventory.hit_node_cap);
  const auto destination_inventory_count = std::ranges::count(
      split_action->inventory.identifiers, split_destination_identifier);
  EXPECT_EQ(1, destination_inventory_count);
  EXPECT_EQ(base::SysNSStringToUTF8(NSAccessibilityMenuItemRole),
            split_action->destination.role);
  EXPECT_EQ(u"Split Rail One with Rail Two", split_action->destination.label);
  EXPECT_TRUE(split_action->destination.enabled);
  EXPECT_TRUE(split_action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    return one->GetSplit().has_value() && one->GetSplit() == two->GetSplit();
  }));
  const auto projected_split =
      limitless::GetSwiftUITabRailSnapshotForTesting(rail);
  ASSERT_TRUE(projected_split);
  EXPECT_GT(projected_split->split_eligibility_batch_invocation_count, 0u);
  EXPECT_LE(projected_split->split_eligibility_batch_invocation_count, 4u);

  const split_tabs::SplitTabId split_id = *one->GetSplit();
  ASSERT_TRUE(tab_strip->ContainsSplit(split_id));
  split_tabs::SplitTabData* split_data = tab_strip->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  EXPECT_EQ((std::vector<tabs::TabInterface*>{one, two}),
            split_data->ListTabs());
  EXPECT_EQ(2, tab_strip->count());
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(two_contents, two->GetContents());
  EXPECT_EQ("one", content::EvalJs(one_contents, "limitlessSplitState"));
  EXPECT_EQ("two", content::EvalJs(two_contents, "limitlessSplitState"));

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  MultiContentsView* multi_contents = browser_view->multi_contents_view();
  ASSERT_TRUE(multi_contents);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return multi_contents->IsInSplitView() &&
           multi_contents->start_contents_view_for_testing()->web_contents() ==
               one_contents &&
           multi_contents->end_contents_view_for_testing()->web_contents() ==
               two_contents &&
           !multi_contents->start_contents_view_for_testing()
                ->GetBoundsInScreen()
                .IsEmpty() &&
           !multi_contents->end_contents_view_for_testing()
                ->GetBoundsInScreen()
                .IsEmpty();
  }));
  EXPECT_EQ(one_contents,
            multi_contents->start_contents_view_for_testing()->web_contents());
  EXPECT_EQ(two_contents,
            multi_contents->end_contents_view_for_testing()->web_contents());
  const gfx::Rect start_bounds =
      multi_contents->start_contents_view_for_testing()->GetBoundsInScreen();
  const gfx::Rect end_bounds =
      multi_contents->end_contents_view_for_testing()->GetBoundsInScreen();
  EXPECT_GT(start_bounds.width(), 0);
  EXPECT_GT(start_bounds.height(), 0);
  EXPECT_GT(end_bounds.width(), 0);
  EXPECT_GT(end_bounds.height(), 0);
  EXPECT_LE(start_bounds.right(), end_bounds.x());

  std::optional<limitless::SwiftUITabRailSnapshotForTesting> split_snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return split_snapshot &&
           split_snapshot->split_pair_identifiers.size() == 1 &&
           split_snapshot->split_active_tab_identifiers ==
               std::vector<std::string>{
                   workspace_snapshot.visible_tab_projections[0].identifier};
  }));
  const auto first_row = limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-tab-" + std::to_string(one->GetHandle().raw_value()));
  const auto second_row = limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-tab-" + std::to_string(two->GetHandle().raw_value()));
  ASSERT_TRUE(first_row);
  ASSERT_TRUE(second_row);
  EXPECT_EQ(u"Focus first pane, Rail One", first_row->label);
  EXPECT_EQ(u"Focused pane", first_row->value);
  EXPECT_EQ(u"Focus second pane, Rail Two", second_row->label);
  EXPECT_EQ(u"Visible pane", second_row->value);

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-tab-" + std::to_string(two->GetHandle().raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    content::RenderWidgetHostView* renderer_view =
        two_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == two && split_snapshot &&
           split_snapshot->split_active_tab_identifiers ==
               std::vector<std::string>{
                   workspace_snapshot.visible_tab_projections[1].identifier} &&
           renderer_view && renderer_view->HasFocus();
  }));
  EXPECT_EQ("limitless-split-input",
            content::EvalJs(two_contents, "document.activeElement.id"));
  SendRendererKey(browser(), ui::VKEY_Z, ui::EF_NONE, u'z');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return content::EvalJs(
               two_contents,
               "document.querySelector('#limitless-split-input').value") == "z";
  }));
  EXPECT_EQ(0, content::EvalJs(one_contents, "limitlessSplitKeys.length"));
  EXPECT_EQ("", content::EvalJs(
                    one_contents,
                    "document.querySelector('#limitless-split-input').value"));
  EXPECT_EQ(1, content::EvalJs(two_contents, "limitlessSplitKeys.length"));
  EXPECT_EQ("z", content::EvalJs(two_contents, "limitlessSplitKeys[0]"));
  EXPECT_EQ("one", content::EvalJs(one_contents, "limitlessSplitState"));
  EXPECT_EQ("two", content::EvalJs(two_contents, "limitlessSplitState"));

  const auto layout_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, "limitless-split-layout", "limitless-split-layout-stacked");
  ASSERT_TRUE(layout_action);
  EXPECT_TRUE(layout_action->destination.enabled);
  EXPECT_TRUE(layout_action->press_invoked);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_tabs::SplitTabData* current = tab_strip->GetSplitData(split_id);
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    const gfx::Rect current_start =
        multi_contents->start_contents_view_for_testing()->GetBoundsInScreen();
    const gfx::Rect current_end =
        multi_contents->end_contents_view_for_testing()->GetBoundsInScreen();
    return current &&
           current->visual_data()->split_layout() ==
               split_tabs::SplitTabLayout::kStacked &&
           current->visual_data()->split_ratio() == 0.5 && split_snapshot &&
           split_snapshot->split_layouts ==
               std::vector<std::string>{"stacked"} &&
           split_snapshot->split_ratios == std::vector<double>{0.5} &&
           current_start.height() > 0 && current_end.height() > 0 &&
           current_start.bottom() <= current_end.y();
  }));
  const gfx::Rect stacked_start =
      multi_contents->start_contents_view_for_testing()->GetBoundsInScreen();
  const gfx::Rect stacked_end =
      multi_contents->end_contents_view_for_testing()->GetBoundsInScreen();
  EXPECT_LE(stacked_start.bottom(), stacked_end.y());

  const auto ratio_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, "limitless-split-ratio", "limitless-split-ratio-25");
  ASSERT_TRUE(ratio_action);
  EXPECT_TRUE(ratio_action->destination.enabled);
  EXPECT_TRUE(ratio_action->press_invoked);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_tabs::SplitTabData* current = tab_strip->GetSplitData(split_id);
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    const int current_start_height =
        multi_contents->start_contents_view_for_testing()
            ->GetBoundsInScreen()
            .height();
    const int current_end_height =
        multi_contents->end_contents_view_for_testing()
            ->GetBoundsInScreen()
            .height();
    return current && current->visual_data()->split_ratio() == 0.25 &&
           split_snapshot &&
           split_snapshot->split_layouts ==
               std::vector<std::string>{"stacked"} &&
           split_snapshot->split_ratios == std::vector<double>{0.25} &&
           current_start_height > 0 && current_end_height > 0 &&
           current_start_height < current_end_height;
  }));
  EXPECT_LT(multi_contents->start_contents_view_for_testing()
                ->GetBoundsInScreen()
                .height(),
            multi_contents->end_contents_view_for_testing()
                ->GetBoundsInScreen()
                .height());

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-split-swap",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_tabs::SplitTabData* current = tab_strip->GetSplitData(split_id);
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return current &&
           current->ListTabs() == std::vector<tabs::TabInterface*>{two, one} &&
           split_snapshot &&
           split_snapshot->split_first_tab_identifiers ==
               std::vector<std::string>{
                   workspace_snapshot.visible_tab_projections[1].identifier} &&
           split_snapshot->split_second_tab_identifiers ==
               std::vector<std::string>{
                   workspace_snapshot.visible_tab_projections[0].identifier};
  }));
  EXPECT_EQ(two_contents,
            multi_contents->start_contents_view_for_testing()->web_contents());
  EXPECT_EQ(one_contents,
            multi_contents->end_contents_view_for_testing()->web_contents());

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-split-unsplit",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    return !one->IsSplit() && !two->IsSplit() &&
           !multi_contents->IsInSplitView();
  }));
  const std::string recreate_menu =
      "limitless-tab-split-" +
      workspace_snapshot.visible_tab_projections[0].identifier;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    const auto recreate_control =
        limitless::GetSwiftUIAccessibilityControlForTesting(rail,
                                                            recreate_menu);
    return split_snapshot && split_snapshot->split_pair_identifiers.empty() &&
           recreate_control && recreate_control->enabled;
  }));
  EXPECT_EQ(2, tab_strip->count());
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(two_contents, two->GetContents());
  EXPECT_EQ("one", content::EvalJs(one_contents, "limitlessSplitState"));
  EXPECT_EQ("two", content::EvalJs(two_contents, "limitlessSplitState"));

  const auto recreate_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, recreate_menu,
          recreate_menu + "-with-" +
              workspace_snapshot.visible_tab_projections[1].identifier);
  ASSERT_TRUE(recreate_action);
  ASSERT_TRUE(recreate_action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    return one->GetSplit().has_value() && one->GetSplit() == two->GetSplit();
  }));
  const tabs::TabHandle two_handle = two->GetHandle();
  const std::string close_two_identifier =
      "limitless-tab-close-" + std::to_string(two_handle.raw_value());
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    const auto close_control =
        limitless::GetSwiftUIAccessibilityControlForTesting(
            rail, close_two_identifier);
    return split_snapshot &&
           split_snapshot->split_pair_identifiers.size() == 1 &&
           close_control && close_control->enabled;
  }));
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, close_two_identifier,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->count() == 1 && two_handle.Get() == nullptr &&
           !one->IsSplit() && !multi_contents->IsInSplitView() &&
           split_snapshot && split_snapshot->split_pair_identifiers.empty() &&
           split_snapshot->tab_identifiers ==
               std::vector<int32_t>{one->GetHandle().raw_value()};
  }));
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ("one", content::EvalJs(one_contents, "limitlessSplitState"));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    SplitBeforeUnloadCancelOrAcceptKeepsExactPaneAndFocusAuthoritative) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  content::WebContents* one_contents = one->GetContents();
  content::WebContents* two_contents = two->GetContents();
  ASSERT_TRUE(content::ExecJs(one_contents, R"JS(
        const input = document.createElement('input');
        input.id = 'limitless-prompt-input';
        document.body.appendChild(input);
      )JS"));
  ASSERT_TRUE(content::ExecJs(two_contents, R"JS(
        const input = document.createElement('input');
        input.id = 'limitless-prompt-input';
        document.body.appendChild(input);
        window.onbeforeunload = event => {
          event.preventDefault();
          event.returnValue = '';
        };
      )JS"));

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(WaitForRailTabs(rail, 2));
  ASSERT_TRUE(CreateNativeSplitThroughRail(rail, browser(), one, two));
  const split_tabs::SplitTabId split_id = *one->GetSplit();
  const tabs::TabHandle one_handle = one->GetHandle();
  const tabs::TabHandle two_handle = two->GetHandle();
  const std::string two_row_identifier =
      "limitless-tab-" + std::to_string(two_handle.raw_value());
  const std::string close_two_identifier =
      "limitless-tab-close-" + std::to_string(two_handle.raw_value());
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, two_row_identifier,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* renderer_view =
        two_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == two && renderer_view &&
           renderer_view->HasFocus();
  }));
  ASSERT_TRUE(content::ExecJs(
      two_contents,
      "document.querySelector('#limitless-prompt-input').focus()"));

  content::PrepContentsForBeforeUnloadTest(two_contents);
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, close_two_identifier,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  javascript_dialogs::AppModalDialogController* prompt =
      ui_test_utils::WaitForAppModalDialog();
  base::ScopedClosureRunner prompt_cleanup(base::BindOnce([]() {
    javascript_dialogs::AppModalDialogQueue* queue =
        javascript_dialogs::AppModalDialogQueue::GetInstance();
    if (queue->HasActiveDialog()) {
      queue->active_dialog()->view()->CancelAppModalDialog();
    }
  }));
  ASSERT_TRUE(prompt);
  ASSERT_TRUE(prompt->IsValid());
  EXPECT_TRUE(prompt->is_before_unload_dialog());
  EXPECT_EQ(two_contents, prompt->web_contents());
  EXPECT_EQ(two, tab_strip->GetActiveTab());
  EXPECT_EQ(split_id, one->GetSplit());
  EXPECT_EQ(split_id, two->GetSplit());
  const auto prompted_snapshot =
      limitless::GetSwiftUITabRailSnapshotForTesting(rail);
  ASSERT_TRUE(prompted_snapshot);
  EXPECT_EQ(1u, prompted_snapshot->split_pair_identifiers.size());

  prompt->view()->CancelAppModalDialog();
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* renderer_view =
        two_contents->GetRenderWidgetHostView();
    return one_handle.Get() == one && two_handle.Get() == two &&
           tab_strip->GetActiveTab() == two && renderer_view &&
           renderer_view->HasFocus();
  }));
  std::ignore = prompt_cleanup.Release();
  EXPECT_EQ(split_id, one->GetSplit());
  EXPECT_EQ(split_id, two->GetSplit());
  SendRendererKey(browser(), ui::VKEY_C, ui::EF_NONE, u'c');
  ASSERT_TRUE(WaitForCondition([&]() {
    return content::EvalJs(
               two_contents,
               "document.querySelector('#limitless-prompt-input').value") ==
           "c";
  }));
  EXPECT_EQ("", content::EvalJs(
                    one_contents,
                    "document.querySelector('#limitless-prompt-input').value"));

  content::PrepContentsForBeforeUnloadTest(two_contents);
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, close_two_identifier,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  prompt = ui_test_utils::WaitForAppModalDialog();
  base::ScopedClosureRunner accept_prompt_cleanup(base::BindOnce([]() {
    javascript_dialogs::AppModalDialogQueue* queue =
        javascript_dialogs::AppModalDialogQueue::GetInstance();
    if (queue->HasActiveDialog()) {
      queue->active_dialog()->view()->CancelAppModalDialog();
    }
  }));
  ASSERT_TRUE(prompt);
  ASSERT_TRUE(prompt->IsValid());
  EXPECT_TRUE(prompt->is_before_unload_dialog());
  EXPECT_EQ(two_contents, prompt->web_contents());
  prompt->view()->AcceptAppModalDialog();

  std::optional<limitless::SwiftUITabRailSnapshotForTesting> final_snapshot;
  ASSERT_TRUE(WaitForCondition([&]() {
    final_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    content::RenderWidgetHostView* renderer_view =
        one_contents->GetRenderWidgetHostView();
    return two_handle.Get() == nullptr && one_handle.Get() == one &&
           tab_strip->count() == 1 && tab_strip->GetActiveTab() == one &&
           !one->IsSplit() && final_snapshot &&
           final_snapshot->split_pair_identifiers.empty() && renderer_view &&
           renderer_view->HasFocus();
  }));
  std::ignore = accept_prompt_cleanup.Release();
  ASSERT_TRUE(content::ExecJs(
      one_contents,
      "document.querySelector('#limitless-prompt-input').focus()"));
  SendRendererKey(browser(), ui::VKEY_A, ui::EF_NONE, u'a');
  ASSERT_TRUE(WaitForCondition([&]() {
    return content::EvalJs(
               one_contents,
               "document.querySelector('#limitless-prompt-input').value") ==
           "a";
  }));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    SplitJavaScriptPromptUsesExactPaneHostAndRetainsNativeFocusRoute) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  content::WebContents* one_contents = one->GetContents();
  content::WebContents* two_contents = two->GetContents();
  ASSERT_TRUE(content::ExecJs(one_contents, R"JS(
        const input = document.createElement('input');
        input.id = 'limitless-dialog-input';
        document.body.appendChild(input);
      )JS"));
  ASSERT_TRUE(content::ExecJs(two_contents, R"JS(
        const input = document.createElement('input');
        input.id = 'limitless-dialog-input';
        document.body.appendChild(input);
      )JS"));

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(WaitForRailTabs(rail, 2));
  ASSERT_TRUE(CreateNativeSplitThroughRail(rail, browser(), one, two));
  const split_tabs::SplitTabId split_id = *one->GetSplit();
  const std::string first_identifier = ProductIdentifierForTab(browser(), one);
  const std::string second_identifier = ProductIdentifierForTab(browser(), two);
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-tab-" + std::to_string(one->GetHandle().raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(
      WaitForCondition([&]() { return tab_strip->GetActiveTab() == one; }));
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()));

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  web_modal::WebContentsModalDialogHost* first_host =
      browser_view->GetWebContentsModalDialogHostFor(one_contents);
  web_modal::WebContentsModalDialogHost* second_host =
      browser_view->GetWebContentsModalDialogHostFor(two_contents);
  ASSERT_TRUE(first_host);
  ASSERT_TRUE(second_host);
  EXPECT_NE(first_host, second_host);
  const gfx::Size representative_dialog_size(120, 80);
  const gfx::Point first_position =
      first_host->GetDialogPosition(representative_dialog_size);
  const gfx::Point second_position =
      second_host->GetDialogPosition(representative_dialog_size);
  EXPECT_LT(first_position.x(), second_position.x());

  auto* first_modal_manager =
      web_modal::WebContentsModalDialogManager::FromWebContents(one_contents);
  auto* second_modal_manager =
      web_modal::WebContentsModalDialogManager::FromWebContents(two_contents);
  auto* prompt_manager =
      javascript_dialogs::TabModalDialogManager::FromWebContents(two_contents);
  ASSERT_TRUE(first_modal_manager);
  ASSERT_TRUE(second_modal_manager);
  ASSERT_TRUE(prompt_manager);
  base::ScopedClosureRunner prompt_cleanup(base::BindOnce(
      [](javascript_dialogs::TabModalDialogManager* manager,
         content::WebContents* contents) {
        if (manager->IsShowingDialogForTesting()) {
          manager->CancelDialogs(contents, /*reset_state=*/false);
        }
      },
      prompt_manager, two_contents));
  two_contents->GetPrimaryMainFrame()->ExecuteJavaScriptWithUserGestureForTests(
      u"window.limitlessPromptResult = 'pending';"
      u"window.limitlessPromptResult = window.prompt('Pane prompt', '') ?? "
      u"'cancelled';"
      u"document.querySelector('#limitless-dialog-input').focus();",
      base::NullCallback(), content::ISOLATED_WORLD_ID_GLOBAL);
  ASSERT_TRUE(WaitForCondition(
      [&]() { return prompt_manager->IsShowingDialogForTesting(); }));
  EXPECT_FALSE(first_modal_manager->IsDialogActive());
  EXPECT_TRUE(second_modal_manager->IsDialogActive());
  ASSERT_TRUE(first_modal_manager->delegate());
  ASSERT_TRUE(second_modal_manager->delegate());
  EXPECT_EQ(first_host,
            first_modal_manager->delegate()->GetWebContentsModalDialogHost(
                one_contents));
  EXPECT_EQ(second_host,
            second_modal_manager->delegate()->GetWebContentsModalDialogHost(
                two_contents));
  EXPECT_EQ(one, tab_strip->GetActiveTab());
  EXPECT_EQ(split_id, one->GetSplit());
  EXPECT_EQ(split_id, two->GetSplit());

  NSWindow* browser_window =
      browser()->GetWindow()->GetNativeWindow().GetNativeNSWindow();
  ASSERT_TRUE(browser_window);
  NSWindow* prompt_window = NSApp.keyWindow;
  ASSERT_TRUE(prompt_window);
  EXPECT_NE(browser_window, prompt_window);
  EXPECT_TRUE(prompt_window.parentWindow == browser_window ||
              [browser_window.childWindows containsObject:prompt_window]);

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-tab-" + std::to_string(two->GetHandle().raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(
      WaitForCondition([&]() { return tab_strip->GetActiveTab() == two; }));
  EXPECT_TRUE(prompt_manager->IsShowingDialogForTesting());
  EXPECT_TRUE(second_modal_manager->IsDialogActive());
  ASSERT_TRUE(
      WaitForCondition([&]() { return NSApp.keyWindow == prompt_window; }));
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> focused_snapshot;
  ASSERT_TRUE(WaitForCondition([&]() {
    focused_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return focused_snapshot && focused_snapshot->split_active_tab_identifiers ==
                                   std::vector<std::string>{second_identifier};
  }));
  ASSERT_TRUE(focused_snapshot);
  EXPECT_EQ(std::vector<std::string>{first_identifier},
            focused_snapshot->split_first_tab_identifiers);
  EXPECT_EQ(std::vector<std::string>{second_identifier},
            focused_snapshot->split_second_tab_identifiers);
  EXPECT_EQ(std::vector<std::string>{second_identifier},
            focused_snapshot->split_active_tab_identifiers);

  const std::u16string prompt_answer = u"pane answer";
  ASSERT_TRUE(prompt_manager->HandleJavaScriptDialog(two_contents,
                                                     /*accept=*/true,
                                                     &prompt_answer));
  EXPECT_FALSE(prompt_manager->IsShowingDialogForTesting());
  ASSERT_TRUE(WaitForCondition(
      [&]() { return !second_modal_manager->IsDialogActive(); }));
  EXPECT_EQ("pane answer",
            content::EvalJs(two_contents, "window.limitlessPromptResult"));
  std::ignore = prompt_cleanup.Release();
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-tab-" + std::to_string(two->GetHandle().raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* renderer_view =
        two_contents->GetRenderWidgetHostView();
    return renderer_view && renderer_view->HasFocus();
  }));
  SendRendererKey(browser(), ui::VKEY_P, ui::EF_NONE, u'p');
  ASSERT_TRUE(WaitForCondition([&]() {
    return content::EvalJs(
               two_contents,
               "document.querySelector('#limitless-dialog-input').value") ==
           "p";
  }));
  EXPECT_EQ("", content::EvalJs(
                    one_contents,
                    "document.querySelector('#limitless-dialog-input').value"));
  EXPECT_EQ(split_id, one->GetSplit());
  EXPECT_EQ(split_id, two->GetSplit());
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(two_contents, two->GetContents());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    NativeBrowserFullscreenPreservesExactSplitProjectionAndPaneFocus) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/keys")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/keys"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  content::WebContents* one_contents = one->GetContents();
  content::WebContents* two_contents = two->GetContents();
  ASSERT_TRUE(ResetKeyEvidence(one_contents, /*focus_input=*/false));
  ASSERT_TRUE(ResetKeyEvidence(two_contents, /*focus_input=*/false));

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(WaitForRailTabs(rail, 2));
  ASSERT_TRUE(CreateNativeSplitThroughRail(rail, browser(), one, two));
  const split_tabs::SplitTabId split_id = *one->GetSplit();
  const auto layout_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, "limitless-split-layout", "limitless-split-layout-stacked");
  ASSERT_TRUE(layout_action);
  ASSERT_TRUE(layout_action->press_invoked);
  const auto ratio_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, "limitless-split-ratio", "limitless-split-ratio-25");
  ASSERT_TRUE(ratio_action);
  ASSERT_TRUE(ratio_action->press_invoked);

  split_tabs::SplitTabData* split_data = tab_strip->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  ASSERT_TRUE(WaitForCondition([&]() {
    split_data = tab_strip->GetSplitData(split_id);
    return split_data &&
           split_data->visual_data()->split_layout() ==
               split_tabs::SplitTabLayout::kStacked &&
           split_data->visual_data()->split_ratio() == 0.25;
  }));
  const std::vector<tabs::TabInterface*> ordered_tabs = split_data->ListTabs();
  ASSERT_EQ(2u, ordered_tabs.size());
  const std::string first_identifier =
      ProductIdentifierForTab(browser(), ordered_tabs[0]);
  const std::string second_identifier =
      ProductIdentifierForTab(browser(), ordered_tabs[1]);
  const std::string active_identifier = ProductIdentifierForTab(browser(), two);
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  const std::optional<std::string> workspace_identifier =
      workspace_controller->GetWorkspaceIdentifierForTab(two->GetHandle());
  ASSERT_TRUE(workspace_identifier);
  EXPECT_EQ(
      workspace_identifier,
      workspace_controller->GetWorkspaceIdentifierForTab(one->GetHandle()));
  const auto focus_two = [&]() {
    return limitless::PerformSwiftUIAccessibilityActionForTesting(
        GetSwiftUITabRail(browser()),
        "limitless-tab-" + std::to_string(two->GetHandle().raw_value()),
        limitless::SwiftUIAccessibilityActionForTesting::kPress);
  };
  const auto focus_one = [&]() {
    return limitless::PerformSwiftUIAccessibilityActionForTesting(
        GetSwiftUITabRail(browser()),
        "limitless-tab-" + std::to_string(one->GetHandle().raw_value()),
        limitless::SwiftUIAccessibilityActionForTesting::kPress);
  };
  ASSERT_TRUE(focus_two());
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        two_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == two && view && view->HasFocus();
  }));
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     active_identifier, "stacked", 0.25));
  ASSERT_TRUE(FocusKeyEvidenceInput(two_contents));
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_NONE, u'b');
  ASSERT_TRUE(
      WaitForCondition([&]() { return KeyInputValue(two_contents) == "b"; }));
  EXPECT_EQ("", KeyInputValue(one_contents));

  BrowserView* initial_browser_view =
      BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(initial_browser_view);
  EXPECT_EQ(nullptr, initial_browser_view->horizontal_tab_strip_for_testing());
  ASSERT_TRUE(initial_browser_view->tab_strip_view());
  EXPECT_EQ(nullptr, initial_browser_view->tab_strip_view()->GetTabStripView());
  EXPECT_EQ(0, initial_browser_view->tab_strip_view()->height());
  ASSERT_TRUE(initial_browser_view->tab_overlay_widget());
  EXPECT_FALSE(initial_browser_view->tab_overlay_widget()->IsVisible());
  EXPECT_GT(initial_browser_view->top_container()->width(), 0);
  EXPECT_GT(rail->width(), 0);
  EXPECT_GT(rail->height(), 0);

  const auto exact_native_state = [&](bool expected_fullscreen) {
    BrowserView* current_view =
        BrowserView::GetBrowserViewForBrowser(browser());
    if (!current_view || current_view->IsFullscreen() != expected_fullscreen) {
      return false;
    }
    if (!current_view->tab_overlay_widget() ||
        current_view->tab_overlay_widget()->IsVisible() ||
        current_view->tab_strip_view()->GetTabStripView() != nullptr ||
        current_view->tab_strip_view()->parent() ==
            current_view->tab_overlay_view()) {
      return false;
    }
    if (!current_view->overlay_widget() ||
        current_view->overlay_widget()->IsVisible() != expected_fullscreen ||
        (expected_fullscreen &&
         current_view->overlay_widget()->GetWindowBoundsInScreen().IsEmpty())) {
      return false;
    }
    NSWindow* native_window =
        browser()->GetWindow()->GetNativeWindow().GetNativeNSWindow();
    const bool native_fullscreen =
        native_window &&
        (native_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (native_fullscreen != expected_fullscreen) {
      return false;
    }
    split_tabs::SplitTabData* current = tab_strip->GetSplitData(split_id);
    if (!current || current->ListTabs() != ordered_tabs ||
        current->visual_data()->split_layout() !=
            split_tabs::SplitTabLayout::kStacked ||
        current->visual_data()->split_ratio() != 0.25 ||
        one->GetContents() != one_contents ||
        two->GetContents() != two_contents ||
        tab_strip->GetActiveTab() != two ||
        workspace_controller->GetWorkspaceIdentifierForTab(one->GetHandle()) !=
            workspace_identifier ||
        workspace_controller->GetWorkspaceIdentifierForTab(two->GetHandle()) !=
            workspace_identifier) {
      return false;
    }
    MultiContentsView* multi_contents = current_view->multi_contents_view();
    views::NativeViewHost* current_host =
        views::AsViewClass<views::NativeViewHost>(
            current_view->limitless_swiftui_rail_for_testing());
    if (!multi_contents || !multi_contents->IsInSplitView() || !current_host ||
        !current_host->native_view() || current_host->width() <= 0 ||
        current_host->height() <= 0 ||
        current_host->native_view().GetNativeNSView().window != native_window ||
        multi_contents->start_contents_view_for_testing()->web_contents() !=
            ordered_tabs[0]->GetContents() ||
        multi_contents->end_contents_view_for_testing()->web_contents() !=
            ordered_tabs[1]->GetContents()) {
      return false;
    }
    const gfx::Rect start_bounds =
        multi_contents->start_contents_view_for_testing()->GetBoundsInScreen();
    const gfx::Rect end_bounds =
        multi_contents->end_contents_view_for_testing()->GetBoundsInScreen();
    return !start_bounds.IsEmpty() && !end_bounds.IsEmpty() &&
           start_bounds.bottom() <= end_bounds.y() &&
           start_bounds.height() < end_bounds.height();
  };

  base::ScopedClosureRunner exit_fullscreen(base::BindLambdaForTesting([&]() {
    BrowserView* current_view =
        BrowserView::GetBrowserViewForBrowser(browser());
    if (current_view && current_view->IsFullscreen()) {
      ui_test_utils::ToggleFullscreenModeAndWait(browser());
    }
  }));
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()));
  ui_test_utils::ToggleFullscreenModeAndWait(browser());
  ASSERT_TRUE(WaitForCondition([&]() { return exact_native_state(true); },
                               base::Seconds(15)));
  rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     active_identifier, "stacked", 0.25));

  ASSERT_TRUE(focus_one());
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        one_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == one && view && view->HasFocus();
  }));
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     ProductIdentifierForTab(browser(), one),
                                     "stacked", 0.25));
  ASSERT_TRUE(FocusKeyEvidenceInput(one_contents));
  SendRendererKey(browser(), ui::VKEY_F, ui::EF_NONE, u'f');
  ASSERT_TRUE(
      WaitForCondition([&]() { return KeyInputValue(one_contents) == "f"; }));
  EXPECT_EQ("b", KeyInputValue(two_contents));

  ASSERT_TRUE(focus_two());
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        two_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == two && view && view->HasFocus();
  }));
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     active_identifier, "stacked", 0.25));
  ui_test_utils::ToggleFullscreenModeAndWait(browser());
  ASSERT_TRUE(WaitForCondition([&]() { return exact_native_state(false); },
                               base::Seconds(15)));
  std::ignore = exit_fullscreen.Release();
  rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     active_identifier, "stacked", 0.25));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        two_contents->GetRenderWidgetHostView();
    return view && view->HasFocus();
  }));
  ASSERT_TRUE(FocusKeyEvidenceInput(two_contents));
  SendRendererKey(browser(), ui::VKEY_X, ui::EF_NONE, u'x');
  ASSERT_TRUE(
      WaitForCondition([&]() { return KeyInputValue(two_contents) == "bx"; }));
  EXPECT_EQ("f", KeyInputValue(one_contents));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    RendererCrashRetainsExactNativeSplitAndReloadRecoversPageInput) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/keys")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/keys"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* healthy = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* crashed = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(healthy);
  ASSERT_TRUE(crashed);
  content::WebContents* healthy_contents = healthy->GetContents();
  content::WebContents* crashed_contents = crashed->GetContents();
  ASSERT_TRUE(healthy_contents->GetPrimaryMainFrame()->IsRenderFrameLive());
  ASSERT_TRUE(crashed_contents->GetPrimaryMainFrame()->IsRenderFrameLive());
  content::RenderProcessHost* healthy_process =
      healthy_contents->GetPrimaryMainFrame()->GetProcess();
  ASSERT_NE(healthy_process,
            crashed_contents->GetPrimaryMainFrame()->GetProcess());
  ASSERT_TRUE(ResetKeyEvidence(healthy_contents, /*focus_input=*/false));
  ASSERT_TRUE(ResetKeyEvidence(crashed_contents, /*focus_input=*/false));

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(WaitForRailTabs(rail, 2));
  ASSERT_TRUE(CreateNativeSplitThroughRail(rail, browser(), healthy, crashed));
  const split_tabs::SplitTabId split_id = *healthy->GetSplit();
  const auto layout_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, "limitless-split-layout", "limitless-split-layout-stacked");
  ASSERT_TRUE(layout_action);
  ASSERT_TRUE(layout_action->press_invoked);
  const auto ratio_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, "limitless-split-ratio", "limitless-split-ratio-25");
  ASSERT_TRUE(ratio_action);
  ASSERT_TRUE(ratio_action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    split_tabs::SplitTabData* current = tab_strip->GetSplitData(split_id);
    return current &&
           current->visual_data()->split_layout() ==
               split_tabs::SplitTabLayout::kStacked &&
           current->visual_data()->split_ratio() == 0.25;
  }));
  split_tabs::SplitTabData* split_data = tab_strip->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  const std::vector<tabs::TabInterface*> ordered_tabs = split_data->ListTabs();
  ASSERT_EQ(2u, ordered_tabs.size());
  const std::string first_identifier =
      ProductIdentifierForTab(browser(), ordered_tabs[0]);
  const std::string second_identifier =
      ProductIdentifierForTab(browser(), ordered_tabs[1]);
  const std::string crashed_identifier =
      ProductIdentifierForTab(browser(), crashed);
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  const std::optional<std::string> workspace_identifier =
      workspace_controller->GetWorkspaceIdentifierForTab(crashed->GetHandle());
  ASSERT_TRUE(workspace_identifier);
  EXPECT_EQ(
      workspace_identifier,
      workspace_controller->GetWorkspaceIdentifierForTab(healthy->GetHandle()));

  const std::string crashed_row =
      "limitless-tab-" + std::to_string(crashed->GetHandle().raw_value());
  const std::string healthy_row =
      "limitless-tab-" + std::to_string(healthy->GetHandle().raw_value());
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, crashed_row,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        crashed_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == crashed && view && view->HasFocus();
  }));
  ASSERT_TRUE(FocusKeyEvidenceInput(crashed_contents));
  SendRendererKey(browser(), ui::VKEY_C, ui::EF_NONE, u'c');
  ASSERT_TRUE(WaitForCondition(
      [&]() { return KeyInputValue(crashed_contents) == "c"; }));
  EXPECT_EQ("", KeyInputValue(healthy_contents));
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()));
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  ASSERT_TRUE(WaitForCondition(
      [&]() { return browser_view->GetWidget()->IsActive(); }));

  {
    content::RenderProcessHost* crash_process =
        crashed_contents->GetPrimaryMainFrame()->GetProcess();
    content::RenderProcessHostWatcher crash_watcher(
        crash_process,
        content::RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
    const bool shutdown_requested =
        crash_process->Shutdown(content::RESULT_CODE_KILLED);
    const bool observed_exit = crash_watcher.Wait();
    ASSERT_TRUE(shutdown_requested);
    ASSERT_TRUE(observed_exit);
  }

  ASSERT_TRUE(WaitForCondition([&]() {
    return crashed_contents->IsCrashed() &&
           !crashed_contents->GetPrimaryMainFrame()->IsRenderFrameLive() &&
           FindSadTabViewForContents(browser_view, crashed_contents);
  }));
  EXPECT_TRUE(healthy_process->IsInitializedAndNotDead());
  EXPECT_TRUE(healthy_contents->GetPrimaryMainFrame()->IsRenderFrameLive());
  EXPECT_EQ(healthy, tab_strip->GetTabAtIndex(0));
  EXPECT_EQ(crashed, tab_strip->GetTabAtIndex(1));
  EXPECT_EQ(healthy_contents, healthy->GetContents());
  EXPECT_EQ(crashed_contents, crashed->GetContents());
  EXPECT_EQ(split_id, healthy->GetSplit());
  EXPECT_EQ(split_id, crashed->GetSplit());
  split_data = tab_strip->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  EXPECT_EQ(ordered_tabs, split_data->ListTabs());
  EXPECT_EQ(split_tabs::SplitTabLayout::kStacked,
            split_data->visual_data()->split_layout());
  EXPECT_EQ(0.25, split_data->visual_data()->split_ratio());
  EXPECT_EQ(
      workspace_identifier,
      workspace_controller->GetWorkspaceIdentifierForTab(healthy->GetHandle()));
  EXPECT_EQ(
      workspace_identifier,
      workspace_controller->GetWorkspaceIdentifierForTab(crashed->GetHandle()));
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     crashed_identifier, "stacked", 0.25));
  MultiContentsView* multi_contents = browser_view->multi_contents_view();
  ASSERT_TRUE(multi_contents);
  EXPECT_TRUE(multi_contents->IsInSplitView());
  EXPECT_EQ(ordered_tabs[0]->GetContents(),
            multi_contents->start_contents_view_for_testing()->web_contents());
  EXPECT_EQ(ordered_tabs[1]->GetContents(),
            multi_contents->end_contents_view_for_testing()->web_contents());
  SadTabHelper* sad_tab_helper = SadTabHelper::From(crashed);
  ASSERT_TRUE(sad_tab_helper);
  ASSERT_TRUE(sad_tab_helper->sad_tab());
  {
    SadTabView* sad_tab_view =
        FindSadTabViewForContents(browser_view, crashed_contents);
    ASSERT_TRUE(sad_tab_view);
    EXPECT_TRUE(sad_tab_view->GetWidget());
    EXPECT_TRUE(sad_tab_view->IsDrawn());
    EXPECT_FALSE(sad_tab_view->GetBoundsInScreen().IsEmpty());
  }

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, healthy_row,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        healthy_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == healthy && view && view->HasFocus();
  }));
  ASSERT_TRUE(FocusKeyEvidenceInput(healthy_contents));
  SendRendererKey(browser(), ui::VKEY_H, ui::EF_NONE, u'h');
  ASSERT_TRUE(WaitForCondition(
      [&]() { return KeyInputValue(healthy_contents) == "h"; }));

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, crashed_row,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    const auto snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    SadTabView* current_sad_tab =
        FindSadTabViewForContents(browser_view, crashed_contents);
    return tab_strip->GetActiveTab() == crashed && snapshot &&
           snapshot->split_active_tab_identifiers ==
               std::vector<std::string>{crashed_identifier} &&
           current_sad_tab && current_sad_tab->IsDrawn();
  }));

  // Chromium's SadTab keyboard-focus acceptance test is disabled on macOS.
  // Keep this product journey scoped to exact rail selection and native SadTab
  // anchoring, then prove page focus and input again after recovery.

  chrome::Reload(browser(), WindowOpenDisposition::CURRENT_TAB);
  ASSERT_TRUE(content::WaitForLoadStop(crashed_contents));
  ASSERT_FALSE(crashed_contents->IsCrashed());
  ASSERT_TRUE(crashed_contents->GetPrimaryMainFrame()->IsRenderFrameLive());
  ASSERT_TRUE(crashed_contents->GetPrimaryMainFrame()
                  ->GetProcess()
                  ->IsInitializedAndNotDead());
  sad_tab_helper = SadTabHelper::From(crashed);
  ASSERT_TRUE(sad_tab_helper);
  EXPECT_EQ(nullptr, sad_tab_helper->sad_tab());
  EXPECT_EQ(nullptr, FindSadTabViewForContents(browser_view, crashed_contents));
  EXPECT_EQ(healthy_contents, healthy->GetContents());
  EXPECT_EQ(crashed_contents, crashed->GetContents());
  EXPECT_EQ(split_id, healthy->GetSplit());
  EXPECT_EQ(split_id, crashed->GetSplit());
  split_data = tab_strip->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  EXPECT_EQ(ordered_tabs, split_data->ListTabs());
  EXPECT_EQ(split_tabs::SplitTabLayout::kStacked,
            split_data->visual_data()->split_layout());
  EXPECT_EQ(0.25, split_data->visual_data()->split_ratio());
  EXPECT_EQ(first_identifier,
            ProductIdentifierForTab(browser(), ordered_tabs[0]));
  EXPECT_EQ(second_identifier,
            ProductIdentifierForTab(browser(), ordered_tabs[1]));
  EXPECT_EQ(
      workspace_identifier,
      workspace_controller->GetWorkspaceIdentifierForTab(healthy->GetHandle()));
  EXPECT_EQ(
      workspace_identifier,
      workspace_controller->GetWorkspaceIdentifierForTab(crashed->GetHandle()));
  EXPECT_EQ(crashed, tab_strip->GetActiveTab());
  ASSERT_TRUE(RailProjectsExactSplit(rail, first_identifier, second_identifier,
                                     crashed_identifier, "stacked", 0.25));
  EXPECT_TRUE(multi_contents->IsInSplitView());
  EXPECT_EQ(ordered_tabs[0]->GetContents(),
            multi_contents->start_contents_view_for_testing()->web_contents());
  EXPECT_EQ(ordered_tabs[1]->GetContents(),
            multi_contents->end_contents_view_for_testing()->web_contents());
  EXPECT_EQ("h", KeyInputValue(healthy_contents));

  ASSERT_TRUE(ResetKeyEvidence(crashed_contents, /*focus_input=*/false));
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, crashed_row,
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* view =
        crashed_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == crashed && view && view->HasFocus();
  }));
  ASSERT_TRUE(FocusKeyEvidenceInput(crashed_contents));
  SendRendererKey(browser(), ui::VKEY_R, ui::EF_NONE, u'r');
  ASSERT_TRUE(WaitForCondition(
      [&]() { return KeyInputValue(crashed_contents) == "r"; }));
  EXPECT_EQ("h", KeyInputValue(healthy_contents));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    ClosingSplitWindowCancelsPendingProjectionAndLeavesPrimarySplitOperable) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* primary_tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* primary_one = primary_tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* primary_two = primary_tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(primary_one);
  ASSERT_TRUE(primary_two);
  content::WebContents* primary_one_contents = primary_one->GetContents();
  content::WebContents* primary_two_contents = primary_two->GetContents();
  views::View* primary_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(primary_rail);
  ASSERT_TRUE(WaitForRailTabs(primary_rail, 2));
  ASSERT_TRUE(CreateNativeSplitThroughRail(primary_rail, browser(), primary_one,
                                           primary_two));
  ASSERT_TRUE(primary_one->GetSplit());
  const split_tabs::SplitTabId primary_split_id = *primary_one->GetSplit();
  split_tabs::SplitTabData* primary_split_data =
      primary_tab_strip->GetSplitData(primary_split_id);
  ASSERT_TRUE(primary_split_data);
  const std::vector<tabs::TabInterface*> primary_order =
      primary_split_data->ListTabs();
  ASSERT_EQ(2u, primary_order.size());
  const std::string primary_first_identifier =
      ProductIdentifierForTab(browser(), primary_order[0]);
  const std::string primary_second_identifier =
      ProductIdentifierForTab(browser(), primary_order[1]);
  const std::string primary_active_identifier =
      ProductIdentifierForTab(browser(), primary_tab_strip->GetActiveTab());
  ASSERT_TRUE(RailProjectsExactSplit(
      primary_rail, primary_first_identifier, primary_second_identifier,
      primary_active_identifier, "sideBySide", 0.5));
  limitless::WorkspaceController* primary_workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(primary_workspace_controller);
  const std::optional<std::string> primary_workspace_identifier =
      primary_workspace_controller->GetWorkspaceIdentifierForTab(
          primary_one->GetHandle());
  ASSERT_TRUE(primary_workspace_identifier);
  ASSERT_EQ(primary_workspace_identifier,
            primary_workspace_controller->GetWorkspaceIdentifierForTab(
                primary_two->GetHandle()));
  const int primary_attached_host_count =
      limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting();
  ASSERT_EQ(1, primary_attached_host_count);

  std::vector<tabs::TabHandle> closing_handles;
  SessionID closing_session_id = SessionID::InvalidValue();
  NSView* __strong retained_native_view = nil;
  int did_close_count = 0;
  {
    Browser* closing_browser = CreateBrowser(browser()->GetProfile());
    ASSERT_TRUE(closing_browser);
    closing_browser->GetWindow()->Show();
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        closing_browser, https_server_.GetURL("c.test", "/three")));
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        closing_browser, https_server_.GetURL("d.test", "/four"),
        WindowOpenDisposition::NEW_BACKGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    TabStripModel* closing_tab_strip = closing_browser->GetTabStripModel();
    tabs::TabInterface* closing_three = closing_tab_strip->GetTabAtIndex(0);
    tabs::TabInterface* closing_four = closing_tab_strip->GetTabAtIndex(1);
    ASSERT_TRUE(closing_three);
    ASSERT_TRUE(closing_four);
    views::View* closing_rail = GetSwiftUITabRail(closing_browser);
    ASSERT_TRUE(closing_rail);
    ASSERT_TRUE(WaitForRailTabs(closing_rail, 2));
    ASSERT_TRUE(CreateNativeSplitThroughRail(closing_rail, closing_browser,
                                             closing_three, closing_four));
    ASSERT_TRUE(closing_three->GetSplit());
    EXPECT_EQ(closing_three->GetSplit(), closing_four->GetSplit());
    closing_handles = {closing_three->GetHandle(), closing_four->GetHandle()};
    closing_session_id = closing_browser->GetSessionID();

    views::NativeViewHost* closing_host =
        views::AsViewClass<views::NativeViewHost>(closing_rail);
    ASSERT_TRUE(closing_host);
    ASSERT_TRUE(WaitForCondition(
        [closing_host]() { return !!closing_host->native_view(); }));
    retained_native_view = closing_host->native_view().GetNativeNSView();
    ASSERT_TRUE(retained_native_view);
    ASSERT_EQ(primary_attached_host_count + 1,
              limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting());

    base::CallbackListSubscription did_close_subscription =
        closing_browser->RegisterBrowserDidClose(base::BindRepeating(
            [](int* count, BrowserWindowInterface* closing_window) {
              ++*count;
              EXPECT_TRUE(closing_window->IsDeleteScheduled());
            },
            &did_close_count));
    ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
        closing_rail, "limitless-split-swap",
        limitless::SwiftUIAccessibilityActionForTesting::kPress));
    CloseBrowserSynchronously(closing_browser);
  }

  EXPECT_EQ(1, did_close_count);
  EXPECT_TRUE(std::ranges::all_of(closing_handles, [](tabs::TabHandle handle) {
    return handle.Get() == nullptr;
  }));
  EXPECT_EQ(nullptr, GlobalBrowserCollection::GetInstance()->FindBrowserWithID(
                         closing_session_id));
  ASSERT_TRUE(WaitForCondition([&]() {
    return limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting() ==
           primary_attached_host_count;
  }));
  EXPECT_EQ(nil, retained_native_view.window);
  EXPECT_EQ(nil, retained_native_view.superview);

  ASSERT_TRUE(primary_tab_strip->ContainsSplit(primary_split_id));
  primary_split_data = primary_tab_strip->GetSplitData(primary_split_id);
  ASSERT_TRUE(primary_split_data);
  EXPECT_EQ(primary_order, primary_split_data->ListTabs());
  EXPECT_EQ(primary_one_contents, primary_one->GetContents());
  EXPECT_EQ(primary_two_contents, primary_two->GetContents());
  EXPECT_EQ(primary_workspace_identifier,
            primary_workspace_controller->GetWorkspaceIdentifierForTab(
                primary_one->GetHandle()));
  EXPECT_EQ(primary_workspace_identifier,
            primary_workspace_controller->GetWorkspaceIdentifierForTab(
                primary_two->GetHandle()));
  ASSERT_TRUE(RailProjectsExactSplit(
      primary_rail, primary_first_identifier, primary_second_identifier,
      primary_active_identifier, "sideBySide", 0.5));

  browser()->GetWindow()->Activate();
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      primary_rail, "limitless-split-swap",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    primary_split_data = primary_tab_strip->GetSplitData(primary_split_id);
    return primary_split_data && primary_split_data->ListTabs() ==
                                     std::vector<tabs::TabInterface*>{
                                         primary_order[1], primary_order[0]};
  }));
  EXPECT_TRUE(RailProjectsExactSplit(
      primary_rail, primary_second_identifier, primary_first_identifier,
      primary_active_identifier, "sideBySide", 0.5));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    WholeNativeSplitTransferPreservesChromiumAuthorityAndDestinationWorkspace) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* source_model = browser()->GetTabStripModel();
  TabListInterface* source_tabs = TabListInterface::From(browser());
  ASSERT_TRUE(source_tabs);
  tabs::TabInterface* one = source_model->GetTabAtIndex(0);
  tabs::TabInterface* two = source_model->GetTabAtIndex(1);
  tabs::TabInterface* guard = source_model->GetTabAtIndex(2);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(guard);
  views::View* source_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(source_rail);
  ASSERT_TRUE(WaitForRailTabs(source_rail, 3));
  ASSERT_TRUE(CreateNativeSplitThroughRail(source_rail, browser(), one, two));
  ASSERT_TRUE(one->GetSplit());
  const split_tabs::SplitTabId split_id = *one->GetSplit();
  split_tabs::SplitTabData* split_data = source_model->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  const std::vector<tabs::TabInterface*> ordered_tabs = split_data->ListTabs();
  ASSERT_EQ(2u, ordered_tabs.size());
  const std::string first_identifier =
      ProductIdentifierForTab(browser(), ordered_tabs[0]);
  const std::string second_identifier =
      ProductIdentifierForTab(browser(), ordered_tabs[1]);
  const tabs::TabHandle first_handle = ordered_tabs[0]->GetHandle();
  const tabs::TabHandle second_handle = ordered_tabs[1]->GetHandle();
  content::WebContents* first_contents = ordered_tabs[0]->GetContents();
  content::WebContents* second_contents = ordered_tabs[1]->GetContents();

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      source_rail, "limitless-tab-" + std::to_string(second_handle.raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition(
      [&]() { return source_model->GetActiveTab() == ordered_tabs[1]; }));
  const auto layout_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          source_rail, "limitless-split-layout",
          "limitless-split-layout-stacked");
  ASSERT_TRUE(layout_action);
  ASSERT_TRUE(layout_action->press_invoked);
  const auto ratio_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          source_rail, "limitless-split-ratio", "limitless-split-ratio-25");
  ASSERT_TRUE(ratio_action);
  ASSERT_TRUE(ratio_action->press_invoked);
  ASSERT_TRUE(RailProjectsExactSplit(source_rail, first_identifier,
                                     second_identifier, second_identifier,
                                     "stacked", 0.25));
  split_data = source_model->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  ASSERT_EQ(split_tabs::SplitTabLayout::kStacked,
            split_data->visual_data()->split_layout());
  ASSERT_EQ(0.25, split_data->visual_data()->split_ratio());

  Browser* destination_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination_browser);
  destination_browser->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      destination_browser, https_server_.GetURL("d.test", "/four")));
  TabStripModel* destination_model = destination_browser->GetTabStripModel();
  views::View* destination_rail = GetSwiftUITabRail(destination_browser);
  ASSERT_TRUE(destination_rail);
  const auto destination_before = WaitForRailTabs(destination_rail, 1);
  ASSERT_TRUE(destination_before);
  ASSERT_EQ(1u, destination_before->workspace_identifiers.size());
  const std::string destination_workspace =
      destination_before->workspace_identifiers[0];

  ASSERT_EQ(browser()->GetProfile(), destination_browser->GetProfile());
  ASSERT_EQ(BrowserWindowInterface::TYPE_NORMAL, browser()->GetType());
  ASSERT_EQ(BrowserWindowInterface::TYPE_NORMAL,
            destination_browser->GetType());
  ASSERT_TRUE(TabListInterface::CanEditTabList(*browser()->GetProfile()));
  std::vector<int> source_indices = {
      source_model->GetIndexOfTab(first_handle.Get()),
      source_model->GetIndexOfTab(second_handle.Get())};
  ASSERT_NE(TabStripModel::kNoTab, source_indices[0]);
  ASSERT_NE(TabStripModel::kNoTab, source_indices[1]);
  std::ranges::sort(source_indices);
  chrome::MoveTabsToExistingWindow(browser(), destination_browser,
                                   source_indices);

  std::optional<limitless::SwiftUITabRailSnapshotForTesting> source_snapshot;
  std::optional<limitless::SwiftUITabRailSnapshotForTesting>
      destination_snapshot;
  ASSERT_TRUE(WaitForCondition([&]() {
    tabs::TabInterface* live_first = first_handle.Get();
    tabs::TabInterface* live_second = second_handle.Get();
    split_tabs::SplitTabData* destination_split =
        destination_model->GetSplitData(split_id);
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    destination_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
    return live_first && live_second &&
           live_first->GetBrowserWindowInterface() == destination_browser &&
           live_second->GetBrowserWindowInterface() == destination_browser &&
           !source_model->ContainsSplit(split_id) && destination_split &&
           destination_split->ListTabs() == ordered_tabs && source_snapshot &&
           source_snapshot->split_pair_identifiers.empty() &&
           destination_snapshot &&
           destination_snapshot->tab_identifiers.size() == 3;
  }));
  split_data = destination_model->GetSplitData(split_id);
  ASSERT_TRUE(split_data);
  EXPECT_EQ(ordered_tabs, split_data->ListTabs());
  EXPECT_EQ(split_tabs::SplitTabLayout::kStacked,
            split_data->visual_data()->split_layout());
  EXPECT_EQ(0.25, split_data->visual_data()->split_ratio());
  EXPECT_EQ(ordered_tabs[1], destination_model->GetActiveTab());
  EXPECT_EQ(first_contents, ordered_tabs[0]->GetContents());
  EXPECT_EQ(second_contents, ordered_tabs[1]->GetContents());
  EXPECT_EQ(first_identifier,
            ProductIdentifierForTab(destination_browser, ordered_tabs[0]));
  EXPECT_EQ(second_identifier,
            ProductIdentifierForTab(destination_browser, ordered_tabs[1]));
  EXPECT_TRUE(RailProjectsExactSplit(destination_rail, first_identifier,
                                     second_identifier, second_identifier,
                                     "stacked", 0.25));
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      source_rail, "limitless-split-swap"));
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(destination_browser));
  ASSERT_TRUE(WaitForCondition([&]() {
    const auto swap = limitless::GetSwiftUIAccessibilityControlForTesting(
        destination_rail, "limitless-split-swap");
    return swap && swap->enabled;
  }));

  limitless::WorkspaceController* source_workspace_controller =
      limitless::WorkspaceController::From(browser());
  limitless::WorkspaceController* destination_workspace_controller =
      limitless::WorkspaceController::From(destination_browser);
  ASSERT_TRUE(source_workspace_controller);
  ASSERT_TRUE(destination_workspace_controller);
  EXPECT_FALSE(
      source_workspace_controller->GetWorkspaceIdentifierForTab(first_handle));
  EXPECT_FALSE(
      source_workspace_controller->GetWorkspaceIdentifierForTab(second_handle));
  EXPECT_EQ(destination_workspace,
            destination_workspace_controller->GetWorkspaceIdentifierForTab(
                first_handle));
  EXPECT_EQ(destination_workspace,
            destination_workspace_controller->GetWorkspaceIdentifierForTab(
                second_handle));
  EXPECT_EQ(guard, source_model->GetActiveTab());

  BrowserView* destination_view =
      BrowserView::GetBrowserViewForBrowser(destination_browser);
  ASSERT_TRUE(destination_view);
  MultiContentsView* destination_multi_contents =
      destination_view->multi_contents_view();
  ASSERT_TRUE(destination_multi_contents);
  ASSERT_TRUE(WaitForCondition([&]() {
    return destination_multi_contents->IsInSplitView() &&
           destination_multi_contents->start_contents_view_for_testing()
                   ->web_contents() == first_contents &&
           destination_multi_contents->end_contents_view_for_testing()
                   ->web_contents() == second_contents;
  }));
  CloseBrowserSynchronously(destination_browser);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    SingleSplitMemberTransferDissolvesPairWithoutDuplicatingPageAuthority) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* source_model = browser()->GetTabStripModel();
  TabListInterface* source_tabs = TabListInterface::From(browser());
  ASSERT_TRUE(source_tabs);
  tabs::TabInterface* one = source_model->GetTabAtIndex(0);
  tabs::TabInterface* two = source_model->GetTabAtIndex(1);
  tabs::TabInterface* guard = source_model->GetTabAtIndex(2);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(guard);
  views::View* source_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(source_rail);
  ASSERT_TRUE(WaitForRailTabs(source_rail, 3));
  ASSERT_TRUE(CreateNativeSplitThroughRail(source_rail, browser(), one, two));
  ASSERT_TRUE(one->GetSplit());
  const split_tabs::SplitTabId former_split_id = *one->GetSplit();
  split_tabs::SplitTabData* split_data =
      source_model->GetSplitData(former_split_id);
  ASSERT_TRUE(split_data);
  const std::vector<tabs::TabInterface*> ordered_tabs = split_data->ListTabs();
  ASSERT_EQ(2u, ordered_tabs.size());
  tabs::TabInterface* survivor = ordered_tabs[0];
  tabs::TabInterface* moved = ordered_tabs[1];
  const tabs::TabHandle survivor_handle = survivor->GetHandle();
  const tabs::TabHandle moved_handle = moved->GetHandle();
  const std::string survivor_identifier =
      ProductIdentifierForTab(browser(), survivor);
  const std::string moved_identifier =
      ProductIdentifierForTab(browser(), moved);
  content::WebContents* survivor_contents = survivor->GetContents();
  content::WebContents* moved_contents = moved->GetContents();
  limitless::WorkspaceController* source_workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(source_workspace_controller);
  const std::optional<std::string> source_workspace =
      source_workspace_controller->GetWorkspaceIdentifierForTab(
          survivor_handle);
  ASSERT_TRUE(source_workspace);

  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      source_rail,
      "limitless-tab-" + std::to_string(survivor_handle.raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition(
      [&]() { return source_model->GetActiveTab() == survivor; }));

  Browser* destination_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination_browser);
  destination_browser->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      destination_browser, https_server_.GetURL("d.test", "/four")));
  TabStripModel* destination_model = destination_browser->GetTabStripModel();
  TabListInterface* destination_tabs =
      TabListInterface::From(destination_browser);
  ASSERT_TRUE(destination_tabs);
  tabs::TabInterface* destination_initial = destination_model->GetActiveTab();
  ASSERT_TRUE(destination_initial);
  const tabs::TabHandle destination_initial_handle =
      destination_initial->GetHandle();
  views::View* destination_rail = GetSwiftUITabRail(destination_browser);
  ASSERT_TRUE(destination_rail);
  const auto destination_before = WaitForRailTabs(destination_rail, 1);
  ASSERT_TRUE(destination_before);
  const std::string destination_workspace =
      destination_before->workspace_identifiers[0];

  ASSERT_EQ(browser()->GetProfile(), destination_browser->GetProfile());
  ASSERT_TRUE(TabListInterface::CanEditTabList(*browser()->GetProfile()));
  ASSERT_EQ(browser(), moved->GetBrowserWindowInterface());
  source_tabs->MoveTabToWindow(moved_handle,
                               destination_browser->GetSessionID(), 1);

  std::optional<limitless::SwiftUITabRailSnapshotForTesting> source_snapshot;
  std::optional<limitless::SwiftUITabRailSnapshotForTesting>
      destination_snapshot;
  ASSERT_TRUE(WaitForCondition([&]() {
    tabs::TabInterface* live_survivor = survivor_handle.Get();
    tabs::TabInterface* live_moved = moved_handle.Get();
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    destination_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
    return live_survivor && live_moved &&
           live_survivor->GetBrowserWindowInterface() == browser() &&
           live_moved->GetBrowserWindowInterface() == destination_browser &&
           !live_survivor->IsSplit() && !live_moved->IsSplit() &&
           !source_model->ContainsSplit(former_split_id) &&
           !destination_model->ContainsSplit(former_split_id) &&
           source_snapshot && source_snapshot->split_pair_identifiers.empty() &&
           source_snapshot->tab_identifiers.size() == 2 &&
           destination_snapshot &&
           destination_snapshot->split_pair_identifiers.empty() &&
           destination_snapshot->tab_identifiers.size() == 2;
  }));
  EXPECT_EQ(survivor, source_model->GetActiveTab());
  EXPECT_EQ(destination_initial, destination_model->GetActiveTab());
  EXPECT_EQ(survivor_contents, survivor->GetContents());
  EXPECT_EQ(moved_contents, moved->GetContents());
  EXPECT_EQ(survivor_identifier, ProductIdentifierForTab(browser(), survivor));
  EXPECT_EQ(moved_identifier,
            ProductIdentifierForTab(destination_browser, moved));
  EXPECT_EQ(source_workspace,
            source_workspace_controller->GetWorkspaceIdentifierForTab(
                survivor_handle));
  EXPECT_FALSE(
      source_workspace_controller->GetWorkspaceIdentifierForTab(moved_handle));
  limitless::WorkspaceController* destination_workspace_controller =
      limitless::WorkspaceController::From(destination_browser);
  ASSERT_TRUE(destination_workspace_controller);
  EXPECT_EQ(destination_workspace,
            destination_workspace_controller->GetWorkspaceIdentifierForTab(
                moved_handle));
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      source_rail, "limitless-split-swap"));
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      destination_rail, "limitless-split-swap"));
  EXPECT_TRUE(limitless::GetSwiftUIAccessibilityControlForTesting(
      source_rail,
      "limitless-tab-" + std::to_string(survivor_handle.raw_value())));
  EXPECT_TRUE(limitless::GetSwiftUIAccessibilityControlForTesting(
      destination_rail,
      "limitless-tab-" + std::to_string(moved_handle.raw_value())));
  EXPECT_TRUE(source_model->GetIndexOfTab(guard) != TabStripModel::kNoTab);
  EXPECT_TRUE(destination_model->GetIndexOfTab(
                  destination_initial_handle.Get()) != TabStripModel::kNoTab);
  CloseBrowserSynchronously(destination_browser);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    PrivateAndPopupWindowsRejectProductSplitMutationAndKeepRegularPairExact) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* regular_model = browser()->GetTabStripModel();
  tabs::TabInterface* regular_one = regular_model->GetTabAtIndex(0);
  tabs::TabInterface* regular_two = regular_model->GetTabAtIndex(1);
  ASSERT_TRUE(regular_one);
  ASSERT_TRUE(regular_two);
  views::View* regular_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(regular_rail);
  ASSERT_TRUE(WaitForRailTabs(regular_rail, 2));
  ASSERT_TRUE(CreateNativeSplitThroughRail(regular_rail, browser(), regular_one,
                                           regular_two));
  ASSERT_TRUE(regular_one->GetSplit());
  const split_tabs::SplitTabId regular_split_id = *regular_one->GetSplit();
  split_tabs::SplitTabData* regular_split_data =
      regular_model->GetSplitData(regular_split_id);
  ASSERT_TRUE(regular_split_data);
  const std::vector<tabs::TabInterface*> regular_order =
      regular_split_data->ListTabs();
  ASSERT_EQ(2u, regular_order.size());

  Browser* private_browser = CreateIncognitoBrowser();
  ASSERT_TRUE(private_browser);
  private_browser->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      private_browser, https_server_.GetURL("c.test", "/three")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      private_browser, https_server_.GetURL("d.test", "/four"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(private_browser->GetProfile()->IsOffTheRecord());
  ASSERT_NE(browser()->GetProfile(), private_browser->GetProfile());
  views::View* private_rail = GetSwiftUITabRail(private_browser);
  ASSERT_TRUE(private_rail);
  auto private_snapshot = WaitForRailTabs(private_rail, 2);
  ASSERT_TRUE(private_snapshot);
  ASSERT_EQ(2u, private_snapshot->tab_product_identifiers.size());
  EXPECT_FALSE(private_snapshot->split_actions_available);
  EXPECT_TRUE(private_snapshot->split_pair_identifiers.empty());
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      private_rail, "limitless-split-controls"));

  TabListInterface* private_tabs = TabListInterface::From(private_browser);
  ASSERT_TRUE(private_tabs);
  tabs::TabInterface* private_three = private_tabs->GetTab(0);
  tabs::TabInterface* private_four = private_tabs->GetTab(1);
  ASSERT_TRUE(private_three);
  ASSERT_TRUE(private_four);
  limitless::SplitPaneController* private_split_controller =
      limitless::SplitPaneController::From(private_browser);
  ASSERT_TRUE(private_split_controller);
  EXPECT_EQ(limitless::SplitPaneOperationStatus::kPrivateProfileUnsupported,
            private_split_controller->GetSnapshot().availability_status);
  EXPECT_EQ(limitless::SplitPaneOperationStatus::kPrivateProfileUnsupported,
            private_split_controller
                ->CreateSplit(private_snapshot->window_identifier,
                              private_snapshot->tab_product_identifiers[0],
                              private_snapshot->tab_product_identifiers[1])
                .status);
  EXPECT_FALSE(private_three->IsSplit());
  EXPECT_FALSE(private_four->IsSplit());
  limitless::WorkspaceController* private_workspace_controller =
      limitless::WorkspaceController::From(private_browser);
  ASSERT_TRUE(private_workspace_controller);
  EXPECT_FALSE(private_workspace_controller->SerializeWindowForSession());
  EXPECT_FALSE(private_workspace_controller->SerializeTabForSession(
      private_three->GetHandle()));

  const std::optional<split_tabs::SplitTabId> private_native_split =
      private_tabs->CreateSplit(
          {private_three->GetHandle(), private_four->GetHandle()});
  ASSERT_TRUE(private_native_split);
  ASSERT_TRUE(WaitForCondition([&]() {
    private_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(private_rail);
    const split_tabs::SplitTabData* native_data =
        private_browser->GetTabStripModel()->GetSplitData(
            *private_native_split);
    return native_data && native_data->ListTabs().size() == 2 &&
           private_snapshot && !private_snapshot->split_actions_available &&
           private_snapshot->split_pair_identifiers.empty();
  }));
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      private_rail, "limitless-split-controls"));
  EXPECT_TRUE(limitless::GetSwiftUIAccessibilityControlForTesting(
      private_rail,
      "limitless-tab-" +
          std::to_string(private_three->GetHandle().raw_value())));
  CloseBrowserSynchronously(private_browser);

  const int host_count_before_popup =
      limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting();
  BrowserWindowCreateParams popup_params(BrowserWindowInterface::TYPE_POPUP,
                                         browser()->GetProfile(),
                                         /*from_user_gesture=*/true);
  popup_params.omit_from_session_restore = true;
  BrowserWindowInterface* popup_window =
      CreateBrowserWindow(std::move(popup_params));
  ASSERT_TRUE(popup_window);
  Browser* popup_browser = popup_window->GetBrowserForMigrationOnly();
  ASSERT_TRUE(popup_browser);
  chrome::AddTabAt(popup_window, GURL("about:blank"), -1,
                   /*foreground=*/true);
  popup_window->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      popup_browser, https_server_.GetURL("c.test", "/three")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      popup_browser, https_server_.GetURL("d.test", "/four"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  EXPECT_EQ(nullptr, GetSwiftUITabRail(popup_browser));
  EXPECT_EQ(nullptr, limitless::WorkspaceController::From(popup_window));
  EXPECT_EQ(nullptr, limitless::SplitPaneController::From(popup_window));
  EXPECT_EQ(host_count_before_popup,
            limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting());
  EXPECT_FALSE(limitless::MaybeCreateSwiftUITabRail(popup_window));

  regular_split_data = regular_model->GetSplitData(regular_split_id);
  ASSERT_TRUE(regular_split_data);
  EXPECT_EQ(regular_order, regular_split_data->ListTabs());
  EXPECT_EQ(regular_split_id, regular_one->GetSplit());
  EXPECT_EQ(regular_split_id, regular_two->GetSplit());
  CloseBrowserSynchronously(popup_browser);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    MissingProductIdentityMappingKeepsRowsAndClearsSplitProjectionAndControls) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  const auto before = WaitForRailTabs(rail, 2);
  ASSERT_TRUE(before);
  ASSERT_EQ(2u, before->tab_product_identifiers.size());
  const std::vector<int32_t> handles = before->tab_identifiers;
  const std::vector<std::u16string> titles = before->tab_titles;
  const std::string split_menu =
      "limitless-tab-split-" + before->tab_product_identifiers[0];
  const auto split_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, split_menu,
          split_menu + "-with-" + before->tab_product_identifiers[1]);
  ASSERT_TRUE(split_action);
  ASSERT_TRUE(split_action->press_invoked);
  tabs::TabInterface* first = browser()->GetTabStripModel()->GetTabAtIndex(0);
  tabs::TabInterface* second = browser()->GetTabStripModel()->GetTabAtIndex(1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(WaitForCondition([&]() {
    return first->GetSplit().has_value() &&
           first->GetSplit() == second->GetSplit();
  }));
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> split_snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    split_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return split_snapshot && split_snapshot->split_pair_identifiers.size() == 1;
  }));
  ASSERT_TRUE(limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-split-unsplit"));
  const split_tabs::SplitTabId authoritative_split = *first->GetSplit();

  ASSERT_TRUE(
      limitless::SimulateMissingSwiftUIProductIdentityMappingForTesting(rail));
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> after;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    after = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return after && after->tab_identifiers == handles &&
           after->tab_titles == titles &&
           after->tab_product_identifiers.empty();
  }));
  EXPECT_TRUE(after->workspaces_enabled);
  EXPECT_FALSE(after->split_actions_available);
  EXPECT_TRUE(after->split_pair_identifiers.empty());
  EXPECT_TRUE(after->split_first_tab_identifiers.empty());
  EXPECT_TRUE(after->split_second_tab_identifiers.empty());
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-split-unsplit"));
  EXPECT_FALSE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-split-unsplit",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  EXPECT_EQ(authoritative_split, first->GetSplit());
  EXPECT_EQ(authoritative_split, second->GetSplit());
  EXPECT_TRUE(
      browser()->GetTabStripModel()->ContainsSplit(authoritative_split));

  const auto inventory =
      limitless::GetSwiftUIAccessibilityInventoryForTesting(rail);
  EXPECT_FALSE(inventory.hit_node_cap);
  for (const std::string& identifier : inventory.identifiers) {
    EXPECT_FALSE(base::StartsWith(identifier, "limitless-tab-split-"));
    EXPECT_EQ(std::string::npos, identifier.find("transient-"));
    for (const std::string& product_identifier :
         before->tab_product_identifiers) {
      EXPECT_EQ(std::string::npos, identifier.find(product_identifier));
    }
  }
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    LaterNativeRowCanSplitWithEarlierRowInAuthoritativeOrder) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* first = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* second = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  const auto before = WaitForRailTabs(rail, 2);
  ASSERT_TRUE(before);
  ASSERT_EQ(2u, before->tab_product_identifiers.size());
  const std::string menu_identifier =
      "limitless-tab-split-" + before->tab_product_identifiers[1];
  const auto action = limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
      rail, menu_identifier,
      menu_identifier + "-with-" + before->tab_product_identifiers[0]);
  ASSERT_TRUE(action);
  EXPECT_TRUE(action->destination.enabled);
  EXPECT_TRUE(action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    return first->GetSplit().has_value() &&
           first->GetSplit() == second->GetSplit();
  }));
  const split_tabs::SplitTabData* split_data =
      tab_strip->GetSplitData(*first->GetSplit());
  ASSERT_TRUE(split_data);
  EXPECT_EQ((std::vector<tabs::TabInterface*>{first, second}),
            split_data->ListTabs());
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> after;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    after = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return after && after->split_pair_identifiers.size() == 1;
  }));
  EXPECT_EQ((std::vector<std::string>{before->tab_product_identifiers[0]}),
            after->split_first_tab_identifiers);
  EXPECT_EQ((std::vector<std::string>{before->tab_product_identifiers[1]}),
            after->split_second_tab_identifiers);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    BackgroundSplitMembersHaveTruthfulNativeAccessibilityState) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* first = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* second = tab_strip->GetTabAtIndex(1);
  tabs::TabInterface* active = tab_strip->GetTabAtIndex(2);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(active);
  ASSERT_EQ(active, tab_strip->GetActiveTab());

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  const auto before = WaitForRailTabs(rail, 3);
  ASSERT_TRUE(before);
  ASSERT_EQ(3u, before->tab_product_identifiers.size());
  const std::string menu_identifier =
      "limitless-tab-split-" + before->tab_product_identifiers[0];
  const auto action = limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
      rail, menu_identifier,
      menu_identifier + "-with-" + before->tab_product_identifiers[1]);
  ASSERT_TRUE(action);
  ASSERT_TRUE(action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    return first->GetSplit().has_value() &&
           first->GetSplit() == second->GetSplit();
  }));
  ASSERT_EQ(active, tab_strip->GetActiveTab());
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> after;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    after = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return after && after->split_pair_identifiers.size() == 1;
  }));
  EXPECT_EQ((std::vector<std::string>{""}),
            after->split_active_tab_identifiers);
  EXPECT_FALSE(limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-split-controls"));

  const auto first_control =
      limitless::GetSwiftUIAccessibilityControlForTesting(
          rail,
          "limitless-tab-" + std::to_string(first->GetHandle().raw_value()));
  const auto second_control =
      limitless::GetSwiftUIAccessibilityControlForTesting(
          rail,
          "limitless-tab-" + std::to_string(second->GetHandle().raw_value()));
  ASSERT_TRUE(first_control);
  ASSERT_TRUE(second_control);
  EXPECT_EQ(u"Background split member, first pane, Rail One",
            first_control->label);
  EXPECT_EQ(u"Background split member", first_control->value);
  EXPECT_EQ(u"Background split member, second pane, Rail Two",
            second_control->label);
  EXPECT_EQ(u"Background split member", second_control->value);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    ReadOnlySplitRemainsProjectedWhileNativeActionsAreDisabled) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* first = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* second = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  const auto before = WaitForRailTabs(rail, 2);
  ASSERT_TRUE(before);
  const std::string menu_identifier =
      "limitless-tab-split-" + before->tab_product_identifiers[0];
  const auto action = limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
      rail, menu_identifier,
      menu_identifier + "-with-" + before->tab_product_identifiers[1]);
  ASSERT_TRUE(action);
  ASSERT_TRUE(action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    return first->GetSplit().has_value() &&
           first->GetSplit() == second->GetSplit();
  }));
  const split_tabs::SplitTabId authoritative_split = *first->GetSplit();

  BrowserWindow::FromBrowser(browser())->DisableTabStripEditingForTesting();
  tab_strip->NotifyTabStripEditabilityChanged(false);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> read_only;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    read_only = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return read_only && !read_only->split_actions_available &&
           read_only->split_pair_identifiers.size() == 1;
  }));
  EXPECT_EQ(authoritative_split, first->GetSplit());
  EXPECT_EQ(authoritative_split, second->GetSplit());
  const auto controls = limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-split-controls");
  const auto unsplit = limitless::GetSwiftUIAccessibilityControlForTesting(
      rail, "limitless-split-unsplit");
  ASSERT_TRUE(controls);
  ASSERT_TRUE(unsplit);
  EXPECT_FALSE(controls->enabled);
  EXPECT_EQ(u"Actions unavailable", controls->value);
  EXPECT_FALSE(unsplit->enabled);
  EXPECT_FALSE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-split-unsplit",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  EXPECT_EQ(authoritative_split, first->GetSplit());
  EXPECT_EQ(authoritative_split, second->GetSplit());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    WorkspaceSwitchPreservesPairAndMovingMemberSafelyUnsplits) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* first = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* second = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  const auto initial = WaitForRailTabs(rail, 2);
  ASSERT_TRUE(initial);
  ASSERT_TRUE(initial->active_workspace_identifier);
  const std::string default_workspace = *initial->active_workspace_identifier;
  const std::string split_menu =
      "limitless-tab-split-" + initial->tab_product_identifiers[0];
  const auto split_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, split_menu,
          split_menu + "-with-" + initial->tab_product_identifiers[1]);
  ASSERT_TRUE(split_action);
  ASSERT_TRUE(split_action->press_invoked);
  ASSERT_TRUE(WaitForCondition([&]() {
    return first->GetSplit().has_value() &&
           first->GetSplit() == second->GetSplit();
  }));
  const split_tabs::SplitTabId authoritative_split = *first->GetSplit();

  const std::optional<std::string> other_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(rail, u"Other");
  ASSERT_TRUE(other_workspace);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot &&
           snapshot->active_workspace_identifier == other_workspace &&
           snapshot->split_pair_identifiers.empty();
  }));
  EXPECT_EQ(authoritative_split, first->GetSplit());
  EXPECT_EQ(authoritative_split, second->GetSplit());

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> restored_pair;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    restored_pair = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return restored_pair &&
           restored_pair->active_workspace_identifier == default_workspace &&
           restored_pair->split_pair_identifiers.size() == 1;
  }));
  EXPECT_EQ(authoritative_split, first->GetSplit());
  EXPECT_EQ(authoritative_split, second->GetSplit());

  const std::string move_control =
      "limitless-tab-move-" + std::to_string(second->GetHandle().raw_value());
  const std::string move_destination = move_control + "-to-" + *other_workspace;
  const auto move_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, move_control, move_destination);
  ASSERT_TRUE(move_action);
  EXPECT_TRUE(move_action->destination.enabled);
  EXPECT_TRUE(move_action->press_invoked);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && !first->IsSplit() && !second->IsSplit() &&
           snapshot->split_pair_identifiers.empty() &&
           std::ranges::find(snapshot->tab_identifiers,
                             second->GetHandle().raw_value()) ==
               snapshot->tab_identifiers.end();
  }));
  EXPECT_EQ(first->GetContents()->GetVisibleURL(),
            https_server_.GetURL("a.test", "/one"));
  EXPECT_EQ(second->GetContents()->GetVisibleURL(),
            https_server_.GetURL("b.test", "/two"));

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *other_workspace));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot &&
           snapshot->active_workspace_identifier == other_workspace &&
           std::ranges::find(snapshot->tab_identifiers,
                             second->GetHandle().raw_value()) !=
               snapshot->tab_identifiers.end();
  }));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    SplitMenuTruthfullyDisablesPinnedAndGroupedDestinations) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* source = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* pinned = tab_strip->GetTabAtIndex(1);
  tabs::TabInterface* grouped = tab_strip->GetTabAtIndex(2);
  ASSERT_TRUE(source);
  ASSERT_TRUE(pinned);
  ASSERT_TRUE(grouped);
  tab_strip->SetTabPinned(tab_strip->GetIndexOfTab(pinned), true);
  tab_strip->AddToNewGroup({tab_strip->GetIndexOfTab(grouped)});

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(WaitForRailTabs(rail, 3));
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  const limitless::WorkspaceWindowSnapshot workspace =
      workspace_controller->GetSnapshot();
  const auto product_identifier_for = [&](tabs::TabInterface* tab) {
    const auto found =
        std::ranges::find(workspace.visible_tab_projections, tab->GetHandle(),
                          &limitless::WorkspaceTabProjection::handle);
    CHECK(found != workspace.visible_tab_projections.end());
    return found->identifier;
  };
  const std::string source_identifier = product_identifier_for(source);
  const std::string pinned_identifier = product_identifier_for(pinned);
  const std::string grouped_identifier = product_identifier_for(grouped);
  const std::string menu_identifier =
      "limitless-tab-split-" + source_identifier;

  const auto before = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
  ASSERT_TRUE(before);
  EXPECT_EQ(0u, before->split_eligibility_batch_invocation_count);
  const auto pinned_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, menu_identifier,
          menu_identifier + "-with-" + pinned_identifier);
  ASSERT_TRUE(pinned_action);
  EXPECT_FALSE(pinned_action->destination.enabled);
  EXPECT_EQ(u"Cannot split with Rail Two - pin state differs",
            pinned_action->destination.label);
  EXPECT_FALSE(source->IsSplit());
  EXPECT_FALSE(pinned->IsSplit());
  const auto after_pinned =
      limitless::GetSwiftUITabRailSnapshotForTesting(rail);
  ASSERT_TRUE(after_pinned);
  EXPECT_GT(after_pinned->split_eligibility_batch_invocation_count, 0u);
  EXPECT_LE(after_pinned->split_eligibility_batch_invocation_count, 4u);

  const auto grouped_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, menu_identifier,
          menu_identifier + "-with-" + grouped_identifier);
  ASSERT_TRUE(grouped_action);
  EXPECT_FALSE(grouped_action->destination.enabled);
  EXPECT_EQ(u"Cannot split with Rail Three - tab group differs",
            grouped_action->destination.label);
  EXPECT_FALSE(source->IsSplit());
  EXPECT_FALSE(grouped->IsSplit());
  const auto after = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
  ASSERT_TRUE(after);
  EXPECT_GT(after->split_eligibility_batch_invocation_count,
            after_pinned->split_eligibility_batch_invocation_count);
  EXPECT_LE(after->split_eligibility_batch_invocation_count -
                after_pinned->split_eligibility_batch_invocation_count,
            4u);
  EXPECT_TRUE(after->split_pair_identifiers.empty());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       CreatesSwitchesAndMovesRealWorkspaceTabs) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(3, tab_strip->count());
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  tabs::TabInterface* three = tab_strip->GetTabAtIndex(2);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  const tabs::TabHandle one_handle = one->GetHandle();
  const tabs::TabHandle two_handle = two->GetHandle();
  const tabs::TabHandle three_handle = three->GetHandle();
  content::WebContents* one_contents = one->GetContents();
  content::WebContents* two_contents = two->GetContents();
  content::WebContents* three_contents = three->GetContents();

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  auto snapshot = WaitForRailTabs(rail, 3);
  ASSERT_TRUE(snapshot);
  ASSERT_TRUE(snapshot->workspaces_enabled);
  ASSERT_FALSE(snapshot->window_identifier.empty());
  ASSERT_EQ(1u, snapshot->workspace_identifiers.size());
  ASSERT_EQ((std::vector<std::u16string>{u"Workspace"}),
            snapshot->workspace_names);
  ASSERT_EQ((std::vector<bool>{true}), snapshot->default_workspaces);
  ASSERT_TRUE(snapshot->active_workspace_identifier);
  const std::string default_workspace = snapshot->workspace_identifiers[0];
  EXPECT_EQ(default_workspace, *snapshot->active_workspace_identifier);
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  const limitless::WorkspaceWindowSnapshot product_snapshot =
      workspace_controller->GetSnapshot();
  ASSERT_EQ(3u, product_snapshot.visible_tab_projections.size());
  const std::string one_product_identifier =
      product_snapshot.visible_tab_projections[0].identifier;
  const std::string two_product_identifier =
      product_snapshot.visible_tab_projections[1].identifier;
  const std::string three_product_identifier =
      product_snapshot.visible_tab_projections[2].identifier;

  const std::optional<std::string> focus_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(rail, u"Focus");
  ASSERT_TRUE(focus_workspace);
  ASSERT_NE(default_workspace, *focus_workspace);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->count() == 4 && snapshot &&
           snapshot->active_workspace_identifier == focus_workspace &&
           snapshot->tab_identifiers.size() == 1;
  }));
  tabs::TabInterface* focus_new_tab = tab_strip->GetActiveTab();
  ASSERT_TRUE(focus_new_tab);
  const tabs::TabHandle focus_new_tab_handle = focus_new_tab->GetHandle();
  content::WebContents* focus_new_tab_contents = focus_new_tab->GetContents();
  EXPECT_EQ(focus_new_tab_handle.raw_value(), snapshot->tab_identifiers[0]);
  EXPECT_EQ(focus_new_tab_handle.raw_value(), snapshot->active_tab_identifier);
  EXPECT_TRUE(focus_new_tab_contents->GetVisibleURL().SchemeIs("chrome"));

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->GetActiveTab() == one && snapshot &&
           snapshot->active_workspace_identifier == default_workspace &&
           snapshot->tab_identifiers ==
               std::vector<int32_t>{one_handle.raw_value(),
                                    two_handle.raw_value(),
                                    three_handle.raw_value()};
  }));

  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      rail, two_handle.raw_value(), *focus_workspace));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers ==
                           std::vector<int32_t>{one_handle.raw_value(),
                                                three_handle.raw_value()};
  }));
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *focus_workspace));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->GetActiveTab() == focus_new_tab && snapshot &&
           snapshot->tab_identifiers ==
               std::vector<int32_t>{two_handle.raw_value(),
                                    focus_new_tab_handle.raw_value()};
  }));

  EXPECT_EQ(limitless::WorkspaceOperationStatus::kForeignWindow,
            workspace_controller
                ->ActivateTabByIdentifier(
                    base::Uuid::GenerateRandomV4().AsLowercaseString(),
                    two_product_identifier)
                .status);
  EXPECT_EQ(limitless::WorkspaceOperationStatus::kForeignWindow,
            workspace_controller
                ->ActivateTabByIdentifier(product_snapshot.window_identifier,
                                          one_product_identifier)
                .status);
  EXPECT_TRUE(workspace_controller
                  ->ActivateTabByIdentifier(product_snapshot.window_identifier,
                                            two_product_identifier)
                  .succeeded());
  ASSERT_TRUE(
      WaitForCondition([&]() { return tab_strip->GetActiveTab() == two; }));
  EXPECT_TRUE(workspace_controller
                  ->MoveTabToWorkspaceByIdentifier(
                      product_snapshot.window_identifier,
                      three_product_identifier, *focus_workspace)
                  .succeeded());
  EXPECT_TRUE(workspace_controller
                  ->MoveTabToWorkspaceByIdentifier(
                      product_snapshot.window_identifier,
                      three_product_identifier, default_workspace)
                  .succeeded());
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(
      WaitForCondition([&]() { return tab_strip->GetActiveTab() == one; }));
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *focus_workspace));
  ASSERT_TRUE(
      WaitForCondition([&]() { return tab_strip->GetActiveTab() == two; }));

  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(two_contents, two->GetContents());
  EXPECT_EQ(three_contents, three->GetContents());
  EXPECT_EQ(focus_new_tab_contents, focus_new_tab->GetContents());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       ReconcilesRealTabLifecycleAndWindowTransfer) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabListInterface* source_tabs = TabListInterface::From(browser());
  ASSERT_TRUE(source_tabs);
  tabs::TabInterface* one = source_tabs->GetTab(0);
  tabs::TabInterface* two = source_tabs->GetTab(1);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  const tabs::TabHandle two_handle = two->GetHandle();
  content::WebContents* one_contents = one->GetContents();

  views::View* source_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(source_rail);
  auto source_snapshot = WaitForRailTabs(source_rail, 2);
  ASSERT_TRUE(source_snapshot);
  const std::string default_workspace =
      source_snapshot->workspace_identifiers[0];
  const std::optional<std::string> focus_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(source_rail, u"Focus");
  ASSERT_TRUE(focus_workspace);
  tabs::TabInterface* focus_new_tab = source_tabs->GetActiveTab();
  ASSERT_TRUE(focus_new_tab);
  const tabs::TabHandle focus_new_tab_handle = focus_new_tab->GetHandle();
  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(source_rail,
                                                          default_workspace));
  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      source_rail, two_handle.raw_value(), *focus_workspace));
  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(source_rail,
                                                          *focus_workspace));

  limitless::WorkspaceController* source_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(source_controller);
  const limitless::WorkspaceWindowSnapshot source_product_snapshot =
      source_controller->GetSnapshot();
  const auto source_two_projection =
      std::ranges::find(source_product_snapshot.visible_tab_projections,
                        two_handle, &limitless::WorkspaceTabProjection::handle);
  ASSERT_NE(source_product_snapshot.visible_tab_projections.end(),
            source_two_projection);
  const std::string two_product_identifier_before_lifecycle =
      source_two_projection->identifier;

  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  tabs::TabInterface* three =
      source_tabs->GetTab(source_tabs->GetTabCount() - 1);
  ASSERT_TRUE(three);
  const tabs::TabHandle three_handle = three->GetHandle();
  content::WebContents* three_contents = three->GetContents();
  ASSERT_TRUE(base::test::RunUntil([&]() {
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    return source_snapshot &&
           source_snapshot->tab_identifiers ==
               std::vector<int32_t>{two_handle.raw_value(),
                                    focus_new_tab_handle.raw_value(),
                                    three_handle.raw_value()};
  }));

  source_tabs->MoveTab(three_handle, 0);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    return source_snapshot &&
           source_snapshot->tab_identifiers ==
               std::vector<int32_t>{three_handle.raw_value(),
                                    two_handle.raw_value(),
                                    focus_new_tab_handle.raw_value()};
  }));
  EXPECT_EQ(one_contents, one->GetContents());
  EXPECT_EQ(three_contents, three->GetContents());

  content::WebContents* two_contents_after_discard =
      source_tabs->DiscardTab(two_handle);
  ASSERT_TRUE(two_contents_after_discard);
  EXPECT_EQ(two_contents_after_discard, two->GetContents());
  EXPECT_EQ(two_handle, two->GetHandle());
  ASSERT_TRUE(source_controller->SerializeTabForSession(two_handle));
  EXPECT_EQ(two_product_identifier_before_lifecycle,
            ReadWorkspaceTabSessionData(
                *source_controller->SerializeTabForSession(two_handle))
                .first);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    return source_snapshot &&
           std::ranges::find(source_snapshot->tab_identifiers,
                             two_handle.raw_value()) !=
               source_snapshot->tab_identifiers.end();
  }));

  auto replacement_contents = content::WebContents::Create(
      content::WebContents::CreateParams(browser()->GetProfile()));
  content::WebContents* replacement_contents_pointer =
      replacement_contents.get();
  std::unique_ptr<content::WebContents> replaced_contents =
      browser()->GetTabStripModel()->DiscardWebContents(
          two->GetContents(), std::move(replacement_contents));
  EXPECT_EQ(two_contents_after_discard, replaced_contents.get());
  EXPECT_EQ(replacement_contents_pointer, two->GetContents());
  EXPECT_EQ(two_handle, two->GetHandle());
  ASSERT_TRUE(source_controller->SerializeTabForSession(two_handle));
  EXPECT_EQ(two_product_identifier_before_lifecycle,
            ReadWorkspaceTabSessionData(
                *source_controller->SerializeTabForSession(two_handle))
                .first);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    return source_snapshot &&
           std::ranges::find(source_snapshot->tab_identifiers,
                             two_handle.raw_value()) !=
               source_snapshot->tab_identifiers.end();
  }));

  Browser* destination_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination_browser);
  destination_browser->GetWindow()->Show();
  views::View* destination_rail = GetSwiftUITabRail(destination_browser);
  ASSERT_TRUE(destination_rail);
  auto destination_snapshot = WaitForRailTabs(destination_rail, 1);
  ASSERT_TRUE(destination_snapshot);
  const std::string destination_workspace =
      destination_snapshot->workspace_identifiers[0];
  tabs::TabInterface* destination_initial =
      destination_browser->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(destination_initial);
  const tabs::TabHandle destination_initial_handle =
      destination_initial->GetHandle();

  source_tabs->MoveTabToWindow(two_handle, destination_browser->GetSessionID(),
                               1);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    source_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
    destination_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
    return source_snapshot && destination_snapshot &&
           source_snapshot->tab_identifiers ==
               std::vector<int32_t>{three_handle.raw_value(),
                                    focus_new_tab_handle.raw_value()} &&
           destination_snapshot->tab_identifiers ==
               std::vector<int32_t>{destination_initial_handle.raw_value(),
                                    two_handle.raw_value()};
  }));
  EXPECT_FALSE(limitless::ActivateSwiftUITabForTesting(source_rail,
                                                       two_handle.raw_value()));
  EXPECT_FALSE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      source_rail, two_handle.raw_value(), *focus_workspace));
  EXPECT_FALSE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      destination_rail, two_handle.raw_value(), *focus_workspace));
  EXPECT_TRUE(limitless::ActivateSwiftUITabForTesting(destination_rail,
                                                      two_handle.raw_value()));
  EXPECT_EQ(replacement_contents_pointer, two_handle.Get()->GetContents());

  limitless::WorkspaceController* destination_controller =
      limitless::WorkspaceController::From(destination_browser);
  ASSERT_TRUE(destination_controller);
  const limitless::WorkspaceWindowSnapshot destination_product_snapshot =
      destination_controller->GetSnapshot();
  const auto transferred_projection =
      std::ranges::find(destination_product_snapshot.visible_tab_projections,
                        two_handle, &limitless::WorkspaceTabProjection::handle);
  ASSERT_NE(destination_product_snapshot.visible_tab_projections.end(),
            transferred_projection);
  const std::string transferred_product_identifier =
      transferred_projection->identifier;
  EXPECT_EQ(two_product_identifier_before_lifecycle,
            transferred_product_identifier);

  EXPECT_TRUE(limitless::CloseSwiftUITabForTesting(destination_rail,
                                                   two_handle.raw_value()));
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return two_handle.Get() == nullptr; }));
  EXPECT_FALSE(limitless::ActivateSwiftUITabForTesting(destination_rail,
                                                       two_handle.raw_value()));
  EXPECT_EQ(limitless::WorkspaceOperationStatus::kStaleTab,
            destination_controller
                ->ActivateTabByIdentifier(
                    destination_product_snapshot.window_identifier,
                    transferred_product_identifier)
                .status);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    destination_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
    return destination_snapshot &&
           destination_snapshot->tab_identifiers ==
               std::vector<int32_t>{destination_initial_handle.raw_value()};
  }));
  EXPECT_EQ(destination_workspace,
            destination_snapshot->workspace_identifiers[0]);
  CloseBrowserSynchronously(destination_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       CloseAllAndTeardownRejectStaleWorkspaceOperations) {
  Browser* closing_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(closing_browser);
  closing_browser->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      closing_browser, https_server_.GetURL("a.test", "/one"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  views::View* closing_rail = GetSwiftUITabRail(closing_browser);
  ASSERT_TRUE(closing_rail);
  auto closing_snapshot = WaitForRailTabs(closing_rail, 2);
  ASSERT_TRUE(closing_snapshot);
  std::vector<tabs::TabHandle> closing_handles;
  for (tabs::TabInterface* tab :
       TabListInterface::From(closing_browser)->GetAllTabs()) {
    closing_handles.push_back(tab->GetHandle());
  }
  BrowserWindowInterface* closing_window = closing_browser;
  closing_browser->GetTabStripModel()->CloseAllTabs();
  ASSERT_TRUE(WaitForCondition([&]() {
    if (std::ranges::any_of(closing_handles,
                            [](tabs::TabHandle tab) { return tab.Get(); })) {
      return false;
    }
    bool window_is_live = false;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* candidate) {
          window_is_live |= candidate == closing_window;
          return true;
        });
    return !window_is_live;
  }));

  Browser* stopped_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(stopped_browser);
  stopped_browser->GetWindow()->Show();
  views::View* stopped_rail = GetSwiftUITabRail(stopped_browser);
  ASSERT_TRUE(stopped_rail);
  auto stopped_snapshot = WaitForRailTabs(stopped_rail, 1);
  ASSERT_TRUE(stopped_snapshot);
  limitless::WorkspaceController* stopped_controller =
      limitless::WorkspaceController::From(stopped_browser);
  ASSERT_TRUE(stopped_controller);
  const limitless::WorkspaceWindowSnapshot product_snapshot =
      stopped_controller->GetSnapshot();
  ASSERT_EQ(1u, product_snapshot.visible_tab_projections.size());
  const std::string product_tab_identifier =
      product_snapshot.visible_tab_projections[0].identifier;
  const std::string workspace_identifier =
      product_snapshot.workspaces[0].identifier;
  stopped_controller->Stop();
  EXPECT_EQ(nullptr, limitless::WorkspaceController::From(stopped_browser));
  EXPECT_FALSE(stopped_controller->GetSnapshot().enabled);
  EXPECT_TRUE(stopped_controller->GetSnapshot().visible_tabs.empty());
  EXPECT_EQ(limitless::WorkspaceOperationStatus::kWindowUnavailable,
            stopped_controller->SwitchWorkspace(workspace_identifier).status);
  EXPECT_EQ(limitless::WorkspaceOperationStatus::kWindowUnavailable,
            stopped_controller
                ->ActivateTabByIdentifier(product_snapshot.window_identifier,
                                          product_tab_identifier)
                .status);
  CloseBrowserSynchronously(stopped_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       WorkspaceLeaderUsesVisibleChromiumOrder) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* one = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* two = tab_strip->GetTabAtIndex(1);
  tabs::TabInterface* three = tab_strip->GetTabAtIndex(2);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  auto snapshot = WaitForRailTabs(rail, 3);
  ASSERT_TRUE(snapshot);
  const std::string default_workspace = snapshot->workspace_identifiers[0];
  const std::optional<std::string> focus_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(rail, u"Focus");
  ASSERT_TRUE(focus_workspace);
  tabs::TabInterface* focus_new_tab = tab_strip->GetActiveTab();
  ASSERT_TRUE(focus_new_tab);
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      rail, two->GetHandle().raw_value(), *focus_workspace));
  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      rail, three->GetHandle().raw_value(), *focus_workspace));
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *focus_workspace));
  ASSERT_EQ(focus_new_tab, tab_strip->GetActiveTab());

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == two && !IsLeaderArmed(rail);
  }));
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_2, ui::EF_NONE, u'2');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == three && !IsLeaderArmed(rail);
  }));
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_P, ui::EF_NONE, u'p');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == two && !IsLeaderArmed(rail);
  }));
  EXPECT_NE(one, tab_strip->GetActiveTab());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       WorkspaceOperationsAreWindowAndProfileScoped) {
  views::View* first_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(first_rail);
  auto first_snapshot = WaitForRailTabs(first_rail, 1);
  ASSERT_TRUE(first_snapshot);
  limitless::WorkspaceController* first_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(first_controller);
  const std::optional<std::string> first_extra_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(first_rail, u"First");
  ASSERT_TRUE(first_extra_workspace);
  const limitless::WorkspaceWindowSnapshot first_before_foreign_operations =
      first_controller->GetSnapshot();
  ASSERT_TRUE(first_before_foreign_operations.active_tab);
  content::WebContents* first_active_contents =
      first_before_foreign_operations.active_tab->Get()->GetContents();

  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  second_browser->GetWindow()->Show();
  views::View* second_rail = GetSwiftUITabRail(second_browser);
  ASSERT_TRUE(second_rail);
  auto second_snapshot = WaitForRailTabs(second_rail, 1);
  ASSERT_TRUE(second_snapshot);
  ASSERT_NE(first_snapshot->window_identifier,
            second_snapshot->window_identifier);
  ASSERT_NE(first_snapshot->workspace_identifiers[0],
            second_snapshot->workspace_identifiers[0]);
  const tabs::TabHandle second_tab =
      second_browser->GetTabStripModel()->GetActiveTab()->GetHandle();
  EXPECT_FALSE(limitless::SwitchSwiftUIWorkspaceForTesting(
      second_rail, *first_extra_workspace));
  EXPECT_FALSE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      first_rail, second_tab.raw_value(), *first_extra_workspace));
  EXPECT_EQ(
      limitless::WorkspaceOperationStatus::kForeignWindow,
      first_controller->MoveTabToWorkspace(second_tab, *first_extra_workspace)
          .status);

  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile* other_profile = &profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  Browser* profile_browser = CreateBrowser(other_profile);
  ASSERT_TRUE(profile_browser);
  profile_browser->GetWindow()->Show();
  views::View* profile_rail = GetSwiftUITabRail(profile_browser);
  ASSERT_TRUE(profile_rail);
  auto profile_snapshot = WaitForRailTabs(profile_rail, 1);
  ASSERT_TRUE(profile_snapshot);
  const tabs::TabHandle profile_tab =
      profile_browser->GetTabStripModel()->GetActiveTab()->GetHandle();
  EXPECT_FALSE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      first_rail, profile_tab.raw_value(), *first_extra_workspace));
  EXPECT_FALSE(limitless::SwitchSwiftUIWorkspaceForTesting(
      profile_rail, *first_extra_workspace));
  EXPECT_EQ(
      limitless::WorkspaceOperationStatus::kProfileMismatch,
      first_controller->MoveTabToWorkspace(profile_tab, *first_extra_workspace)
          .status);
  limitless::WorkspaceController* profile_controller =
      limitless::WorkspaceController::From(profile_browser);
  ASSERT_TRUE(profile_controller);
  const limitless::WorkspaceWindowSnapshot profile_product_snapshot =
      profile_controller->GetSnapshot();
  ASSERT_EQ(1u, profile_product_snapshot.visible_tab_projections.size());
  EXPECT_EQ(
      limitless::WorkspaceOperationStatus::kStaleTab,
      first_controller
          ->ActivateTabByIdentifier(
              first_before_foreign_operations.window_identifier,
              profile_product_snapshot.visible_tab_projections[0].identifier)
          .status);

  Browser* private_browser = CreateIncognitoBrowser();
  ASSERT_TRUE(private_browser);
  private_browser->GetWindow()->Show();
  views::View* private_rail = GetSwiftUITabRail(private_browser);
  ASSERT_TRUE(private_rail);
  auto private_snapshot = WaitForRailTabs(private_rail, 1);
  ASSERT_TRUE(private_snapshot);
  ASSERT_NE(first_snapshot->window_identifier,
            private_snapshot->window_identifier);
  ASSERT_NE(first_snapshot->workspace_identifiers[0],
            private_snapshot->workspace_identifiers[0]);
  EXPECT_FALSE(limitless::SwitchSwiftUIWorkspaceForTesting(
      private_rail, *first_extra_workspace));
  EXPECT_FALSE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      first_rail,
      private_browser->GetTabStripModel()
          ->GetActiveTab()
          ->GetHandle()
          .raw_value(),
      *first_extra_workspace));
  const std::optional<std::string> private_extra_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(private_rail, u"Private");
  ASSERT_TRUE(private_extra_workspace);
  EXPECT_NE(*first_extra_workspace, *private_extra_workspace);
  EXPECT_FALSE(limitless::GetWorkspaceWindowSessionData(private_browser));
  EXPECT_FALSE(limitless::GetWorkspaceTabSessionData(
      private_browser->GetTabStripModel()->GetActiveWebContents()));

  CloseBrowserSynchronously(private_browser);
  Browser* recreated_private_browser = CreateIncognitoBrowser();
  ASSERT_TRUE(recreated_private_browser);
  recreated_private_browser->GetWindow()->Show();
  views::View* recreated_private_rail =
      GetSwiftUITabRail(recreated_private_browser);
  ASSERT_TRUE(recreated_private_rail);
  auto recreated_private_snapshot = WaitForRailTabs(recreated_private_rail, 1);
  ASSERT_TRUE(recreated_private_snapshot);
  EXPECT_NE(private_snapshot->window_identifier,
            recreated_private_snapshot->window_identifier);
  EXPECT_NE(private_snapshot->workspace_identifiers,
            recreated_private_snapshot->workspace_identifiers);
  EXPECT_EQ((std::vector<std::u16string>{u"Workspace"}),
            recreated_private_snapshot->workspace_names);
  EXPECT_FALSE(
      limitless::GetWorkspaceWindowSessionData(recreated_private_browser));
  const limitless::WorkspaceWindowSnapshot first_after_foreign_operations =
      first_controller->GetSnapshot();
  EXPECT_EQ(first_before_foreign_operations.window_identifier,
            first_after_foreign_operations.window_identifier);
  EXPECT_EQ(first_before_foreign_operations.active_workspace_identifier,
            first_after_foreign_operations.active_workspace_identifier);
  EXPECT_EQ(first_before_foreign_operations.visible_tabs,
            first_after_foreign_operations.visible_tabs);
  ASSERT_TRUE(first_after_foreign_operations.active_tab);
  EXPECT_EQ(*first_before_foreign_operations.active_tab,
            *first_after_foreign_operations.active_tab);
  EXPECT_EQ(first_active_contents,
            first_after_foreign_operations.active_tab->Get()->GetContents());
  CloseBrowserSynchronously(recreated_private_browser);
  CloseBrowserSynchronously(profile_browser);
  CloseBrowserSynchronously(second_browser);
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    ProfileWideSessionFinalizationSkipsControllerDestroyedByEarlierActivation) {
  Browser* auxiliary_one = CreateBrowser(browser()->GetProfile());
  Browser* auxiliary_two = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(auxiliary_one);
  ASSERT_TRUE(auxiliary_two);
  auxiliary_one->GetWindow()->Show();
  auxiliary_two->GetWindow()->Show();
  ASSERT_TRUE(WaitForRailTabs(GetSwiftUITabRail(auxiliary_one), 1));
  ASSERT_TRUE(WaitForRailTabs(GetSwiftUITabRail(auxiliary_two), 1));

  const std::vector<Browser*> browsers = {browser(), auxiliary_one,
                                          auxiliary_two};
  Browser* first_browser = browsers.front();
  for (Browser* candidate : browsers) {
    if (std::less<BrowserWindowInterface*>{}(
            static_cast<BrowserWindowInterface*>(candidate),
            static_cast<BrowserWindowInterface*>(first_browser))) {
      first_browser = candidate;
    }
  }
  Browser* browser_to_close =
      first_browser == auxiliary_one ? auxiliary_two : auxiliary_one;
  BrowserWindowInterface* const closed_window = browser_to_close;

  TabListInterface* first_tabs = TabListInterface::From(first_browser);
  ASSERT_TRUE(first_tabs);
  tabs::TabInterface* initially_active = first_tabs->GetActiveTab();
  ASSERT_TRUE(initially_active);
  const int original_tab_count = first_tabs->GetTabCount();
  chrome::AddTabAt(first_browser, GURL("about:blank#restore-target"), -1,
                   /*foreground=*/false);
  ASSERT_EQ(original_tab_count + 1, first_tabs->GetTabCount());
  tabs::TabInterface* restore_target =
      first_tabs->GetTab(first_tabs->GetTabCount() - 1);
  ASSERT_TRUE(restore_target);
  ASSERT_EQ(initially_active, first_tabs->GetActiveTab());
  ASSERT_TRUE(WaitForRailTabs(GetSwiftUITabRail(first_browser),
                              first_tabs->GetTabCount()));

  limitless::WorkspaceController* first_controller =
      limitless::WorkspaceController::From(first_browser);
  ASSERT_TRUE(first_controller);
  const limitless::WorkspaceWindowSnapshot initial_snapshot =
      first_controller->GetSnapshot();
  ASSERT_EQ(1u, initial_snapshot.workspaces.size());
  const std::string default_workspace =
      initial_snapshot.workspaces.front().identifier;
  const std::optional<std::string> serialized_window =
      first_controller->SerializeWindowForSession();
  ASSERT_TRUE(serialized_window);
  std::optional<base::Value> restored_window =
      base::JSONReader::Read(*serialized_window, base::JSON_PARSE_RFC);
  ASSERT_TRUE(restored_window && restored_window->is_dict());
  base::ListValue* restored_workspaces =
      restored_window->GetDict().FindList("workspaces");
  ASSERT_TRUE(restored_workspaces);
  ASSERT_EQ(1u, restored_workspaces->size());
  const std::string restored_target_identifier =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  restored_workspaces->front().GetDict().Set("last_active_tab_id",
                                             restored_target_identifier);
  std::string restored_window_data;
  ASSERT_TRUE(base::JSONWriter::Write(*restored_window, &restored_window_data));

  first_controller->BeginSessionRestore();
  ASSERT_TRUE(first_controller->RestoreWindowFromSession(restored_window_data));
  ASSERT_TRUE(first_controller->RestoreTabFromSession(
      restore_target->GetHandle(),
      BuildWorkspaceTabSessionData(restored_target_identifier,
                                   default_workspace)));
  ASSERT_TRUE(first_controller->GetSnapshot().session_restore_in_progress);
  ASSERT_EQ(initially_active, first_tabs->GetActiveTab());

  limitless::WorkspaceController* controller_to_destroy =
      limitless::WorkspaceController::From(browser_to_close);
  ASSERT_TRUE(controller_to_destroy);
  controller_to_destroy->BeginSessionRestore();
  ASSERT_TRUE(controller_to_destroy->GetSnapshot().session_restore_in_progress);

  int close_callback_count = 0;
  RunOnceOnActiveTabChangedObserver destroy_later_controller(
      first_tabs, base::BindLambdaForTesting([this, browser_to_close,
                                              &close_callback_count]() mutable {
        ++close_callback_count;
        CloseBrowserSynchronously(browser_to_close);
      }));

  limitless::FinalizeWorkspaceSessionRestore(browser()->GetProfile());

  EXPECT_EQ(1, close_callback_count);
  EXPECT_EQ(first_controller,
            limitless::WorkspaceController::From(first_browser));
  const limitless::WorkspaceWindowSnapshot finalized_snapshot =
      first_controller->GetSnapshot();
  EXPECT_FALSE(finalized_snapshot.session_restore_in_progress);
  EXPECT_EQ(restore_target, first_tabs->GetActiveTab());
  ASSERT_TRUE(finalized_snapshot.active_tab);
  EXPECT_EQ(restore_target->GetHandle(), *finalized_snapshot.active_tab);
  bool closed_window_is_live = false;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* candidate) {
        closed_window_is_live |= candidate == closed_window;
        return true;
      });
  EXPECT_FALSE(closed_window_is_live);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       WorkspacePolicyAndRestoreDataAreStrictlyValidated) {
  base::CommandLine policy(base::CommandLine::NO_PROGRAM);
  EXPECT_TRUE(limitless::AreWorkspacesEnabled(policy));
  policy.AppendSwitchASCII(limitless::kWorkspacesSwitch, "invalid");
  EXPECT_FALSE(limitless::AreWorkspacesEnabled(policy));

  base::CommandLine named_policy(base::CommandLine::NO_PROGRAM);
  named_policy.AppendSwitchUTF8(limitless::kDefaultWorkspaceNameSwitch,
                                "D\xC3\xA9j\xC3\xA0 Vu");
  EXPECT_EQ(u"D\u00e9j\u00e0 Vu",
            limitless::GetDefaultWorkspaceName(named_policy));
  base::CommandLine unsafe_name(base::CommandLine::NO_PROGRAM);
  unsafe_name.AppendSwitchUTF8(limitless::kDefaultWorkspaceNameSwitch,
                               base::UTF16ToUTF8(u"unsafe\u202ename"));
  EXPECT_EQ(u"Workspace", limitless::GetDefaultWorkspaceName(unsafe_name));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabListInterface* tabs = TabListInterface::From(browser());
  ASSERT_TRUE(tabs);
  tabs::TabInterface* one = tabs->GetTab(0);
  tabs::TabInterface* two = tabs->GetTab(1);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  limitless::WorkspaceController* controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(controller);
  const limitless::WorkspaceWindowSnapshot initial = controller->GetSnapshot();
  ASSERT_EQ(1u, initial.workspaces.size());
  ASSERT_EQ(2u, initial.visible_tab_projections.size());
  const std::string initial_window_identifier = initial.window_identifier;
  const std::string default_workspace = initial.workspaces[0].identifier;

  EXPECT_FALSE(controller->RestoreWindowFromSession("{malformed"));
  EXPECT_EQ(initial_window_identifier,
            controller->GetSnapshot().window_identifier);
  EXPECT_FALSE(
      controller->RestoreWindowFromSession(std::string(33 * 1024, 'x')));
  EXPECT_EQ(initial_window_identifier,
            controller->GetSnapshot().window_identifier);

  const std::optional<std::string> serialized_window =
      controller->SerializeWindowForSession();
  ASSERT_TRUE(serialized_window);
  std::optional<base::Value> duplicate_workspace_value =
      base::JSONReader::Read(*serialized_window, base::JSON_PARSE_RFC);
  ASSERT_TRUE(duplicate_workspace_value &&
              duplicate_workspace_value->is_dict());
  base::ListValue* workspace_values =
      duplicate_workspace_value->GetDict().FindList("workspaces");
  ASSERT_TRUE(workspace_values);
  ASSERT_FALSE(workspace_values->empty());
  workspace_values->Append((*workspace_values)[0].Clone());
  std::string duplicate_workspace_data;
  ASSERT_TRUE(base::JSONWriter::Write(*duplicate_workspace_value,
                                      &duplicate_workspace_data));
  EXPECT_FALSE(controller->RestoreWindowFromSession(duplicate_workspace_data));
  EXPECT_EQ(initial_window_identifier,
            controller->GetSnapshot().window_identifier);

  const std::string shared_product_tab_identifier =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string missing_workspace_identifier =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  controller->BeginSessionRestore();
  ASSERT_TRUE(controller->RestoreWindowFromSession(*serialized_window));
  ASSERT_TRUE(controller->RestoreTabFromSession(
      one->GetHandle(),
      BuildWorkspaceTabSessionData(shared_product_tab_identifier,
                                   missing_workspace_identifier)));
  ASSERT_TRUE(controller->RestoreTabFromSession(
      two->GetHandle(), BuildWorkspaceTabSessionData(
                            shared_product_tab_identifier, default_workspace)));
  controller->FinalizeSessionRestore();
  const std::optional<std::string> one_data =
      controller->SerializeTabForSession(one->GetHandle());
  const std::optional<std::string> two_data =
      controller->SerializeTabForSession(two->GetHandle());
  ASSERT_TRUE(one_data);
  ASSERT_TRUE(two_data);
  const auto [one_product_identifier, one_workspace_identifier] =
      ReadWorkspaceTabSessionData(*one_data);
  const auto [two_product_identifier, two_workspace_identifier] =
      ReadWorkspaceTabSessionData(*two_data);
  EXPECT_EQ(shared_product_tab_identifier, one_product_identifier);
  EXPECT_EQ(default_workspace, one_workspace_identifier);
  EXPECT_NE(shared_product_tab_identifier, two_product_identifier);
  EXPECT_EQ(default_workspace, two_workspace_identifier);

  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  second_browser->GetWindow()->Show();
  auto second_snapshot = WaitForRailTabs(GetSwiftUITabRail(second_browser), 1);
  ASSERT_TRUE(second_snapshot);
  limitless::WorkspaceController* second_controller =
      limitless::WorkspaceController::From(second_browser);
  ASSERT_TRUE(second_controller);
  const std::optional<std::string> second_serialized_window =
      second_controller->SerializeWindowForSession();
  ASSERT_TRUE(second_serialized_window);
  second_controller->BeginSessionRestore();
  ASSERT_TRUE(
      second_controller->RestoreWindowFromSession(*second_serialized_window));
  const tabs::TabHandle second_tab =
      second_browser->GetTabStripModel()->GetActiveTab()->GetHandle();
  const std::string second_workspace_identifier =
      second_controller->GetSnapshot().workspaces[0].identifier;
  ASSERT_TRUE(second_controller->RestoreTabFromSession(
      second_tab, BuildWorkspaceTabSessionData(shared_product_tab_identifier,
                                               second_workspace_identifier)));
  second_controller->FinalizeSessionRestore();
  const std::optional<std::string> duplicate_cross_window_tab_data =
      second_controller->SerializeTabForSession(second_tab);
  ASSERT_TRUE(duplicate_cross_window_tab_data);
  EXPECT_NE(
      shared_product_tab_identifier,
      ReadWorkspaceTabSessionData(*duplicate_cross_window_tab_data).first);

  const std::string second_original_window_identifier =
      second_controller->GetSnapshot().window_identifier;
  second_controller->BeginSessionRestore();
  EXPECT_FALSE(second_controller->RestoreWindowFromSession(*serialized_window));
  second_controller->FinalizeSessionRestore();
  const limitless::WorkspaceWindowSnapshot duplicate_window_snapshot =
      second_controller->GetSnapshot();
  EXPECT_NE(initial_window_identifier,
            duplicate_window_snapshot.window_identifier);
  EXPECT_NE(second_original_window_identifier,
            duplicate_window_snapshot.window_identifier);

  const std::string saved_active_product_identifier =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  std::optional<base::Value> saved_active_value =
      base::JSONReader::Read(*serialized_window, base::JSON_PARSE_RFC);
  ASSERT_TRUE(saved_active_value && saved_active_value->is_dict());
  base::ListValue* saved_active_workspace_values =
      saved_active_value->GetDict().FindList("workspaces");
  ASSERT_TRUE(saved_active_workspace_values);
  ASSERT_FALSE(saved_active_workspace_values->empty());
  saved_active_workspace_values->front().GetDict().Set(
      "last_active_tab_id", saved_active_product_identifier);
  std::string saved_active_data;
  ASSERT_TRUE(base::JSONWriter::Write(*saved_active_value, &saved_active_data));
  tabs->ActivateTab(one->GetHandle());
  ASSERT_EQ(one, tabs->GetActiveTab());
  controller->BeginSessionRestore();
  ASSERT_TRUE(controller->RestoreWindowFromSession(saved_active_data));
  ASSERT_TRUE(controller->RestoreTabFromSession(
      two->GetHandle(),
      BuildWorkspaceTabSessionData(saved_active_product_identifier,
                                   default_workspace)));
  controller->FinalizeSessionRestore();
  EXPECT_EQ(two, tabs->GetActiveTab());
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting>
      saved_active_snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    saved_active_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return saved_active_snapshot &&
           saved_active_snapshot->active_tab_identifier ==
               two->GetHandle().raw_value();
  }));
  const std::optional<std::string> saved_active_tab_data =
      controller->SerializeTabForSession(two->GetHandle());
  ASSERT_TRUE(saved_active_tab_data);
  EXPECT_EQ(saved_active_product_identifier,
            ReadWorkspaceTabSessionData(*saved_active_tab_data).first);

  const std::string native_active_product_identifier =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  tabs->ActivateTab(one->GetHandle());
  ASSERT_EQ(one, tabs->GetActiveTab());
  controller->BeginSessionRestore();
  ASSERT_TRUE(controller->RestoreWindowFromSession(saved_active_data));
  ASSERT_TRUE(controller->RestoreTabFromSession(
      one->GetHandle(),
      BuildWorkspaceTabSessionData(native_active_product_identifier,
                                   default_workspace)));
  ASSERT_TRUE(controller->RestoreTabFromSession(
      two->GetHandle(),
      BuildWorkspaceTabSessionData(saved_active_product_identifier,
                                   default_workspace)));
  controller->FinalizeSessionRestore();
  EXPECT_EQ(one, tabs->GetActiveTab());
  ASSERT_TRUE(base::test::RunUntil([&]() {
    saved_active_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return saved_active_snapshot &&
           saved_active_snapshot->active_tab_identifier ==
               one->GetHandle().raw_value();
  }));
  const std::optional<std::string> native_active_window_data =
      controller->SerializeWindowForSession();
  ASSERT_TRUE(native_active_window_data);
  const std::optional<base::Value> native_active_value =
      base::JSONReader::Read(*native_active_window_data, base::JSON_PARSE_RFC);
  ASSERT_TRUE(native_active_value && native_active_value->is_dict());
  const base::ListValue* native_active_workspace_values =
      native_active_value->GetDict().FindList("workspaces");
  ASSERT_TRUE(native_active_workspace_values);
  ASSERT_FALSE(native_active_workspace_values->empty());
  const std::string* serialized_last_active =
      native_active_workspace_values->front().GetDict().FindString(
          "last_active_tab_id");
  ASSERT_TRUE(serialized_last_active);
  EXPECT_EQ(native_active_product_identifier, *serialized_last_active);

  std::optional<base::Value> stale_last_active_value =
      base::JSONReader::Read(*serialized_window, base::JSON_PARSE_RFC);
  ASSERT_TRUE(stale_last_active_value && stale_last_active_value->is_dict());
  base::ListValue* stale_workspace_values =
      stale_last_active_value->GetDict().FindList("workspaces");
  ASSERT_TRUE(stale_workspace_values);
  stale_workspace_values->front().GetDict().Set(
      "last_active_tab_id", base::Uuid::GenerateRandomV4().AsLowercaseString());
  std::string stale_last_active_data;
  ASSERT_TRUE(base::JSONWriter::Write(*stale_last_active_value,
                                      &stale_last_active_data));
  controller->BeginSessionRestore();
  ASSERT_TRUE(controller->RestoreWindowFromSession(stale_last_active_data));
  controller->FinalizeSessionRestore();
  const limitless::WorkspaceWindowSnapshot reconciled =
      controller->GetSnapshot();
  EXPECT_EQ(2u, reconciled.visible_tabs.size());
  ASSERT_TRUE(reconciled.active_tab);
  EXPECT_EQ(one->GetHandle(), *reconciled.active_tab);

  controller->BeginSessionRestore();
  const std::string fresh_window_after_bad_envelope =
      controller->GetSnapshot().window_identifier;
  EXPECT_FALSE(controller->RestoreWindowFromSession("{malformed"));
  EXPECT_FALSE(controller->RestoreTabFromSession(
      one->GetHandle(), BuildWorkspaceTabSessionData(
                            shared_product_tab_identifier, default_workspace)));
  controller->FinalizeSessionRestore();
  const limitless::WorkspaceWindowSnapshot malformed_recovery =
      controller->GetSnapshot();
  EXPECT_EQ(fresh_window_after_bad_envelope,
            malformed_recovery.window_identifier);
  ASSERT_TRUE(controller->SerializeTabForSession(one->GetHandle()));
  EXPECT_NE(shared_product_tab_identifier,
            ReadWorkspaceTabSessionData(
                *controller->SerializeTabForSession(one->GetHandle()))
                .first);

  Browser* malformed_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(malformed_browser);
  malformed_browser->GetWindow()->Show();
  ASSERT_TRUE(WaitForRailTabs(GetSwiftUITabRail(malformed_browser), 1));
  limitless::WorkspaceController* malformed_controller =
      limitless::WorkspaceController::From(malformed_browser);
  ASSERT_TRUE(malformed_controller);
  const limitless::WorkspaceWindowSnapshot before_malformed_restore =
      malformed_controller->GetSnapshot();
  const tabs::TabHandle malformed_tab =
      malformed_browser->GetTabStripModel()->GetActiveTab()->GetHandle();
  limitless::RestoreWorkspaceWindowSessionData(
      malformed_browser,
      {{limitless::kWorkspaceWindowSessionKey, "{malformed"}});
  limitless::RestoreWorkspaceTabSessionData(
      malformed_browser, malformed_tab.Get()->GetContents(),
      {{limitless::kWorkspaceTabSessionKey,
        BuildWorkspaceTabSessionData(
            shared_product_tab_identifier,
            before_malformed_restore.workspaces[0].identifier)}});
  limitless::FinalizeWorkspaceSessionRestore(browser()->GetProfile());
  const limitless::WorkspaceWindowSnapshot after_malformed_restore =
      malformed_controller->GetSnapshot();
  EXPECT_NE(before_malformed_restore.window_identifier,
            after_malformed_restore.window_identifier);
  const std::optional<std::string> malformed_tab_data =
      malformed_controller->SerializeTabForSession(malformed_tab);
  ASSERT_TRUE(malformed_tab_data);
  EXPECT_NE(shared_product_tab_identifier,
            ReadWorkspaceTabSessionData(*malformed_tab_data).first);
  ASSERT_TRUE(after_malformed_restore.active_tab);
  EXPECT_EQ(malformed_tab, *after_malformed_restore.active_tab);
  CloseBrowserSynchronously(malformed_browser);

  Browser* missing_tab_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(missing_tab_browser);
  missing_tab_browser->GetWindow()->Show();
  views::View* missing_tab_rail = GetSwiftUITabRail(missing_tab_browser);
  ASSERT_TRUE(missing_tab_rail);
  ASSERT_TRUE(WaitForRailTabs(missing_tab_rail, 1));
  const std::optional<std::string> saved_active_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(missing_tab_rail,
                                                  u"Saved Active");
  ASSERT_TRUE(saved_active_workspace);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting>
      missing_tab_snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    missing_tab_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(missing_tab_rail);
    return missing_tab_snapshot &&
           missing_tab_snapshot->workspace_identifiers.size() == 2 &&
           missing_tab_snapshot->active_workspace_identifier ==
               saved_active_workspace &&
           missing_tab_snapshot->tab_identifiers.size() == 1;
  }));
  TabListInterface* missing_tab_list =
      TabListInterface::From(missing_tab_browser);
  ASSERT_TRUE(missing_tab_list);
  ASSERT_EQ(2, missing_tab_list->GetTabCount());
  tabs::TabInterface* missing_envelope_tab = missing_tab_list->GetTab(0);
  tabs::TabInterface* malformed_envelope_tab = missing_tab_list->GetTab(1);
  ASSERT_TRUE(missing_envelope_tab);
  ASSERT_TRUE(malformed_envelope_tab);
  limitless::WorkspaceController* missing_tab_controller =
      limitless::WorkspaceController::From(missing_tab_browser);
  ASSERT_TRUE(missing_tab_controller);
  const limitless::WorkspaceWindowSnapshot missing_before =
      missing_tab_controller->GetSnapshot();
  ASSERT_EQ(2u, missing_before.workspaces.size());
  const std::string missing_default_workspace =
      missing_before.workspaces[0].identifier;
  const std::optional<std::string> missing_window_data =
      missing_tab_controller->SerializeWindowForSession();
  ASSERT_TRUE(missing_window_data);

  limitless::RestoreWorkspaceWindowSessionData(
      missing_tab_browser,
      {{limitless::kWorkspaceWindowSessionKey, *missing_window_data}});
  limitless::RestoreWorkspaceTabSessionData(
      missing_tab_browser, missing_envelope_tab->GetContents(), {});
  limitless::RestoreWorkspaceTabSessionData(
      missing_tab_browser, malformed_envelope_tab->GetContents(),
      {{limitless::kWorkspaceTabSessionKey, "{malformed"}});
  limitless::FinalizeWorkspaceSessionRestore(browser()->GetProfile());

  const std::optional<std::string> missing_envelope_data =
      missing_tab_controller->SerializeTabForSession(
          missing_envelope_tab->GetHandle());
  const std::optional<std::string> malformed_envelope_data =
      missing_tab_controller->SerializeTabForSession(
          malformed_envelope_tab->GetHandle());
  ASSERT_TRUE(missing_envelope_data);
  ASSERT_TRUE(malformed_envelope_data);
  const auto [missing_product_identifier, missing_tab_workspace_identifier] =
      ReadWorkspaceTabSessionData(*missing_envelope_data);
  const auto [malformed_product_identifier, malformed_workspace_identifier] =
      ReadWorkspaceTabSessionData(*malformed_envelope_data);
  EXPECT_TRUE(
      base::Uuid::ParseLowercase(missing_product_identifier).is_valid());
  EXPECT_TRUE(
      base::Uuid::ParseLowercase(malformed_product_identifier).is_valid());
  EXPECT_NE(missing_product_identifier, malformed_product_identifier);
  EXPECT_EQ(missing_default_workspace, missing_tab_workspace_identifier);
  EXPECT_EQ(missing_default_workspace, malformed_workspace_identifier);
  const limitless::WorkspaceWindowSnapshot missing_after =
      missing_tab_controller->GetSnapshot();
  EXPECT_EQ(saved_active_workspace, missing_after.active_workspace_identifier);
  ASSERT_TRUE(missing_after.active_tab);
  EXPECT_NE(missing_envelope_tab->GetHandle(), *missing_after.active_tab);
  EXPECT_NE(malformed_envelope_tab->GetHandle(), *missing_after.active_tab);
  EXPECT_TRUE(
      missing_after.active_tab->Get()->GetContents()->GetVisibleURL().SchemeIs(
          "chrome"));
  CloseBrowserSynchronously(missing_tab_browser);
  CloseBrowserSynchronously(second_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIWorkspacesDisabledBrowserTest,
                       DisabledPolicyKeepsChromiumTabAuthorityAndNoMetadata) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  auto snapshot = WaitForRailTabs(rail, 2);
  ASSERT_TRUE(snapshot);
  EXPECT_FALSE(snapshot->workspaces_enabled);
  EXPECT_TRUE(snapshot->workspace_identifiers.empty());
  EXPECT_EQ(2u, snapshot->tab_identifiers.size());
  EXPECT_EQ(2u, snapshot->tab_titles.size());
  EXPECT_TRUE(snapshot->tab_product_identifiers.empty());
  EXPECT_FALSE(snapshot->split_actions_available);
  EXPECT_TRUE(snapshot->split_pair_identifiers.empty());
  const auto closed_inventory =
      limitless::GetSwiftUIAccessibilityInventoryForTesting(rail);
  EXPECT_FALSE(closed_inventory.hit_node_cap);
  for (const std::string& identifier : closed_inventory.identifiers) {
    EXPECT_FALSE(base::StartsWith(identifier, "limitless-tab-split-"));
    EXPECT_EQ(std::string::npos, identifier.find("transient-"));
  }
  EXPECT_FALSE(limitless::CreateSwiftUIWorkspaceForTesting(rail, u"Blocked"));
  EXPECT_FALSE(limitless::SwitchSwiftUIWorkspaceForTesting(
      rail, base::Uuid::GenerateRandomV4().AsLowercaseString()));
  EXPECT_FALSE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      rail, snapshot->tab_identifiers[0],
      base::Uuid::GenerateRandomV4().AsLowercaseString()));
  EXPECT_FALSE(limitless::GetWorkspaceWindowSessionData(browser()));
  EXPECT_FALSE(limitless::GetWorkspaceTabSessionData(
      browser()->GetTabStripModel()->GetActiveWebContents()));

  tabs::TabInterface* first = browser()->GetTabStripModel()->GetTabAtIndex(0);
  ASSERT_TRUE(first);
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(WaitForCondition([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(WaitForCondition([&]() {
    return browser()->GetTabStripModel()->GetActiveTab() == first &&
           !IsLeaderArmed(rail);
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUICustomWorkspaceBrowserTest,
                       CliDefaultWorkspaceNameReachesNativeRail) {
  auto snapshot = WaitForRailTabs(GetSwiftUITabRail(browser()), 1);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ((std::vector<std::u16string>{u"D\u00e9j\u00e0 Vu"}),
            snapshot->workspace_names);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       PRE_RestoresNativeSplitPanesAcrossCleanBrowserRestart) {
  SessionStartupPref::SetStartupPref(
      browser()->GetProfile(), SessionStartupPref(SessionStartupPref::LAST));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  for (const std::string& path :
       std::vector<std::string>{"/two", "/three", "/four", "/five", "/six"}) {
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser(), https_server_.GetURL("a.test", path),
        WindowOpenDisposition::NEW_BACKGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  }

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  ASSERT_EQ(6, tab_strip->count());
  tabs::TabInterface* one = FindTabWithPath(browser(), "/one");
  tabs::TabInterface* two = FindTabWithPath(browser(), "/two");
  tabs::TabInterface* three = FindTabWithPath(browser(), "/three");
  tabs::TabInterface* four = FindTabWithPath(browser(), "/four");
  tabs::TabInterface* five = FindTabWithPath(browser(), "/five");
  tabs::TabInterface* six = FindTabWithPath(browser(), "/six");
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  ASSERT_TRUE(four);
  ASSERT_TRUE(five);
  ASSERT_TRUE(six);

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  auto snapshot = WaitForRailTabs(rail, 6);
  ASSERT_TRUE(snapshot);
  ASSERT_EQ(1u, snapshot->workspace_identifiers.size());
  const std::string window_identifier = snapshot->window_identifier;
  const std::string default_workspace = snapshot->workspace_identifiers[0];
  const std::optional<std::string> focus_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(rail, u"Focus");
  ASSERT_TRUE(focus_workspace);
  const tabs::TabHandle new_workspace_tab =
      tab_strip->GetActiveTab()->GetHandle();
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->workspace_identifiers.size() == 2 &&
           snapshot->active_workspace_identifier == focus_workspace;
  }));

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot &&
           snapshot->active_workspace_identifier == default_workspace &&
           snapshot->tab_identifiers.size() == 6;
  }));
  for (tabs::TabInterface* tab : {three, four, five, six}) {
    ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
        rail, tab->GetHandle().raw_value(), *focus_workspace));
  }
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers.size() == 2;
  }));

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *focus_workspace));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot &&
           snapshot->active_workspace_identifier == focus_workspace &&
           snapshot->tab_identifiers.size() == 5;
  }));
  ASSERT_TRUE(limitless::CloseSwiftUITabForTesting(
      rail, new_workspace_tab.raw_value()));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return !new_workspace_tab.Get() && snapshot &&
           snapshot->tab_identifiers.size() == 4;
  }));

  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  const std::map<tabs::TabInterface*, std::string> product_identifier = {
      {one, ProductIdentifierForTab(browser(), one)},
      {two, ProductIdentifierForTab(browser(), two)},
      {three, ProductIdentifierForTab(browser(), three)},
      {four, ProductIdentifierForTab(browser(), four)},
      {five, ProductIdentifierForTab(browser(), five)},
      {six, ProductIdentifierForTab(browser(), six)},
  };
  const auto create_split = [&](tabs::TabInterface* first,
                                tabs::TabInterface* second) {
    const std::string menu =
        "limitless-tab-split-" + product_identifier.at(first);
    const auto action =
        limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
            rail, menu, menu + "-with-" + product_identifier.at(second));
    return action && action->press_invoked && WaitForCondition([&]() {
             return first->IsSplit() && second->IsSplit() &&
                    first->GetSplit() == second->GetSplit();
           });
  };
  const auto set_layout_and_ratio =
      [&](tabs::TabInterface* active_member, std::string_view layout_identifier,
          std::string_view ratio_identifier,
          split_tabs::SplitTabLayout expected_layout, double expected_ratio) {
        if (!limitless::PerformSwiftUIAccessibilityActionForTesting(
                rail,
                "limitless-tab-" + base::NumberToString(
                                       active_member->GetHandle().raw_value()),
                limitless::SwiftUIAccessibilityActionForTesting::kPress)) {
          return false;
        }
        if (!WaitForCondition([&]() {
              const auto current =
                  limitless::GetSwiftUITabRailSnapshotForTesting(rail);
              return tab_strip->GetActiveTab() == active_member && current &&
                     std::ranges::find(current->split_active_tab_identifiers,
                                       product_identifier.at(active_member)) !=
                         current->split_active_tab_identifiers.end();
            })) {
          return false;
        }
        const auto current_visual_data = [&]() {
          const std::optional<split_tabs::SplitTabId> split =
              active_member->GetSplit();
          const split_tabs::SplitTabData* data =
              split ? tab_strip->GetSplitData(*split) : nullptr;
          return data ? data->visual_data() : nullptr;
        };
        const split_tabs::SplitTabVisualData* visual_data =
            current_visual_data();
        if (!visual_data) {
          return false;
        }
        if (visual_data->split_layout() != expected_layout) {
          const auto layout_action =
              limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
                  rail, "limitless-split-layout",
                  std::string(layout_identifier));
          if (!layout_action || !layout_action->press_invoked ||
              !WaitForCondition([&]() {
                const split_tabs::SplitTabVisualData* current =
                    current_visual_data();
                return current && current->split_layout() == expected_layout;
              })) {
            return false;
          }
        }
        visual_data = current_visual_data();
        if (!visual_data) {
          return false;
        }
        if (visual_data->split_ratio() != expected_ratio) {
          const auto ratio_action =
              limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
                  rail, "limitless-split-ratio", std::string(ratio_identifier));
          if (!ratio_action || !ratio_action->press_invoked) {
            return false;
          }
        }
        return WaitForCondition([&]() {
          const split_tabs::SplitTabVisualData* current = current_visual_data();
          return current && current->split_layout() == expected_layout &&
                 current->split_ratio() == expected_ratio;
        });
      };

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers.size() == 2;
  }));
  ASSERT_TRUE(create_split(one, two));
  ASSERT_TRUE(set_layout_and_ratio(
      one, "limitless-split-layout-side-by-side", "limitless-split-ratio-50",
      split_tabs::SplitTabLayout::kSideBySide, 0.5));

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *focus_workspace));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers.size() == 4;
  }));
  ASSERT_TRUE(create_split(three, four));
  ASSERT_TRUE(set_layout_and_ratio(three, "limitless-split-layout-stacked",
                                   "limitless-split-ratio-25",
                                   split_tabs::SplitTabLayout::kStacked, 0.25));
  ASSERT_TRUE(create_split(five, six));
  ASSERT_TRUE(set_layout_and_ratio(
      five, "limitless-split-layout-side-by-side", "limitless-split-ratio-75",
      split_tabs::SplitTabLayout::kSideBySide, 0.75));
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail, "limitless-split-swap",
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    const split_tabs::SplitTabData* data =
        five->GetSplit() ? tab_strip->GetSplitData(*five->GetSplit()) : nullptr;
    return data &&
           data->ListTabs() == std::vector<tabs::TabInterface*>{six, five};
  }));
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail,
      "limitless-tab-" + base::NumberToString(four->GetHandle().raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return tab_strip->GetActiveTab() == four && snapshot &&
           snapshot->active_workspace_identifier == focus_workspace &&
           snapshot->split_pair_identifiers.size() == 2;
  }));

  const auto ordered_product_members =
      [&](tabs::TabInterface* member) -> std::vector<std::string> {
    CHECK(member->GetSplit());
    const split_tabs::SplitTabData* data =
        tab_strip->GetSplitData(*member->GetSplit());
    CHECK(data);
    std::vector<std::string> ordered;
    for (tabs::TabInterface* tab : data->ListTabs()) {
      ordered.push_back(product_identifier.at(tab));
    }
    return ordered;
  };
  const std::vector<std::string> pair_one = ordered_product_members(one);
  const std::vector<std::string> pair_two = ordered_product_members(three);
  const std::vector<std::string> pair_three = ordered_product_members(five);
  ASSERT_EQ(2u, pair_one.size());
  ASSERT_EQ(2u, pair_two.size());
  ASSERT_EQ(2u, pair_three.size());

  const std::string oracle =
      base::JoinString(
          {"v1", window_identifier, default_workspace, *focus_workspace,
           pair_one[0], pair_one[1], pair_two[0], pair_two[1], pair_three[0],
           pair_three[1], product_identifier.at(four)},
          "\n") +
      "\n";
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(browser()->GetProfile()->GetPath().AppendASCII(
                                    "LimitlessSplitRestoreOracle"),
                                oracle));
  }
  workspace_controller->PersistAll();
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(browser()->GetProfile());
  ASSERT_TRUE(session_service);
  session_service->ResetFromCurrentBrowsers();
  SessionServiceTestHelper(session_service).SaveNow();
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       RestoresNativeSplitPanesAcrossCleanBrowserRestart) {
  std::string oracle;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(
        base::ReadFileToString(browser()->GetProfile()->GetPath().AppendASCII(
                                   "LimitlessSplitRestoreOracle"),
                               &oracle));
  }
  const std::vector<std::string> expected = base::SplitString(
      oracle, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(11u, expected.size());
  ASSERT_EQ("v1", expected[0]);

  Browser* restored_browser = nullptr;
  std::vector<std::string> observed_restore_state;
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        observed_restore_state.clear();
        GlobalBrowserCollection::GetInstance()->ForEach(
            [&](BrowserWindowInterface* browser_window) {
              Browser* candidate = browser_window->GetBrowserForMigrationOnly();
              limitless::WorkspaceController* controller =
                  limitless::WorkspaceController::From(browser_window);
              if (candidate && controller) {
                const limitless::WorkspaceWindowSnapshot current =
                    controller->GetSnapshot();
                observed_restore_state.push_back(base::StrCat(
                    {current.window_identifier, ":tabs=",
                     base::NumberToString(
                         candidate->GetTabStripModel()->count()),
                     ":restoring=",
                     current.session_restore_in_progress ? "true" : "false"}));
              }
              return true;
            });
        restored_browser =
            FindBrowserWithWorkspaceWindowIdentifier(expected[1]);
        if (!restored_browser) {
          return false;
        }
        limitless::WorkspaceController* controller =
            limitless::WorkspaceController::From(restored_browser);
        return controller &&
               !controller->GetSnapshot().session_restore_in_progress &&
               restored_browser->GetTabStripModel()->count() >= 6;
      },
      base::Seconds(15)))
      << base::JoinString(observed_restore_state, ", ");
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(restored_browser);
  ASSERT_TRUE(workspace_controller);
  const limitless::WorkspaceWindowSnapshot workspace_snapshot =
      workspace_controller->GetSnapshot();
  ASSERT_EQ(expected[1], workspace_snapshot.window_identifier);
  ASSERT_EQ(2u, workspace_snapshot.workspaces.size());
  EXPECT_EQ(expected[2], workspace_snapshot.workspaces[0].identifier);
  EXPECT_EQ(expected[3], workspace_snapshot.workspaces[1].identifier);
  EXPECT_EQ(expected[3], workspace_snapshot.active_workspace_identifier);

  std::map<std::string, tabs::TabInterface*> restored_tabs;
  for (size_t index = 4; index <= 10; ++index) {
    restored_tabs.try_emplace(
        expected[index],
        FindTabWithProductIdentifier(restored_browser, expected[index]));
  }
  ASSERT_EQ(6u, restored_tabs.size());
  for (const auto& [identifier, tab] : restored_tabs) {
    ASSERT_TRUE(tab) << identifier;
  }
  const auto expect_restored_page = [&](size_t identifier_index,
                                        std::string_view path,
                                        std::u16string_view title) {
    tabs::TabInterface* tab = restored_tabs.at(expected[identifier_index]);
    EXPECT_EQ(std::string(path), tab->GetContents()->GetVisibleURL().path());
    EXPECT_EQ(std::u16string(title), tab->GetContents()->GetTitle());
  };
  expect_restored_page(4, "/one", u"Rail One");
  expect_restored_page(5, "/two", u"Rail Two");
  expect_restored_page(6, "/three", u"Rail Three");
  expect_restored_page(7, "/four", u"Rail Four");
  expect_restored_page(8, "/six", u"Rail Six");
  expect_restored_page(9, "/five", u"Rail Five");

  TabStripModel* tab_strip = restored_browser->GetTabStripModel();
  size_t unrelated_live_tabs = 0;
  for (tabs::TabInterface* tab :
       TabListInterface::From(restored_browser)->GetAllTabs()) {
    const std::string product_identifier =
        ProductIdentifierForTab(restored_browser, tab);
    if (restored_tabs.contains(product_identifier)) {
      continue;
    }
    ++unrelated_live_tabs;
    EXPECT_FALSE(tab->IsSplit());
    EXPECT_NE(tab_strip->GetActiveTab(), tab);
    const base::Uuid parsed_identifier =
        base::Uuid::ParseLowercase(product_identifier);
    EXPECT_TRUE(parsed_identifier.is_valid());
    EXPECT_EQ(product_identifier, parsed_identifier.AsLowercaseString());
    EXPECT_EQ(expected.end(), std::ranges::find(expected, product_identifier));
    EXPECT_EQ(expected[3], workspace_controller->GetWorkspaceIdentifierForTab(
                               tab->GetHandle()));
    const GURL& visible_url = tab->GetContents()->GetVisibleURL();
    EXPECT_TRUE(visible_url.SchemeIs("chrome") ||
                visible_url.SchemeIs("about"));
  }
  EXPECT_LE(unrelated_live_tabs, 1u);
  ASSERT_EQ(restored_tabs.at(expected[10]), tab_strip->GetActiveTab());
  views::View* rail = GetSwiftUITabRail(restored_browser);
  ASSERT_TRUE(rail);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> rail_snapshot;
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        rail_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
        return rail_snapshot &&
               rail_snapshot->active_workspace_identifier == expected[3] &&
               rail_snapshot->tab_identifiers.size() ==
                   4 + unrelated_live_tabs &&
               rail_snapshot->split_pair_identifiers.size() == 2;
      },
      base::Seconds(15)));

  struct ExpectedPair {
    size_t first_index;
    size_t second_index;
    std::string workspace_identifier;
    split_tabs::SplitTabLayout layout;
    double ratio;
  };
  const std::vector<ExpectedPair> expected_pairs = {
      {4, 5, expected[2], split_tabs::SplitTabLayout::kSideBySide, 0.5},
      {6, 7, expected[3], split_tabs::SplitTabLayout::kStacked, 0.25},
      {8, 9, expected[3], split_tabs::SplitTabLayout::kSideBySide, 0.75},
  };
  for (const ExpectedPair& pair : expected_pairs) {
    tabs::TabInterface* first = restored_tabs.at(expected[pair.first_index]);
    tabs::TabInterface* second = restored_tabs.at(expected[pair.second_index]);
    ASSERT_TRUE(first->GetSplit());
    ASSERT_TRUE(second->GetSplit());
    const split_tabs::SplitTabData* split_data =
        tab_strip->GetSplitData(*first->GetSplit());
    ASSERT_TRUE(split_data);
    EXPECT_EQ((std::vector<tabs::TabInterface*>{first, second}),
              split_data->ListTabs());
    EXPECT_EQ(pair.layout, split_data->visual_data()->split_layout());
    EXPECT_EQ(pair.ratio, split_data->visual_data()->split_ratio());
    EXPECT_EQ(
        pair.workspace_identifier,
        workspace_controller->GetWorkspaceIdentifierForTab(first->GetHandle()));
    EXPECT_EQ(pair.workspace_identifier,
              workspace_controller->GetWorkspaceIdentifierForTab(
                  second->GetHandle()));
  }

  const auto expect_projected_pair =
      [&](const limitless::SwiftUITabRailSnapshotForTesting& projected,
          const std::string& first, const std::string& second,
          std::string_view layout, double ratio, std::string_view active) {
        const auto found =
            std::ranges::find(projected.split_first_tab_identifiers, first);
        ASSERT_NE(found, projected.split_first_tab_identifiers.end());
        const size_t index = static_cast<size_t>(std::distance(
            projected.split_first_tab_identifiers.begin(), found));
        ASSERT_LT(index, projected.split_second_tab_identifiers.size());
        ASSERT_LT(index, projected.split_layouts.size());
        ASSERT_LT(index, projected.split_ratios.size());
        ASSERT_LT(index, projected.split_active_tab_identifiers.size());
        EXPECT_EQ(second, projected.split_second_tab_identifiers[index]);
        EXPECT_EQ(layout, projected.split_layouts[index]);
        EXPECT_EQ(ratio, projected.split_ratios[index]);
        EXPECT_EQ(active, projected.split_active_tab_identifiers[index]);
      };
  expect_projected_pair(*rail_snapshot, expected[6], expected[7], "stacked",
                        0.25, expected[10]);
  expect_projected_pair(*rail_snapshot, expected[8], expected[9], "sideBySide",
                        0.75, "");

  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(rail, expected[2]));
  ASSERT_TRUE(WaitForCondition([&]() {
    rail_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return rail_snapshot &&
           rail_snapshot->active_workspace_identifier == expected[2] &&
           rail_snapshot->tab_identifiers.size() == 2 &&
           rail_snapshot->split_pair_identifiers.size() == 1 &&
           rail_snapshot->split_first_tab_identifiers ==
               std::vector<std::string>{expected[4]} &&
           rail_snapshot->split_second_tab_identifiers ==
               std::vector<std::string>{expected[5]};
  }));
  expect_projected_pair(*rail_snapshot, expected[4], expected[5], "sideBySide",
                        0.5, expected[4]);
  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(rail, expected[3]));
  ASSERT_TRUE(WaitForCondition([&]() {
    rail_snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return rail_snapshot &&
           rail_snapshot->active_workspace_identifier == expected[3] &&
           rail_snapshot->tab_identifiers.size() == 4 + unrelated_live_tabs &&
           rail_snapshot->split_pair_identifiers.size() == 2 &&
           tab_strip->GetActiveTab() == restored_tabs.at(expected[10]);
  }));
  expect_projected_pair(*rail_snapshot, expected[6], expected[7], "stacked",
                        0.25, expected[10]);
  expect_projected_pair(*rail_snapshot, expected[8], expected[9], "sideBySide",
                        0.75, "");

  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(restored_browser);
  ASSERT_TRUE(browser_view);
  MultiContentsView* multi_contents = browser_view->multi_contents_view();
  ASSERT_TRUE(multi_contents);
  ASSERT_TRUE(WaitForCondition([&]() {
    return multi_contents->IsInSplitView() &&
           multi_contents->start_contents_view_for_testing()->web_contents() ==
               restored_tabs.at(expected[6])->GetContents() &&
           multi_contents->end_contents_view_for_testing()->web_contents() ==
               restored_tabs.at(expected[7])->GetContents();
  }));
  const gfx::Rect start_bounds =
      multi_contents->start_contents_view_for_testing()->GetBoundsInScreen();
  const gfx::Rect end_bounds =
      multi_contents->end_contents_view_for_testing()->GetBoundsInScreen();
  EXPECT_FALSE(start_bounds.IsEmpty());
  EXPECT_FALSE(end_bounds.IsEmpty());
  EXPECT_LE(start_bounds.bottom(), end_bounds.y());

  content::WebContents* three_contents =
      restored_tabs.at(expected[6])->GetContents();
  content::WebContents* four_contents =
      restored_tabs.at(expected[7])->GetContents();
  ASSERT_TRUE(content::ExecJs(three_contents, R"JS(
        window.limitlessRestoreKeys = [];
        const input = document.createElement('input');
        input.id = 'limitless-restore-input';
        input.addEventListener('keydown', event => {
          window.limitlessRestoreKeys.push(event.key);
        });
        document.body.appendChild(input);
        input.focus();
      )JS"));
  ASSERT_TRUE(content::ExecJs(four_contents, R"JS(
        window.limitlessRestoreKeys = [];
        const input = document.createElement('input');
        input.id = 'limitless-restore-input';
        input.addEventListener('keydown', event => {
          window.limitlessRestoreKeys.push(event.key);
        });
        document.body.appendChild(input);
        input.focus();
      )JS"));
  ASSERT_TRUE(limitless::PerformSwiftUIAccessibilityActionForTesting(
      rail,
      "limitless-tab-" +
          base::NumberToString(
              restored_tabs.at(expected[7])->GetHandle().raw_value()),
      limitless::SwiftUIAccessibilityActionForTesting::kPress));
  ASSERT_TRUE(WaitForCondition([&]() {
    content::RenderWidgetHostView* renderer_view =
        four_contents->GetRenderWidgetHostView();
    return tab_strip->GetActiveTab() == restored_tabs.at(expected[7]) &&
           renderer_view && renderer_view->HasFocus() &&
           content::EvalJs(four_contents, "document.activeElement.id") ==
               "limitless-restore-input";
  }));
  SendRendererKey(restored_browser, ui::VKEY_R, ui::EF_NONE, u'r');
  ASSERT_TRUE(WaitForCondition([&]() {
    return content::EvalJs(
               four_contents,
               "document.querySelector('#limitless-restore-input').value") ==
           "r";
  }));
  EXPECT_EQ(0, content::EvalJs(three_contents, "limitlessRestoreKeys.length"));
  EXPECT_EQ(1, content::EvalJs(four_contents, "limitlessRestoreKeys.length"));
  EXPECT_EQ("r", content::EvalJs(four_contents, "limitlessRestoreKeys[0]"));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    PRE_ReconcilesHostileWorkspaceMetadataAcrossCleanBrowserRestart) {
  SessionStartupPref::SetStartupPref(
      browser()->GetProfile(), SessionStartupPref(SessionStartupPref::LAST));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  for (const std::string& path :
       std::vector<std::string>{"/two", "/three", "/four", "/five"}) {
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser(), https_server_.GetURL("a.test", path),
        WindowOpenDisposition::NEW_BACKGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  }

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  tabs::TabInterface* one = FindTabWithPath(browser(), "/one");
  tabs::TabInterface* two = FindTabWithPath(browser(), "/two");
  tabs::TabInterface* three = FindTabWithPath(browser(), "/three");
  tabs::TabInterface* four = FindTabWithPath(browser(), "/four");
  tabs::TabInterface* five = FindTabWithPath(browser(), "/five");
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  ASSERT_TRUE(four);
  ASSERT_TRUE(five);
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  auto snapshot = WaitForRailTabs(rail, 5);
  ASSERT_TRUE(snapshot);
  const std::string window_identifier = snapshot->window_identifier;
  const std::string default_workspace = snapshot->workspace_identifiers[0];
  const std::optional<std::string> focus_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(rail, u"Hostile");
  ASSERT_TRUE(focus_workspace);
  const tabs::TabHandle created_workspace_tab =
      tab_strip->GetActiveTab()->GetHandle();
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      rail, one->GetHandle().raw_value(), *focus_workspace));
  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      rail, two->GetHandle().raw_value(), *focus_workspace));
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, *focus_workspace));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers.size() == 3;
  }));
  ASSERT_TRUE(limitless::CloseSwiftUITabForTesting(
      rail, created_workspace_tab.raw_value()));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return !created_workspace_tab.Get() && snapshot &&
           snapshot->tab_identifiers.size() == 2;
  }));

  const std::string one_identifier = ProductIdentifierForTab(browser(), one);
  const std::string two_identifier = ProductIdentifierForTab(browser(), two);
  const std::string four_identifier = ProductIdentifierForTab(browser(), four);
  const std::string five_identifier = ProductIdentifierForTab(browser(), five);
  const auto create_split = [&](tabs::TabInterface* first,
                                tabs::TabInterface* second) {
    const std::string menu =
        "limitless-tab-split-" + ProductIdentifierForTab(browser(), first);
    const auto action =
        limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
            rail, menu,
            menu + "-with-" + ProductIdentifierForTab(browser(), second));
    return action && action->press_invoked && WaitForCondition([&]() {
             return first->IsSplit() && second->IsSplit() &&
                    first->GetSplit() == second->GetSplit();
           });
  };
  ASSERT_TRUE(create_split(one, two));
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->tab_identifiers.size() == 3;
  }));
  ASSERT_TRUE(create_split(four, five));
  ASSERT_TRUE(limitless::ActivateSwiftUITabForTesting(
      rail, three->GetHandle().raw_value()));
  ASSERT_TRUE(
      WaitForCondition([&]() { return tab_strip->GetActiveTab() == three; }));

  const std::string duplicate_identifier =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string oracle =
      base::JoinString({"v1", window_identifier, default_workspace,
                        *focus_workspace, one_identifier, two_identifier,
                        four_identifier, five_identifier, duplicate_identifier},
                       "\n") +
      "\n";
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(browser()->GetProfile()->GetPath().AppendASCII(
                                    "LimitlessHostileSplitRestoreOracle"),
                                oracle));
  }

  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(browser());
  ASSERT_TRUE(workspace_controller);
  workspace_controller->PersistAll();
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(browser()->GetProfile());
  ASSERT_TRUE(session_service);
  session_service->ResetFromCurrentBrowsers();
  const auto override_tab_data = [&](tabs::TabInterface* tab,
                                     const std::string& value) {
    sessions::SessionTabHelper* helper =
        sessions::SessionTabHelper::FromWebContents(tab->GetContents());
    CHECK(helper);
    session_service->AddTabExtraData(browser()->GetSessionID(),
                                     helper->session_id(),
                                     limitless::kWorkspaceTabSessionKey, value);
  };
  override_tab_data(two, "{malformed");
  override_tab_data(four, BuildWorkspaceTabSessionData(duplicate_identifier,
                                                       default_workspace));
  override_tab_data(five, BuildWorkspaceTabSessionData(duplicate_identifier,
                                                       default_workspace));
  SessionServiceTestHelper(session_service).SaveNow();
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    ReconcilesHostileWorkspaceMetadataAcrossCleanBrowserRestart) {
  std::string oracle;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(
        base::ReadFileToString(browser()->GetProfile()->GetPath().AppendASCII(
                                   "LimitlessHostileSplitRestoreOracle"),
                               &oracle));
  }
  const std::vector<std::string> expected = base::SplitString(
      oracle, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(9u, expected.size());
  ASSERT_EQ("v1", expected[0]);

  Browser* restored_browser = nullptr;
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        restored_browser =
            FindBrowserWithWorkspaceWindowIdentifier(expected[1]);
        if (!restored_browser) {
          return false;
        }
        limitless::WorkspaceController* controller =
            limitless::WorkspaceController::From(restored_browser);
        return controller &&
               !controller->GetSnapshot().session_restore_in_progress &&
               restored_browser->GetTabStripModel()->count() >= 5;
      },
      base::Seconds(15)));
  limitless::WorkspaceController* workspace_controller =
      limitless::WorkspaceController::From(restored_browser);
  ASSERT_TRUE(workspace_controller);
  tabs::TabInterface* one =
      FindTabWithProductIdentifier(restored_browser, expected[4]);
  tabs::TabInterface* two = FindTabWithPath(restored_browser, "/two");
  tabs::TabInterface* three = FindTabWithPath(restored_browser, "/three");
  tabs::TabInterface* four = FindTabWithPath(restored_browser, "/four");
  tabs::TabInterface* five = FindTabWithPath(restored_browser, "/five");
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  ASSERT_TRUE(four);
  ASSERT_TRUE(five);
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        return !one->IsSplit() && !two->IsSplit() && four->GetSplit() &&
               five->GetSplit() && four->GetSplit() == five->GetSplit();
      },
      base::Seconds(15)));

  const std::string restored_two_identifier =
      ProductIdentifierForTab(restored_browser, two);
  const std::string restored_four_identifier =
      ProductIdentifierForTab(restored_browser, four);
  const std::string restored_five_identifier =
      ProductIdentifierForTab(restored_browser, five);
  EXPECT_EQ(expected[3], workspace_controller->GetWorkspaceIdentifierForTab(
                             one->GetHandle()));
  EXPECT_EQ(expected[2], workspace_controller->GetWorkspaceIdentifierForTab(
                             two->GetHandle()));
  EXPECT_NE(expected[5], restored_two_identifier);
  EXPECT_TRUE(base::Uuid::ParseLowercase(restored_two_identifier).is_valid());
  EXPECT_EQ(nullptr,
            FindTabWithProductIdentifier(restored_browser, expected[5]));
  EXPECT_FALSE(one->IsSplit());
  EXPECT_FALSE(two->IsSplit());

  EXPECT_EQ(expected[8], restored_four_identifier);
  EXPECT_NE(expected[8], restored_five_identifier);
  EXPECT_NE(expected[6], restored_four_identifier);
  EXPECT_NE(expected[7], restored_five_identifier);
  EXPECT_TRUE(base::Uuid::ParseLowercase(restored_five_identifier).is_valid());
  EXPECT_EQ(expected[2], workspace_controller->GetWorkspaceIdentifierForTab(
                             four->GetHandle()));
  EXPECT_EQ(expected[2], workspace_controller->GetWorkspaceIdentifierForTab(
                             five->GetHandle()));
  size_t unrelated_live_tabs = 0;
  for (tabs::TabInterface* tab :
       TabListInterface::From(restored_browser)->GetAllTabs()) {
    if (tab == one || tab == two || tab == three || tab == four ||
        tab == five) {
      continue;
    }
    ++unrelated_live_tabs;
    EXPECT_FALSE(tab->IsSplit());
    EXPECT_NE(restored_browser->GetTabStripModel()->GetActiveTab(), tab);
    const std::string product_identifier =
        ProductIdentifierForTab(restored_browser, tab);
    const base::Uuid parsed_identifier =
        base::Uuid::ParseLowercase(product_identifier);
    EXPECT_TRUE(parsed_identifier.is_valid());
    EXPECT_EQ(product_identifier, parsed_identifier.AsLowercaseString());
    EXPECT_EQ(expected.end(), std::ranges::find(expected, product_identifier));
    EXPECT_EQ(expected[2], workspace_controller->GetWorkspaceIdentifierForTab(
                               tab->GetHandle()));
    const GURL& visible_url = tab->GetContents()->GetVisibleURL();
    EXPECT_TRUE(visible_url.SchemeIs("chrome") ||
                visible_url.SchemeIs("about"));
  }
  EXPECT_LE(unrelated_live_tabs, 1u);
  ASSERT_TRUE(four->GetSplit());
  ASSERT_TRUE(five->GetSplit());
  const split_tabs::SplitTabData* duplicate_pair =
      restored_browser->GetTabStripModel()->GetSplitData(*four->GetSplit());
  ASSERT_TRUE(duplicate_pair);
  EXPECT_EQ((std::vector<tabs::TabInterface*>{four, five}),
            duplicate_pair->ListTabs());

  views::View* rail = GetSwiftUITabRail(restored_browser);
  ASSERT_TRUE(rail);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> snapshot;
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->active_workspace_identifier == expected[2] &&
           snapshot->tab_identifiers.size() == 4 + unrelated_live_tabs &&
           snapshot->split_pair_identifiers.size() == 1 &&
           snapshot->split_first_tab_identifiers ==
               std::vector<std::string>{restored_four_identifier} &&
           snapshot->split_second_tab_identifiers ==
               std::vector<std::string>{restored_five_identifier};
  }));
  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(rail, expected[3]));
  ASSERT_TRUE(WaitForCondition([&]() {
    snapshot = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->active_workspace_identifier == expected[3] &&
           snapshot->tab_identifiers.size() == 1 &&
           snapshot->split_pair_identifiers.empty();
  }));
  EXPECT_FALSE(one->IsSplit());
  EXPECT_FALSE(two->IsSplit());
  EXPECT_TRUE(four->IsSplit());
  EXPECT_TRUE(five->IsSplit());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       PRE_RestoresWorkspacesAcrossCleanBrowserRestart) {
  SessionStartupPref::SetStartupPref(
      browser()->GetProfile(), SessionStartupPref(SessionStartupPref::LAST));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/one")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("b.test", "/two"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("c.test", "/three"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_server_.GetURL("d.test", "/keys"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  TabListInterface* source_tabs = TabListInterface::From(browser());
  ASSERT_TRUE(source_tabs);
  tabs::TabInterface* one = FindTabWithPath(browser(), "/one");
  tabs::TabInterface* two = FindTabWithPath(browser(), "/two");
  tabs::TabInterface* three = FindTabWithPath(browser(), "/three");
  tabs::TabInterface* closed_before_shutdown =
      FindTabWithPath(browser(), "/keys");
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  ASSERT_TRUE(closed_before_shutdown);
  const tabs::TabHandle closed_before_shutdown_handle =
      closed_before_shutdown->GetHandle();
  views::View* source_rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(source_rail);
  auto source_snapshot = WaitForRailTabs(source_rail, 4);
  ASSERT_TRUE(source_snapshot);
  const std::string source_window_identifier =
      source_snapshot->window_identifier;
  const std::string source_default_workspace =
      source_snapshot->workspace_identifiers[0];
  const std::optional<std::string> source_focus_workspace =
      limitless::CreateSwiftUIWorkspaceForTesting(source_rail, u"Focus");
  ASSERT_TRUE(source_focus_workspace);
  tabs::TabInterface* source_focus_new_tab = source_tabs->GetActiveTab();
  ASSERT_TRUE(source_focus_new_tab);
  const tabs::TabHandle source_focus_new_tab_handle =
      source_focus_new_tab->GetHandle();
  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(
      source_rail, source_default_workspace));
  ASSERT_TRUE(limitless::MoveSwiftUITabToWorkspaceForTesting(
      source_rail, two->GetHandle().raw_value(), *source_focus_workspace));
  ASSERT_TRUE(limitless::SwitchSwiftUIWorkspaceForTesting(
      source_rail, *source_focus_workspace));
  ASSERT_TRUE(limitless::ActivateSwiftUITabForTesting(
      source_rail, two->GetHandle().raw_value()));
  ASSERT_TRUE(limitless::CloseSwiftUITabForTesting(
      source_rail, source_focus_new_tab_handle.raw_value()));
  ASSERT_TRUE(WaitForCondition(
      [&]() { return source_focus_new_tab_handle.Get() == nullptr; }));

  Browser* destination_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(destination_browser);
  destination_browser->GetWindow()->Show();
  views::View* destination_rail = GetSwiftUITabRail(destination_browser);
  ASSERT_TRUE(destination_rail);
  auto destination_snapshot = WaitForRailTabs(destination_rail, 1);
  ASSERT_TRUE(destination_snapshot);
  const std::string destination_window_identifier =
      destination_snapshot->window_identifier;
  const std::string destination_default_workspace =
      destination_snapshot->workspace_identifiers[0];
  const tabs::TabHandle destination_initial_handle =
      destination_browser->GetTabStripModel()->GetActiveTab()->GetHandle();
  source_tabs->MoveTabToWindow(three->GetHandle(),
                               destination_browser->GetSessionID(), 1);
  ASSERT_TRUE(WaitForCondition([&]() {
    destination_snapshot =
        limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
    return destination_snapshot &&
           destination_snapshot->tab_identifiers.size() == 2;
  }));
  ASSERT_TRUE(limitless::ActivateSwiftUITabForTesting(
      destination_rail, three->GetHandle().raw_value()));
  ASSERT_TRUE(limitless::CloseSwiftUITabForTesting(
      destination_rail, destination_initial_handle.raw_value()));
  ASSERT_TRUE(WaitForCondition(
      [&]() { return destination_initial_handle.Get() == nullptr; }));

  browser()->GetTabStripModel()->CloseWebContentsAt(
      browser()->GetTabStripModel()->GetIndexOfWebContents(
          closed_before_shutdown->GetContents()),
      TabCloseTypes::CLOSE_NONE);
  ASSERT_TRUE(WaitForCondition(
      [&]() { return closed_before_shutdown_handle.Get() == nullptr; }));
  ASSERT_TRUE(WaitForCondition([&]() {
    return browser()->GetTabStripModel()->count() == 2 &&
           browser()->GetTabStripModel()->GetActiveTab() == two &&
           destination_browser->GetTabStripModel()->count() == 1 &&
           destination_browser->GetTabStripModel()->GetActiveTab() == three;
  }));

  limitless::WorkspaceController* source_controller =
      limitless::WorkspaceController::From(browser());
  limitless::WorkspaceController* destination_controller =
      limitless::WorkspaceController::From(destination_browser);
  ASSERT_TRUE(source_controller);
  ASSERT_TRUE(destination_controller);
  const std::optional<std::string> one_data =
      source_controller->SerializeTabForSession(one->GetHandle());
  const std::optional<std::string> two_data =
      source_controller->SerializeTabForSession(two->GetHandle());
  const std::optional<std::string> three_data =
      destination_controller->SerializeTabForSession(three->GetHandle());
  ASSERT_TRUE(one_data);
  ASSERT_TRUE(two_data);
  ASSERT_TRUE(three_data);
  std::string oracle =
      source_window_identifier + "\n" + source_default_workspace + "\n" +
      *source_focus_workspace + "\n" +
      ReadWorkspaceTabSessionData(*one_data).first + "\n" +
      ReadWorkspaceTabSessionData(*two_data).first + "\n" +
      destination_window_identifier + "\n" + destination_default_workspace +
      "\n" + ReadWorkspaceTabSessionData(*three_data).first + "\n";
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(browser()->GetProfile()->GetPath().AppendASCII(
                                    "LimitlessWorkspaceRestoreOracle"),
                                oracle));
  }

  source_controller->PersistAll();
  destination_controller->PersistAll();
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(browser()->GetProfile());
  ASSERT_TRUE(session_service);
  session_service->ResetFromCurrentBrowsers();
  SessionServiceTestHelper(session_service).SaveNow();
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       RestoresWorkspacesAcrossCleanBrowserRestart) {
  std::string oracle;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(
        base::ReadFileToString(browser()->GetProfile()->GetPath().AppendASCII(
                                   "LimitlessWorkspaceRestoreOracle"),
                               &oracle));
  }
  const std::vector<std::string> expected = base::SplitString(
      oracle, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(8u, expected.size());

  Browser* source_browser = nullptr;
  Browser* destination_browser = nullptr;
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        source_browser = FindBrowserWithWorkspaceWindowIdentifier(expected[0]);
        destination_browser =
            FindBrowserWithWorkspaceWindowIdentifier(expected[5]);
        return source_browser && destination_browser;
      },
      base::Seconds(15)));
  ASSERT_NE(source_browser, destination_browser);
  views::View* source_rail = GetSwiftUITabRail(source_browser);
  views::View* destination_rail = GetSwiftUITabRail(destination_browser);
  ASSERT_TRUE(source_rail);
  ASSERT_TRUE(destination_rail);

  std::optional<limitless::SwiftUITabRailSnapshotForTesting> source_snapshot;
  std::optional<limitless::SwiftUITabRailSnapshotForTesting>
      destination_snapshot;
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        source_snapshot =
            limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
        destination_snapshot =
            limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
        return source_snapshot && destination_snapshot &&
               source_snapshot->workspace_identifiers.size() == 2 &&
               destination_snapshot->workspace_identifiers.size() == 1;
      },
      base::Seconds(15)));
  EXPECT_EQ(expected[0], source_snapshot->window_identifier);
  EXPECT_EQ((std::vector<std::string>{expected[1], expected[2]}),
            source_snapshot->workspace_identifiers);
  EXPECT_EQ((std::vector<std::u16string>{u"Workspace", u"Focus"}),
            source_snapshot->workspace_names);
  EXPECT_EQ(expected[2], *source_snapshot->active_workspace_identifier);
  EXPECT_EQ(expected[5], destination_snapshot->window_identifier);
  EXPECT_EQ((std::vector<std::string>{expected[6]}),
            destination_snapshot->workspace_identifiers);

  tabs::TabInterface* one = FindTabWithPath(source_browser, "/one");
  tabs::TabInterface* two = FindTabWithPath(source_browser, "/two");
  tabs::TabInterface* three = FindTabWithPath(destination_browser, "/three");
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  ASSERT_TRUE(three);
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        source_snapshot =
            limitless::GetSwiftUITabRailSnapshotForTesting(source_rail);
        destination_snapshot =
            limitless::GetSwiftUITabRailSnapshotForTesting(destination_rail);
        return source_browser->GetTabStripModel()->GetActiveTab() == two &&
               destination_browser->GetTabStripModel()->GetActiveTab() ==
                   three &&
               source_snapshot && destination_snapshot &&
               source_snapshot->active_tab_identifier ==
                   two->GetHandle().raw_value() &&
               destination_snapshot->active_tab_identifier ==
                   three->GetHandle().raw_value();
      },
      base::Seconds(15)));

  limitless::WorkspaceController* source_controller =
      limitless::WorkspaceController::From(source_browser);
  limitless::WorkspaceController* destination_controller =
      limitless::WorkspaceController::From(destination_browser);
  ASSERT_TRUE(source_controller);
  ASSERT_TRUE(destination_controller);
  ASSERT_TRUE(source_controller->SerializeTabForSession(one->GetHandle()));
  ASSERT_TRUE(source_controller->SerializeTabForSession(two->GetHandle()));
  ASSERT_TRUE(
      destination_controller->SerializeTabForSession(three->GetHandle()));
  EXPECT_EQ(expected[3],
            ReadWorkspaceTabSessionData(
                *source_controller->SerializeTabForSession(one->GetHandle()))
                .first);
  EXPECT_EQ(expected[4],
            ReadWorkspaceTabSessionData(
                *source_controller->SerializeTabForSession(two->GetHandle()))
                .first);
  EXPECT_EQ(
      expected[7],
      ReadWorkspaceTabSessionData(
          *destination_controller->SerializeTabForSession(three->GetHandle()))
          .first);
  EXPECT_NE(expected[3], expected[4]);
  EXPECT_NE(expected[3], expected[7]);
  EXPECT_NE(expected[4], expected[7]);

  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(source_rail, expected[1]));
  ASSERT_TRUE(WaitForCondition([&]() {
    return source_browser->GetTabStripModel()->GetActiveTab() == one;
  }));
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(source_rail, expected[2]));
  ASSERT_TRUE(WaitForCondition([&]() {
    return source_browser->GetTabStripModel()->GetActiveTab() == two;
  }));

  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser_window) {
        Browser* candidate = browser_window->GetBrowserForMigrationOnly();
        EXPECT_TRUE(!candidate || !FindTabWithPath(candidate, "/keys"));
        return true;
      });
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserUnifiedTest,
                       SuppressesUpstreamTabStripsAcrossModeChanges) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  ASSERT_TRUE(browser_view->limitless_swiftui_rail_for_testing());
  EXPECT_EQ(nullptr, browser_view->horizontal_tab_strip_for_testing());
  TabStripRegionView* suppressed_horizontal = browser_view->tab_strip_view();
  ASSERT_TRUE(suppressed_horizontal);
  EXPECT_EQ(nullptr, suppressed_horizontal->GetTabStripView());

  tabs::VerticalTabStripStateController* controller =
      tabs::VerticalTabStripStateController::From(browser());
  ASSERT_TRUE(controller);
  EXPECT_FALSE(controller->ShouldDisplayVerticalTabs());
  controller->SetVerticalTabsEnabled(true);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return controller->ShouldDisplayVerticalTabs(); }));

  EXPECT_EQ(nullptr, browser_view->horizontal_tab_strip_for_testing());
  EXPECT_EQ(suppressed_horizontal, browser_view->tab_strip_view());
  EXPECT_EQ(nullptr, suppressed_horizontal->GetTabStripView());
  ASSERT_TRUE(browser_view->vertical_tab_strip_region_view_for_testing());
  EXPECT_EQ(nullptr, browser_view->vertical_tab_strip_region_view_for_testing()
                         ->GetTabStripView());

  std::vector<views::View*> panes;
  browser_view->GetAccessiblePanes(&panes);
  EXPECT_EQ(panes.end(), std::ranges::find(panes, suppressed_horizontal));
  EXPECT_EQ(
      panes.end(),
      std::ranges::find(
          panes, browser_view->vertical_tab_strip_region_view_for_testing()));
  EXPECT_NE(panes.end(),
            std::ranges::find(
                panes, browser_view->limitless_swiftui_rail_for_testing()));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       ScopesRailStateAndIntentsToOwningWindow) {
  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  second_browser->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      second_browser, https_server_.GetURL("b.test", "/two")));

  views::View* first_rail = GetSwiftUITabRail(browser());
  views::View* second_rail = GetSwiftUITabRail(second_browser);
  ASSERT_TRUE(first_rail);
  ASSERT_TRUE(second_rail);
  auto first_snapshot = WaitForRailTabs(first_rail, 1);
  auto second_snapshot = WaitForRailTabs(second_rail, 1);
  ASSERT_TRUE(first_snapshot);
  ASSERT_TRUE(second_snapshot);
  ASSERT_NE(first_snapshot->tab_identifiers, second_snapshot->tab_identifiers);

  const int32_t foreign_identifier = second_snapshot->tab_identifiers[0];
  tabs::TabInterface* second_tab =
      second_browser->GetTabStripModel()->GetActiveTab();
  ASSERT_TRUE(second_tab);
  EXPECT_FALSE(
      limitless::ActivateSwiftUITabForTesting(first_rail, foreign_identifier));
  EXPECT_FALSE(
      limitless::CloseSwiftUITabForTesting(first_rail, foreign_identifier));
  EXPECT_EQ(second_tab, second_browser->GetTabStripModel()->GetActiveTab());
  EXPECT_EQ(1, browser()->GetTabStripModel()->count());
  EXPECT_EQ(1, second_browser->GetTabStripModel()->count());

  CloseBrowserSynchronously(second_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       LeaderRendererRouteRunsHerdrTabAndSidebarMap) {
  base::CommandLine absent_policy(base::CommandLine::NO_PROGRAM);
  EXPECT_TRUE(limitless::AreLeaderShortcutsEnabled(absent_policy));
  base::CommandLine enabled_policy(base::CommandLine::NO_PROGRAM);
  enabled_policy.AppendSwitchASCII(limitless::kLeaderShortcutsSwitch,
                                   "enabled");
  EXPECT_TRUE(limitless::AreLeaderShortcutsEnabled(enabled_policy));
  base::CommandLine disabled_policy(base::CommandLine::NO_PROGRAM);
  disabled_policy.AppendSwitchASCII(limitless::kLeaderShortcutsSwitch,
                                    "disabled");
  EXPECT_FALSE(limitless::AreLeaderShortcutsEnabled(disabled_policy));
  base::CommandLine unrecognized_policy(base::CommandLine::NO_PROGRAM);
  unrecognized_policy.AppendSwitchASCII(limitless::kLeaderShortcutsSwitch,
                                        "invalid");
  EXPECT_FALSE(limitless::AreLeaderShortcutsEnabled(unrecognized_policy));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/keys")));
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  content::WebContents* key_contents = tab_strip->GetActiveWebContents();
  ASSERT_TRUE(key_contents);
  ASSERT_TRUE(ResetKeyEvidence(key_contents, false));
  while (tab_strip->count() < 10) {
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, false);
  }
  ASSERT_EQ(10, tab_strip->count());

  std::vector<tabs::TabInterface*> tabs;
  std::vector<content::WebContents*> contents;
  for (int index = 0; index < tab_strip->count(); ++index) {
    tabs::TabInterface* tab = tab_strip->GetTabAtIndex(index);
    ASSERT_TRUE(tab);
    tabs.push_back(tab);
    contents.push_back(tab->GetContents());
  }

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = GetLeaderSnapshot(rail);
    return snapshot && snapshot->leader_native_delegate_requested;
  }));
  EXPECT_TRUE(snapshot->leader_enabled);
  EXPECT_FALSE(snapshot->leader_armed);
  EXPECT_TRUE(snapshot->leader_native_delegate_requested)
      << "OnWindowInitialized must bind the native hook to this exact host";

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = GetLeaderSnapshot(rail);
    return snapshot && snapshot->leader_armed;
  }));
  EXPECT_TRUE(snapshot->leader_armed);
  EXPECT_EQ(u"Leader: n/p tabs, 1-9 jump, Shift+X close, b sidebar",
            snapshot->leader_status_text);

  SendRendererKey(browser(), ui::VKEY_P, ui::EF_NONE, u'p');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == tabs[9] && !IsLeaderArmed(rail);
  })) << "Previous must wrap from the first tab to the last tab";
  EXPECT_EQ(contents[9], tabs[9]->GetContents());
  EXPECT_EQ(0, KeyEventCount(key_contents, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  EXPECT_EQ(0, KeyEventCount(key_contents, ui::VKEY_P, ui::EF_NONE));
  EXPECT_TRUE(KeyInputValue(key_contents).empty());

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == tabs[0] && !IsLeaderArmed(rail);
  })) << "Next must wrap from the last tab to the first tab";
  EXPECT_EQ(contents[0], tabs[0]->GetContents());

  constexpr ui::KeyboardCode kIndexedTabKeys[] = {
      ui::VKEY_1, ui::VKEY_2, ui::VKEY_3, ui::VKEY_4, ui::VKEY_5,
      ui::VKEY_6, ui::VKEY_7, ui::VKEY_8, ui::VKEY_9};
  for (size_t index = 0; index < std::size(kIndexedTabKeys); ++index) {
    SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
    ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
    SendRendererKey(browser(), kIndexedTabKeys[index], ui::EF_NONE,
                    static_cast<char16_t>(u'1' + index));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return tab_strip->GetActiveTab() == tabs[index] && !IsLeaderArmed(rail);
    }));
    EXPECT_EQ(contents[index], tabs[index]->GetContents());
  }
  EXPECT_EQ(tabs[8], tab_strip->GetActiveTab());
  EXPECT_NE(tabs.back(), tab_strip->GetActiveTab())
      << "Leader 9 is an exact index, not Chromium's last-tab shortcut";

  const tabs::TabHandle closing_handle = tabs[8]->GetHandle();
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_X, ui::EF_SHIFT_DOWN, u'X');
  ASSERT_TRUE(base::test::RunUntil([&]() { return tab_strip->count() == 9; }));
  EXPECT_EQ(nullptr, closing_handle.Get());
  for (size_t index = 0; index < contents.size(); ++index) {
    if (index != 8) {
      EXPECT_EQ(contents[index], tabs[index]->GetContents());
    }
  }

  ASSERT_TRUE(rail->GetVisible());
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_NONE, u'b');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return !rail->GetVisible() && !IsLeaderArmed(rail); }));

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return rail->GetVisible() && IsLeaderArmed(rail); }));
  EXPECT_TRUE(rail->GetVisible())
      << "The hidden sidebar temporarily reveals the armed Leader status";
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_NONE, u'b');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return rail->GetVisible() && !IsLeaderArmed(rail); }));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       LeaderCancelsExpiresAndResetsUnknownKeys) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/keys")));
  content::WebContents* key_contents =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(key_contents);
  ASSERT_TRUE(ResetKeyEvidence(key_contents, true));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, false);
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* initial_tab = tab_strip->GetActiveTab();
  ASSERT_TRUE(initial_tab);

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return KeyInputValue(key_contents) == "n"; }));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());
  EXPECT_FALSE(IsLeaderArmed(rail));
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_N, ui::EF_NONE));

  ASSERT_TRUE(ResetKeyEvidence(key_contents, false));
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_ESCAPE);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());
  EXPECT_TRUE(KeyInputValue(key_contents).empty());

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_Q, ui::EF_NONE, u'q');
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());
  EXPECT_TRUE(KeyInputValue(key_contents).empty());
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_Q, ui::EF_NONE))
      << "An unknown second key resets Leader and falls through to the page";

  struct UnclaimedPaneKey {
    ui::KeyboardCode key_code;
    char16_t character;
  };
  constexpr UnclaimedPaneKey kUnclaimedPaneKeys[] = {
      {ui::VKEY_H, u'h'}, {ui::VKEY_J, u'j'}, {ui::VKEY_K, u'k'},
      {ui::VKEY_L, u'l'}, {ui::VKEY_V, u'v'}, {ui::VKEY_OEM_MINUS, u'-'}};
  for (const UnclaimedPaneKey& key : kUnclaimedPaneKeys) {
    SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
    ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
    SendRendererKey(browser(), key.key_code, ui::EF_NONE, key.character);
    ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
    EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());
    EXPECT_EQ(2, tab_strip->count());
    EXPECT_EQ(1, KeyEventCount(key_contents, key.key_code, ui::EF_NONE))
        << "Future pane keys remain unclaimed until their commands ship";
  }

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_Q, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_Q, ui::EF_CONTROL_DOWN));

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_V, ui::EF_ALT_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_V, ui::EF_ALT_DOWN));

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_9, ui::EF_NONE, u'9');
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab())
      << "An unavailable 1-based tab index must not wrap or mutate state";
  EXPECT_TRUE(KeyInputValue(key_contents).empty());
  EXPECT_EQ(0, KeyEventCount(key_contents, ui::VKEY_9, ui::EF_NONE));

  const int tab_count_before_plain_x = tab_strip->count();
  const tabs::TabHandle initial_handle = initial_tab->GetHandle();
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_X, ui::EF_NONE, u'x');
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(tab_count_before_plain_x, tab_strip->count());
  EXPECT_EQ(initial_tab, initial_handle.Get())
      << "Only Shift+X is the close command";
  EXPECT_TRUE(KeyInputValue(key_contents).empty());
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_X, ui::EF_NONE))
      << "Plain x is unrelated input, not the Shift+X close command";

  EXPECT_GT(limitless::GetLeaderShortcutTimeoutForTesting(), base::TimeDelta());
  EXPECT_LE(limitless::GetLeaderShortcutTimeoutForTesting(), base::Seconds(2));
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(IsLeaderArmed(rail));
  ASSERT_TRUE(limitless::ExpireSwiftUILeaderForTesting(rail));
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  ASSERT_TRUE(FocusKeyEvidenceInput(key_contents));
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return KeyInputValue(key_contents) == "n"; }));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  OmniboxViewViews* omnibox =
      browser_view->GetLocationBarView()->omnibox_view();
  ASSERT_TRUE(omnibox);
  ASSERT_TRUE(ResetKeyEvidence(key_contents, false));
  const GURL committed_url = key_contents->GetLastCommittedURL();
  NSWindow* native_window =
      browser()->GetWindow()->GetNativeWindow().GetNativeNSWindow();
  ASSERT_TRUE(native_window);
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()))
      << "Chromium's macOS activation helper must foreground the exact browser";
  ASSERT_TRUE(WaitForCondition([&]() { return native_window.isKeyWindow; }))
      << "The renderer redispatch path requires its exact window to stay key";
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_L, ui::EF_COMMAND_DOWN, std::nullopt,
                  true, false);
  const bool leader_disarmed_from_native_key_down = !IsLeaderArmed(rail);
  if (!leader_disarmed_from_native_key_down) {
    SendRendererNativeKeyUp(browser(), ui::VKEY_L, ui::EF_COMMAND_DOWN);
  }
  ASSERT_TRUE(leader_disarmed_from_native_key_down)
      << "The attached native keydown must synchronously traverse the "
         "renderer route and disarm Leader";
  const bool renderer_cmd_l_succeeded = WaitForCondition(
      [&]() { return omnibox->HasFocus() && omnibox->IsSelectAll(); });
  SendRendererNativeKeyUp(browser(), ui::VKEY_L, ui::EF_COMMAND_DOWN);
  ASSERT_TRUE(renderer_cmd_l_succeeded)
      << "The renderer route must retain Cmd+L after cancelling Leader";
  EXPECT_EQ(committed_url, key_contents->GetLastCommittedURL());
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());

  browser_view->RestoreFocus();
  ASSERT_TRUE(ResetKeyEvidence(key_contents, true));
  SendRendererKey(browser(), ui::VKEY_P, ui::EF_NONE, u'p');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return KeyInputValue(key_contents) == "p"; }));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       LeaderViewsControlsShareTheWindowController) {
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, false);
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* first_tab = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* second_tab = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(first_tab);
  ASSERT_TRUE(second_tab);

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  OmniboxViewViews* omnibox =
      browser_view->GetLocationBarView()->omnibox_view();
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(omnibox);
  ASSERT_TRUE(rail);

  const std::u16string kSentinel = u"omnibox sentinel";
  omnibox->SetUserText(kSentinel, false);
  EXPECT_TRUE(
      SendViewsControlKey(browser(), omnibox, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  EXPECT_EQ(kSentinel, omnibox->GetText());
  SendViewsControlKey(browser(), omnibox, ui::VKEY_Q);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(kSentinel, omnibox->GetText());
  EXPECT_EQ(omnibox,
            browser_view->GetWidget()->GetFocusManager()->GetFocusedView());
  EXPECT_EQ(first_tab, tab_strip->GetActiveTab());

  EXPECT_TRUE(
      SendViewsControlKey(browser(), omnibox, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  EXPECT_TRUE(SendViewsControlKey(browser(), omnibox, ui::VKEY_ESCAPE));
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  EXPECT_EQ(kSentinel, omnibox->GetText());

  EXPECT_TRUE(
      SendViewsControlKey(browser(), omnibox, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  ASSERT_TRUE(limitless::ExpireSwiftUILeaderForTesting(rail));
  ASSERT_TRUE(base::test::RunUntil([&]() { return !IsLeaderArmed(rail); }));
  SendViewsControlKey(browser(), omnibox, ui::VKEY_N);
  EXPECT_EQ(kSentinel, omnibox->GetText());
  EXPECT_EQ(first_tab, tab_strip->GetActiveTab())
      << "The expired Views-control prefix must not retain a tab command";

  EXPECT_TRUE(
      SendViewsControlKey(browser(), omnibox, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  EXPECT_TRUE(SendViewsControlKey(browser(), omnibox, ui::VKEY_N));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == second_tab && !IsLeaderArmed(rail);
  }));

  EXPECT_TRUE(
      SendViewsControlKey(browser(), rail, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  EXPECT_TRUE(SendViewsControlKey(browser(), rail, ui::VKEY_P));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == first_tab && !IsLeaderArmed(rail);
  }));

  omnibox->SetUserText(kSentinel, false);
  EXPECT_TRUE(
      SendViewsControlKey(browser(), omnibox, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  ASSERT_TRUE(base::test::RunUntil([&]() { return IsLeaderArmed(rail); }));
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return tab_strip->GetActiveTab() == second_tab && !IsLeaderArmed(rail);
  })) << "Views and renderer adapters must share one window controller";
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    LeaderDeduplicatesIdenticalNativeEventAcrossRendererAndViewsRoutes) {
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);

  content::RenderWidgetHost* target = GetActiveRenderWidgetHost(browser());
  const int32_t process_id = target->GetProcess()->GetID().GetUnsafeValue();
  const int32_t routing_id = target->GetRoutingID();
  NSWindow* native_window =
      browser()->GetWindow()->GetNativeWindow().GetNativeNSWindow();
  ASSERT_TRUE(native_window);
  NSEvent* prefix_event = cocoa_test_event_utils::SynthesizeKeyEvent(
      native_window, true, ui::VKEY_B, NSEventModifierFlagControl);
  ASSERT_TRUE(prefix_event);
  ASSERT_EQ(native_window, prefix_event.window);

  ForwardRendererNativeKeyEvent(process_id, routing_id, prefix_event);
  const auto renderer_snapshot = GetLeaderSnapshot(rail);
  ASSERT_TRUE(renderer_snapshot);
  ASSERT_TRUE(renderer_snapshot->leader_armed);
  ASSERT_EQ(1u, renderer_snapshot->leader_arm_generation);
  ASSERT_FALSE(renderer_snapshot->leader_arm_deadline.is_null());

  ui::KeyEvent views_event{base::apple::OwnedNSEvent(prefix_event)};
  EXPECT_TRUE(limitless::PreHandleSwiftUILeaderViewsKeyEventForTesting(
      rail, views_event));
  const auto deduplicated_snapshot = GetLeaderSnapshot(rail);
  ASSERT_TRUE(deduplicated_snapshot);
  EXPECT_TRUE(deduplicated_snapshot->leader_armed);
  EXPECT_EQ(renderer_snapshot->leader_arm_generation,
            deduplicated_snapshot->leader_arm_generation);
  EXPECT_EQ(renderer_snapshot->leader_arm_deadline,
            deduplicated_snapshot->leader_arm_deadline)
      << "The same physical event must not re-arm or extend the timeout";

  NSEvent* key_up_event = cocoa_test_event_utils::SynthesizeKeyEvent(
      native_window, false, ui::VKEY_B, NSEventModifierFlagControl);
  ASSERT_TRUE(key_up_event);
  ForwardRendererNativeKeyEvent(process_id, routing_id, key_up_event);
  ASSERT_TRUE(limitless::ExpireSwiftUILeaderForTesting(rail));
  ASSERT_TRUE(WaitForCondition([&]() { return !IsLeaderArmed(rail); }));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessSwiftUIBrowserTest,
    LeaderApplicationRoutePrecedesHighPriorityAcceleratorAndFallsThrough) {
  TabStripModel* tab_strip = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, false);
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* first_tab = tab_strip->GetTabAtIndex(0);
  tabs::TabInterface* second_tab = tab_strip->GetTabAtIndex(1);
  ASSERT_TRUE(first_tab);
  ASSERT_TRUE(second_tab);
  ASSERT_EQ(first_tab, tab_strip->GetActiveTab());

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(browser_view);
  ASSERT_TRUE(rail);
  OmniboxViewViews* omnibox =
      browser_view->GetLocationBarView()->omnibox_view();
  views::FocusManager* focus_manager =
      browser_view->GetWidget()->GetFocusManager();
  ASSERT_TRUE(omnibox);
  ASSERT_TRUE(focus_manager);
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()));

  const std::u16string sentinel = u"native route sentinel";
  omnibox->SetUserText(sentinel, false);
  omnibox->RequestFocus();
  ASSERT_TRUE(WaitForCondition([&]() { return omnibox->HasFocus(); }));

  ui::TestAcceleratorTarget high_priority_target;
  const ui::Accelerator prefix_accelerator(ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ScopedAcceleratorRegistration registration(
      focus_manager, prefix_accelerator, ui::AcceleratorManager::kHighPriority,
      &high_priority_target);

  // This uses EventGenerator's macOS APPLICATION target, which synthesizes an
  // NSEvent and dispatches it through NSApp. It is in-process AppKit routing
  // evidence, not external hardware-input acceptance.
  {
    ui::test::EventGenerator native_input(
        browser()->GetWindow()->GetNativeWindow());
    native_input.set_target(ui::test::EventGenerator::Target::APPLICATION);
    native_input.PressAndReleaseKey(ui::VKEY_B, ui::EF_CONTROL_DOWN);
    ASSERT_TRUE(WaitForCondition([&]() { return IsLeaderArmed(rail); }));
    EXPECT_EQ(0, high_priority_target.accelerator_count())
        << "Enabled Leader must precede the high-priority accelerator "
           "substrate";
    EXPECT_EQ(sentinel, omnibox->GetText());

    native_input.PressAndReleaseKey(ui::VKEY_N);
    ASSERT_TRUE(WaitForCondition([&]() {
      return tab_strip->GetActiveTab() == second_tab && !IsLeaderArmed(rail);
    })) << "The armed unmodified stroke must traverse the local AppKit monitor";
    EXPECT_EQ(0, high_priority_target.accelerator_count());

    rail->RequestFocus();
    ASSERT_EQ(rail, focus_manager->GetFocusedView());
    const GURL committed_url = second_tab->GetContents()->GetLastCommittedURL();
    native_input.PressAndReleaseKey(ui::VKEY_B, ui::EF_CONTROL_DOWN);
    ASSERT_TRUE(WaitForCondition([&]() { return IsLeaderArmed(rail); }));
    native_input.PressAndReleaseKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
    ASSERT_TRUE(WaitForCondition([&]() {
      return !IsLeaderArmed(rail) && omnibox->HasFocus() &&
             omnibox->IsSelectAll();
    })) << "An unrelated Cmd shortcut must fall through after Leader disarms";
    EXPECT_EQ(committed_url, second_tab->GetContents()->GetLastCommittedURL());
    EXPECT_EQ(second_tab, tab_strip->GetActiveTab());
    EXPECT_EQ(0, high_priority_target.accelerator_count());
  }

  Browser* routed_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(routed_browser);
  routed_browser->GetWindow()->Show();
  chrome::AddTabAt(routed_browser, GURL("about:blank"), -1, false);
  TabStripModel* routed_tabs = routed_browser->GetTabStripModel();
  tabs::TabInterface* routed_first_tab = routed_tabs->GetActiveTab();
  tabs::TabInterface* routed_second_tab = routed_tabs->GetTabAtIndex(1);
  views::View* routed_rail = GetSwiftUITabRail(routed_browser);
  ASSERT_TRUE(routed_first_tab);
  ASSERT_TRUE(routed_second_tab);
  ASSERT_TRUE(routed_rail);
  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(routed_browser));
  ui::test::EventGenerator routed_input(
      routed_browser->GetWindow()->GetNativeWindow());
  routed_input.set_target(ui::test::EventGenerator::Target::APPLICATION);
  routed_input.PressAndReleaseKey(ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(WaitForCondition([&]() { return IsLeaderArmed(routed_rail); }));
  EXPECT_FALSE(IsLeaderArmed(rail));
  routed_input.PressAndReleaseKey(ui::VKEY_N);
  ASSERT_TRUE(WaitForCondition([&]() {
    return routed_tabs->GetActiveTab() == routed_second_tab &&
           !IsLeaderArmed(routed_rail);
  })) << "APPLICATION events must route to the exact second NSWindow";
  EXPECT_EQ(second_tab, tab_strip->GetActiveTab());
  EXPECT_EQ(routed_second_tab, routed_tabs->GetActiveTab());
  CloseBrowserSynchronously(routed_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       LeaderStateIsWindowScopedAndSkipsPopups) {
  TabStripModel* first_tabs = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, false);
  tabs::TabInterface* first_window_first_tab = first_tabs->GetActiveTab();
  tabs::TabInterface* first_window_second_tab = first_tabs->GetTabAtIndex(1);
  ASSERT_TRUE(first_window_first_tab);
  ASSERT_TRUE(first_window_second_tab);

  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  second_browser->GetWindow()->Show();
  chrome::AddTabAt(second_browser, GURL("about:blank"), -1, false);
  TabStripModel* second_tabs = second_browser->GetTabStripModel();
  ASSERT_EQ(2, second_tabs->count());
  tabs::TabInterface* second_window_first_tab = second_tabs->GetActiveTab();
  tabs::TabInterface* second_window_second_tab = second_tabs->GetTabAtIndex(1);
  ASSERT_TRUE(second_window_first_tab);
  ASSERT_TRUE(second_window_second_tab);

  views::View* first_rail = GetSwiftUITabRail(browser());
  views::View* second_rail = GetSwiftUITabRail(second_browser);
  ASSERT_TRUE(first_rail);
  ASSERT_TRUE(second_rail);
  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return IsLeaderArmed(first_rail); }));
  EXPECT_FALSE(IsLeaderArmed(second_rail));

  SendRendererKey(second_browser, ui::VKEY_N, ui::EF_NONE, u'n');
  EXPECT_EQ(second_window_first_tab, second_tabs->GetActiveTab());
  EXPECT_TRUE(IsLeaderArmed(first_rail));

  SendRendererKey(second_browser, ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return IsLeaderArmed(second_rail); }));
  SendRendererKey(second_browser, ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return second_tabs->GetActiveTab() == second_window_second_tab &&
           !IsLeaderArmed(second_rail);
  }));
  EXPECT_TRUE(IsLeaderArmed(first_rail));
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return first_tabs->GetActiveTab() == first_window_second_tab &&
           !IsLeaderArmed(first_rail);
  }));

  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP,
                                   browser()->GetProfile(),
                                   /*from_user_gesture=*/true);
  params.omit_from_session_restore = true;
  BrowserWindowInterface* popup = CreateBrowserWindow(std::move(params));
  ASSERT_TRUE(popup);
  Browser* popup_browser = popup->GetBrowserForMigrationOnly();
  ASSERT_TRUE(popup_browser);
  chrome::AddTabAt(popup_browser, GURL("about:blank"), -1, true);
  popup->GetWindow()->Show();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      popup_browser, https_server_.GetURL("b.test", "/keys")));
  BrowserView* popup_view = BrowserView::GetBrowserViewForBrowser(popup);
  ASSERT_TRUE(popup_view);
  EXPECT_EQ(nullptr, popup_view->limitless_swiftui_rail_for_testing());
  content::WebContents* popup_contents =
      popup_browser->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(popup_contents);
  ASSERT_TRUE(ResetKeyEvidence(popup_contents, true));
  SendRendererKey(popup_browser, ui::VKEY_B, ui::EF_CONTROL_DOWN);
  SendRendererKey(popup_browser, ui::VKEY_B, ui::EF_NONE, u'b');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return KeyInputValue(popup_contents) == "b"; }));
  EXPECT_EQ(1, KeyEventCount(popup_contents, ui::VKEY_B, ui::EF_CONTROL_DOWN));

  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(popup));
  {
    ui::TestAcceleratorTarget popup_high_priority_target;
    const ui::Accelerator prefix_accelerator(ui::VKEY_B, ui::EF_CONTROL_DOWN);
    ScopedAcceleratorRegistration popup_registration(
        popup_view->GetWidget()->GetFocusManager(), prefix_accelerator,
        ui::AcceleratorManager::kHighPriority, &popup_high_priority_target);
    ui::test::EventGenerator popup_input(popup->GetWindow()->GetNativeWindow());
    popup_input.set_target(ui::test::EventGenerator::Target::APPLICATION);
    popup_input.PressAndReleaseKey(ui::VKEY_B, ui::EF_CONTROL_DOWN);
    EXPECT_EQ(1, popup_high_priority_target.accelerator_count())
        << "Popup APPLICATION routing must retain Chromium's stock delegate";
    EXPECT_EQ(1,
              KeyEventCount(popup_contents, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  }

  CloseBrowserSynchronously(popup);

  SendRendererKey(second_browser, ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return IsLeaderArmed(second_rail); }));
  CloseBrowserSynchronously(second_browser);
  EXPECT_TRUE(base::test::RunUntil([]() {
    return limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting() == 1;
  })) << "Closing an armed window must tear down its controller and timer";
  EXPECT_FALSE(IsLeaderArmed(first_rail));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserTest,
                       LimitsHostToNormalWindowsAndDetachesOnClose) {
  ASSERT_EQ(1, limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting());

  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  second_browser->GetWindow()->Show();
  BrowserView* second_view =
      BrowserView::GetBrowserViewForBrowser(second_browser);
  ASSERT_TRUE(second_view);
  views::NativeViewHost* second_host =
      views::AsViewClass<views::NativeViewHost>(
          second_view->limitless_swiftui_rail_for_testing());
  ASSERT_TRUE(second_host);
  ASSERT_TRUE(base::test::RunUntil(
      [second_host]() { return !!second_host->native_view(); }));
  NSView* __strong retained_native_view =
      second_host->native_view().GetNativeNSView();
  ASSERT_TRUE(retained_native_view);
  EXPECT_TRUE(base::test::RunUntil([]() {
    return limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting() == 2;
  }));
  CloseBrowserSynchronously(second_browser);
  EXPECT_TRUE(base::test::RunUntil([]() {
    return limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting() == 1;
  }));
  EXPECT_EQ(nil, retained_native_view.window);
  EXPECT_EQ(nil, retained_native_view.superview);

  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP,
                                   browser()->GetProfile(),
                                   /*from_user_gesture=*/true);
  params.omit_from_session_restore = true;
  BrowserWindowInterface* popup = CreateBrowserWindow(std::move(params));
  ASSERT_TRUE(popup);
  chrome::AddTabAt(popup, GURL("about:blank"), -1, true);
  popup->GetWindow()->Show();

  BrowserView* popup_view = BrowserView::GetBrowserViewForBrowser(popup);
  ASSERT_TRUE(popup_view);
  EXPECT_EQ(nullptr, popup_view->limitless_swiftui_rail_for_testing());
  EXPECT_EQ(1, limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting());
  CloseBrowserSynchronously(popup);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIHerdrBrowserTest,
                       ProjectsWholeSessionAndProtectsOffTheRecord) {
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  auto snapshot = WaitForRailAgents(rail, 2);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ((std::vector<std::string>{"w1:p1", "w2:p2"}),
            snapshot->pane_identifiers);
  EXPECT_EQ((std::vector<std::string>{"w1", "w2"}),
            snapshot->workspace_identifiers);
  EXPECT_EQ(
      (std::vector<std::u16string>{u"Alpha Workspace", u"Beta Workspace"}),
      snapshot->workspace_labels);
  EXPECT_EQ((std::vector<std::string>{"codex", "codex"}),
            snapshot->agent_kinds);
  EXPECT_EQ((std::vector<std::string>{"working", "blocked"}),
            snapshot->statuses);
  EXPECT_EQ((std::vector<std::u16string>{u"Agent 1", u"Agent 2"}),
            snapshot->terminal_titles);
  EXPECT_EQ((std::vector<bool>{false, true}), snapshot->focused);

  Browser* private_browser = CreateIncognitoBrowser();
  ASSERT_TRUE(private_browser);
  private_browser->GetWindow()->Show();
  views::View* private_rail = GetSwiftUITabRail(private_browser);
  ASSERT_TRUE(private_rail);
  std::optional<limitless::SwiftUIAgentRailSnapshotForTesting> private_snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    private_snapshot =
        limitless::GetSwiftUIAgentRailSnapshotForTesting(private_rail);
    return private_snapshot &&
           private_snapshot->source_state == "privateProfileDisabled";
  }));
  EXPECT_TRUE(private_snapshot->pane_identifiers.empty());
  EXPECT_TRUE(private_snapshot->workspace_identifiers.empty());
  EXPECT_TRUE(private_snapshot->workspace_labels.empty());
  EXPECT_TRUE(private_snapshot->agent_kinds.empty());
  EXPECT_TRUE(private_snapshot->statuses.empty());
  EXPECT_TRUE(private_snapshot->terminal_titles.empty());
  EXPECT_TRUE(private_snapshot->focused.empty());

  ASSERT_TRUE(WriteSnapshot(2, true));
  limitless::RequestHerdrAgentRefreshForTesting();
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = limitless::GetSwiftUIAgentRailSnapshotForTesting(rail);
    return snapshot && snapshot->source_state == "ready" &&
           snapshot->statuses == std::vector<std::string>{"done", "blocked"} &&
           snapshot->focused == std::vector<bool>{true, false};
  }));
  private_snapshot =
      limitless::GetSwiftUIAgentRailSnapshotForTesting(private_rail);
  ASSERT_TRUE(private_snapshot);
  EXPECT_EQ("privateProfileDisabled", private_snapshot->source_state);
  EXPECT_TRUE(private_snapshot->pane_identifiers.empty());
  CloseBrowserSynchronously(private_browser);

  snapshot = limitless::GetSwiftUIAgentRailSnapshotForTesting(rail);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ("ready", snapshot->source_state);
  EXPECT_EQ((std::vector<std::string>{"w1:p1", "w2:p2"}),
            snapshot->pane_identifiers);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIHerdrBrowserTest,
                       ProjectsAgentFailuresAndRecoversWithoutFakeRows) {
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);

  auto snapshot = WaitForRailAgentState(rail, "ready", 2);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ("ready", snapshot->source_state);
  EXPECT_EQ((std::vector<std::string>{"w1:p1", "w2:p2"}),
            snapshot->pane_identifiers);

  ASSERT_TRUE(WriteResponse("not json"));
  limitless::RequestHerdrAgentRefreshForTesting();
  snapshot = WaitForRailAgentState(rail, "malformedResponse", 0);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ("malformedResponse", snapshot->source_state);
  ExpectNoAgentRows(*snapshot);

  ASSERT_TRUE(WriteSnapshot(0, false));
  limitless::RequestHerdrAgentRefreshForTesting();
  snapshot = WaitForRailAgentState(rail, "ready", 0);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ("ready", snapshot->source_state);
  ExpectNoAgentRows(*snapshot);

  ASSERT_TRUE(WriteResponse(
      R"json({"id":"cli:api:snapshot","error":{"code":"unavailable","message":"Herdr unavailable"}})json"));
  limitless::RequestHerdrAgentRefreshForTesting();
  snapshot = WaitForRailAgentState(rail, "unavailable", 0);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ("unavailable", snapshot->source_state);
  ExpectNoAgentRows(*snapshot);

  ASSERT_TRUE(WriteSnapshot(3, true));
  limitless::RequestHerdrAgentRefreshForTesting();
  snapshot = WaitForRailAgentState(rail, "ready", 3);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ("ready", snapshot->source_state);
  EXPECT_EQ((std::vector<std::string>{"w1:p1", "w2:p2", "w1:p3"}),
            snapshot->pane_identifiers);
  EXPECT_EQ((std::vector<std::string>{"done", "blocked", "working"}),
            snapshot->statuses);
  EXPECT_EQ((std::vector<bool>{true, false, false}), snapshot->focused);
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIHerdrBrowserTest,
                       ManyTabsAndAgentsKeepBothSectionsUsable) {
  constexpr size_t kAgentCount = 12;
  constexpr size_t kTabCount = 40;
  constexpr size_t kWorkspaceCount = 12;
  constexpr double kAgentRowHeight = 32;
  constexpr double kMaximumAgentViewportHeight = 160;
  constexpr double kMinimumWorkspaceViewportHeight = 80;

  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(rail);
  ASSERT_TRUE(WaitForRailAgents(rail, 2));

  ASSERT_TRUE(WriteSnapshot(kAgentCount, false));
  limitless::RequestHerdrAgentRefreshForTesting();
  auto agents = WaitForRailAgents(rail, kAgentCount);
  ASSERT_TRUE(agents);
  const auto expected_agents = *agents;

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  while (static_cast<size_t>(tab_strip->count()) < kTabCount) {
    tab_strip->AppendWebContents(
        content::WebContents::Create(
            content::WebContents::CreateParams(browser()->GetProfile())),
        /*foreground=*/false);
  }
  auto tabs = WaitForRailTabs(rail, kTabCount);
  ASSERT_TRUE(tabs);
  const std::string default_workspace = tabs->workspace_identifiers[0];
  while (tabs->workspace_identifiers.size() < kWorkspaceCount) {
    const size_t expected_workspace_count =
        tabs->workspace_identifiers.size() + 1;
    ASSERT_TRUE(limitless::CreateSwiftUIWorkspaceForTesting(
        rail, u"Long Workspace Name For Overflow"));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      tabs = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
      return tabs &&
             tabs->workspace_identifiers.size() == expected_workspace_count;
    }));
  }
  ASSERT_TRUE(
      limitless::SwitchSwiftUIWorkspaceForTesting(rail, default_workspace));
  tabs = WaitForRailTabs(rail, kTabCount);
  ASSERT_TRUE(tabs);
  ASSERT_TRUE(WaitForCondition([&]() {
    agents = limitless::GetSwiftUIAgentRailSnapshotForTesting(rail);
    if (!agents || agents->source_state != "ready" ||
        agents->pane_identifiers.size() != kAgentCount) {
      return false;
    }
    const auto has_positive_area = [](const auto& rect) {
      return rect.width > 0 && rect.height > 0;
    };
    const auto lies_inside_rail = [&](const auto& rect) {
      return rect.x >= agents->rail.x && rect.y >= agents->rail.y &&
             rect.x + rect.width <= agents->rail.x + agents->rail.width &&
             rect.y + rect.height <= agents->rail.y + agents->rail.height;
    };
    const bool viewports_are_disjoint =
        agents->workspace_viewport.y + agents->workspace_viewport.height <=
            agents->agent_viewport.y ||
        agents->agent_viewport.y + agents->agent_viewport.height <=
            agents->workspace_viewport.y;
    return has_positive_area(agents->rail) &&
           has_positive_area(agents->workspace_header) &&
           has_positive_area(agents->workspace_create_control) &&
           has_positive_area(agents->workspace_switcher_viewport) &&
           has_positive_area(agents->workspace_switcher_content) &&
           has_positive_area(agents->workspace_viewport) &&
           has_positive_area(agents->workspace_content) &&
           has_positive_area(agents->agent_viewport) &&
           has_positive_area(agents->agent_content) &&
           lies_inside_rail(agents->workspace_header) &&
           lies_inside_rail(agents->workspace_create_control) &&
           lies_inside_rail(agents->workspace_switcher_viewport) &&
           lies_inside_rail(agents->workspace_viewport) &&
           lies_inside_rail(agents->agent_viewport) && viewports_are_disjoint &&
           agents->workspace_switcher_content.width >
               agents->workspace_switcher_viewport.width &&
           agents->workspace_viewport.height >=
               kMinimumWorkspaceViewportHeight &&
           agents->workspace_content.height >
               agents->workspace_viewport.height &&
           agents->agent_viewport.height >= kAgentRowHeight &&
           agents->agent_viewport.height <= kMaximumAgentViewportHeight &&
           agents->agent_content.height > agents->agent_viewport.height;
  })) << "workspace viewport/content: "
      << (agents ? agents->workspace_viewport.height : -1) << "/"
      << (agents ? agents->workspace_content.height : -1)
      << ", agent viewport/content: "
      << (agents ? agents->agent_viewport.height : -1) << "/"
      << (agents ? agents->agent_content.height : -1);
  EXPECT_EQ(kAgentCount, agents->pane_identifiers.size());
  EXPECT_EQ(kAgentCount, agents->focused.size());

  const int32_t last_tab_identifier = tabs->tab_identifiers.back();
  tabs::TabInterface* last_tab = tabs::TabHandle(last_tab_identifier).Get();
  ASSERT_TRUE(last_tab);
  EXPECT_TRUE(
      limitless::ActivateSwiftUITabForTesting(rail, last_tab_identifier));
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return tab_strip->GetActiveTab() == last_tab; }));
  auto unchanged_agents =
      limitless::GetSwiftUIAgentRailSnapshotForTesting(rail);
  ASSERT_TRUE(unchanged_agents);
  EXPECT_EQ(expected_agents.source_state, unchanged_agents->source_state);
  EXPECT_EQ(expected_agents.pane_identifiers,
            unchanged_agents->pane_identifiers);
  EXPECT_EQ(expected_agents.workspace_identifiers,
            unchanged_agents->workspace_identifiers);
  EXPECT_EQ(expected_agents.workspace_labels,
            unchanged_agents->workspace_labels);
  EXPECT_EQ(expected_agents.statuses, unchanged_agents->statuses);
  EXPECT_EQ(expected_agents.focused, unchanged_agents->focused);

  ASSERT_TRUE(WriteSnapshot(0, false));
  limitless::RequestHerdrAgentRefreshForTesting();
  agents = WaitForRailAgents(rail, 0);
  ASSERT_TRUE(agents);
  EXPECT_EQ("ready", agents->source_state);

  constexpr size_t kMaximumClosedNodeCount =
      32 + 6 * kTabCount + 4 * kWorkspaceCount;
  constexpr size_t kMaximumClosedProductIdentifierCount =
      6 + kWorkspaceCount + 4 * kTabCount;
  const limitless::SwiftUIAccessibilityInventoryForTesting inventory =
      limitless::GetSwiftUIAccessibilityInventoryForTesting(rail);
  EXPECT_FALSE(inventory.hit_node_cap);
  EXPECT_LE(inventory.node_count, kMaximumClosedNodeCount);

  std::vector<std::string> product_identifiers;
  std::ranges::copy_if(inventory.identifiers,
                       std::back_inserter(product_identifiers),
                       [](const std::string& identifier) {
                         return base::StartsWith(identifier, "limitless-");
                       });
  EXPECT_LE(product_identifiers.size(), kMaximumClosedProductIdentifierCount);
  const std::set<std::string> unique_product_identifiers(
      product_identifiers.begin(), product_identifiers.end());
  EXPECT_EQ(product_identifiers.size(), unique_product_identifiers.size());

  for (const std::string& workspace_identifier : tabs->workspace_identifiers) {
    EXPECT_EQ(
        1, std::ranges::count(product_identifiers,
                              "limitless-workspace-" + workspace_identifier));
  }

  ASSERT_EQ(tabs->tab_identifiers.size(), tabs->tab_product_identifiers.size());
  std::vector<int32_t> materialized_tabs;
  for (size_t index = 0; index < tabs->tab_identifiers.size(); ++index) {
    const int32_t tab_identifier = tabs->tab_identifiers[index];
    const std::string suffix = base::NumberToString(tab_identifier);
    if (!unique_product_identifiers.contains("limitless-tab-" + suffix)) {
      continue;
    }
    materialized_tabs.push_back(tab_identifier);
    EXPECT_TRUE(
        unique_product_identifiers.contains("limitless-tab-close-" + suffix));
    EXPECT_TRUE(
        unique_product_identifiers.contains("limitless-tab-move-" + suffix));
    EXPECT_TRUE(unique_product_identifiers.contains(
        "limitless-tab-split-" + tabs->tab_product_identifiers[index]));
  }
  EXPECT_FALSE(materialized_tabs.empty());
  EXPECT_LE(materialized_tabs.size(), kTabCount);
  EXPECT_EQ(product_identifiers.end(),
            std::ranges::find_if(
                product_identifiers, [](const std::string& identifier) {
                  return identifier.find("-to-") != std::string::npos;
                }));
  EXPECT_EQ(product_identifiers.end(),
            std::ranges::find_if(
                product_identifiers, [](const std::string& identifier) {
                  return identifier.find("-with-") != std::string::npos;
                }));

  const int32_t menu_tab = materialized_tabs.front();
  const std::string menu_button_identifier =
      "limitless-tab-move-" + base::NumberToString(menu_tab);
  const std::string active_workspace =
      tabs->active_workspace_identifier.value();
  std::set<std::string> expected_destinations;
  for (const std::string& workspace_identifier : tabs->workspace_identifiers) {
    if (workspace_identifier == active_workspace) {
      continue;
    }
    expected_destinations.insert("limitless-tab-move-" +
                                 base::NumberToString(menu_tab) + "-to-" +
                                 workspace_identifier);
  }
  ASSERT_EQ(kWorkspaceCount - 1, expected_destinations.size());
  const std::string selected_destination = *expected_destinations.begin();
  const auto menu_action =
      limitless::PerformSwiftUIAccessibilityMenuItemForTesting(
          rail, menu_button_identifier, selected_destination);
  ASSERT_TRUE(menu_action);
  EXPECT_TRUE(menu_action->destination.focused);
  EXPECT_TRUE(menu_action->press_invoked);
  EXPECT_FALSE(menu_action->inventory.hit_node_cap);
  EXPECT_LE(menu_action->inventory.node_count, 4 + 2 * (kWorkspaceCount - 1));

  std::set<std::string> actual_destinations;
  for (const std::string& identifier : menu_action->inventory.identifiers) {
    if (identifier.find("-to-") != std::string::npos) {
      actual_destinations.insert(identifier);
    }
  }
  EXPECT_EQ(expected_destinations, actual_destinations);
  ASSERT_EQ(kWorkspaceCount - 1, actual_destinations.size());
  ASSERT_TRUE(WaitForCondition([&]() {
    limitless::WorkspaceController* controller =
        limitless::WorkspaceController::From(browser());
    const std::optional<std::string> serialized =
        controller->SerializeTabForSession(tabs::TabHandle(menu_tab));
    tabs = limitless::GetSwiftUITabRailSnapshotForTesting(rail);
    return serialized &&
           ReadWorkspaceTabSessionData(*serialized).second !=
               active_workspace &&
           tabs &&
           std::ranges::find(tabs->tab_identifiers, menu_tab) ==
               tabs->tab_identifiers.end();
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUIBrowserDisabledTest,
                       LeavesOrdinaryBrowserViewUnchanged) {
  EXPECT_EQ(nullptr, WebUIBrowserWindow::FromBrowser(browser()));
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  EXPECT_EQ(nullptr, browser_view->limitless_swiftui_rail_for_testing());
  EXPECT_EQ(0, limitless::GetSwiftUIBoundaryProbeAttachedHostCountForTesting());
}

IN_PROC_BROWSER_TEST_F(LimitlessSwiftUILeaderDisabledBrowserTest,
                       RestoresChromeRoutingAndOrdinaryTyping) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), https_server_.GetURL("a.test", "/keys")));
  content::WebContents* key_contents =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(key_contents);
  ASSERT_TRUE(ResetKeyEvidence(key_contents, true));

  TabStripModel* tab_strip = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, false);
  ASSERT_EQ(2, tab_strip->count());
  tabs::TabInterface* initial_tab = tab_strip->GetActiveTab();
  ASSERT_TRUE(initial_tab);

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  views::View* rail = GetSwiftUITabRail(browser());
  ASSERT_TRUE(browser_view);
  ASSERT_TRUE(rail);
  std::optional<limitless::SwiftUITabRailSnapshotForTesting> snapshot;
  ASSERT_TRUE(base::test::RunUntil([&]() {
    snapshot = GetLeaderSnapshot(rail);
    return snapshot && snapshot->leader_native_delegate_requested;
  }));
  EXPECT_FALSE(snapshot->leader_enabled);
  EXPECT_FALSE(snapshot->leader_armed);
  EXPECT_TRUE(snapshot->leader_native_delegate_requested)
      << "The disabled fallback still proves exact-window hook discovery";

  SendRendererKey(browser(), ui::VKEY_B, ui::EF_CONTROL_DOWN);
  SendRendererKey(browser(), ui::VKEY_N, ui::EF_NONE, u'n');
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return KeyInputValue(key_contents) == "n"; }));
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_B, ui::EF_CONTROL_DOWN));
  EXPECT_EQ(1, KeyEventCount(key_contents, ui::VKEY_N, ui::EF_NONE));
  EXPECT_FALSE(IsLeaderArmed(rail));
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());

  OmniboxViewViews* omnibox =
      browser_view->GetLocationBarView()->omnibox_view();
  ASSERT_TRUE(omnibox);
  const std::u16string sentinel = u"disabled leader sentinel";
  omnibox->SetUserText(sentinel, false);
  SendViewsControlKey(browser(), omnibox, ui::VKEY_B, ui::EF_CONTROL_DOWN);
  EXPECT_FALSE(IsLeaderArmed(rail));
  EXPECT_EQ(sentinel, omnibox->GetText());
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());

  ASSERT_TRUE(ui_test_utils::BringBrowserWindowToFront(browser()));
  omnibox->RequestFocus();
  ASSERT_TRUE(WaitForCondition([&]() { return omnibox->HasFocus(); }));
  ui::TestAcceleratorTarget high_priority_target;
  const ui::Accelerator prefix_accelerator(ui::VKEY_B, ui::EF_CONTROL_DOWN);
  ScopedAcceleratorRegistration registration(
      browser_view->GetWidget()->GetFocusManager(), prefix_accelerator,
      ui::AcceleratorManager::kHighPriority, &high_priority_target);
  ui::test::EventGenerator native_input(
      browser()->GetWindow()->GetNativeWindow());
  native_input.set_target(ui::test::EventGenerator::Target::APPLICATION);
  native_input.PressAndReleaseKey(ui::VKEY_B, ui::EF_CONTROL_DOWN);
  EXPECT_EQ(1, high_priority_target.accelerator_count())
      << "Disabled policy must restore stock high-priority routing";
  EXPECT_FALSE(IsLeaderArmed(rail));
  EXPECT_EQ(sentinel, omnibox->GetText());
  EXPECT_EQ(initial_tab, tab_strip->GetActiveTab());
}

}  // namespace
