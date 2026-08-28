// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_window_registry.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "shell/commands/limitless_command_registry.h"

namespace limitless {

class ProductCommandWindowRegistryBrowserTest : public InProcessBrowserTest {};

class TestWindowAuthority {
 public:
  TestWindowAuthority(std::string window_id, bool is_active = true)
      : window_id_(std::move(window_id)),
        is_active_(is_active),
        command_registry_(base::BindRepeating(&TestWindowAuthority::Query,
                                              base::Unretained(this)),
                          base::BindRepeating(&TestWindowAuthority::Execute,
                                              base::Unretained(this))),
        binding_(command_registry_.GetWeakPtr(),
                 base::BindRepeating(&TestWindowAuthority::GetState,
                                     base::Unretained(this))) {}

  base::WeakPtr<ProductCommandWindowBinding> binding() {
    return binding_.GetWeakPtr();
  }
  void set_window_id(std::string window_id) {
    window_id_ = std::move(window_id);
  }

  std::optional<ProductCommandWindowState> GetState() {
    if (!available) {
      return std::nullopt;
    }
    if (on_state) {
      on_state.Run();
    }
    ProductCommandWindowState state;
    state.window_id = window_id_;
    state.active_workspace_id = "22222222-2222-4222-8222-222222222222";
    state.visible_tab_ids = {"33333333-3333-4333-8333-333333333333"};
    state.active_tab_id = state.visible_tab_ids.front();
    state.is_active = is_active_;
    return state;
  }
  ProductCommandQueryResult Query(const ProductCommandInvocation&) {
    ++query_count;
    if (on_query) {
      on_query.Run();
    }
    return {.status = ProductCommandQueryStatus::kEnabled};
  }
  ProductCommandExecutionResult Execute(const ProductCommandInvocation&,
                                        ProductCommandOrigin) {
    ++execute_count;
    if (on_execute) {
      on_execute.Run();
    }
    return {.status = ProductCommandExecutionStatus::kSucceeded};
  }

  bool available = true;
  int query_count = 0;
  int execute_count = 0;
  base::RepeatingClosure on_state;
  base::RepeatingClosure on_query;
  base::RepeatingClosure on_execute;

 private:
  std::string window_id_;
  bool is_active_;

  ProductCommandRegistry command_registry_;
  ProductCommandWindowBinding binding_;
};

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       TwoSameProfileWindowsTargetExactInactiveWindow) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority first("11111111-1111-4111-8111-111111111111", true);
  TestWindowAuthority second("44444444-4444-4444-8444-444444444444", false);
  Profile* profile = browser()->GetProfile();
  auto first_registration = registry.Register(browser(), first.binding());
  Browser* second_browser = CreateBrowser(profile);
  ASSERT_TRUE(first_registration);
  ASSERT_TRUE(second_browser);
  auto second_registration =
      registry.Register(second_browser, second.binding());
  ASSERT_TRUE(second_registration);
  EXPECT_NE(first_registration->window_instance_id(),
            second_registration->window_instance_id());
  auto discovery = registry.Discover();
  ASSERT_EQ(ProductCommandWindowLookupStatus::kFound, discovery.lookup_status);
  ASSERT_EQ(2u, discovery.windows.size());
  ProductCommandWindowTarget target{"44444444-4444-4444-8444-444444444444",
                                    second_registration->window_instance_id()};
  ProductCommandInvocation invocation;
  auto query = registry.Query(target, invocation);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, query.lookup_status);
  EXPECT_EQ(ProductCommandQueryStatus::kEnabled, query.command.status);
  auto execution = registry.ExecuteGuarded(
      target, invocation, ProductCommandOrigin::kExternal,
      base::BindOnce([](const ProductCommandInvocation&) {
        return ProductCommandPreExecuteDecision::kAllow;
      }));
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, execution.lookup_status);
  EXPECT_EQ(ProductCommandExecutionStatus::kSucceeded,
            execution.command.status);
  EXPECT_EQ(0, first.query_count);
  EXPECT_EQ(0, first.execute_count);
  EXPECT_EQ(2, second.query_count);
  EXPECT_EQ(1, second.execute_count);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       ResetAndReregisterDefeatsStaleTargetABA) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto first = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(first);
  const ProductCommandWindowTarget old_target{
      "11111111-1111-4111-8111-111111111111", first->window_instance_id()};
  first->Reset();
  auto second = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(second);
  EXPECT_NE(old_target.window_instance_id, second->window_instance_id());
  auto stale = registry.GetState(old_target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            stale.lookup_status);
  EXPECT_FALSE(stale.state);
  const ProductCommandWindowTarget new_target{old_target.window_id,
                                              second->window_instance_id()};
  auto current = registry.GetState(new_target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, current.lookup_status);
  ASSERT_TRUE(current.state);
  EXPECT_EQ(old_target.window_id, current.state->window_id);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       DuplicatePersistentWindowIdIsAmbiguous) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority first("11111111-1111-4111-8111-111111111111", true);
  TestWindowAuthority second("11111111-1111-4111-8111-111111111111", false);
  auto first_registration = registry.Register(browser(), first.binding());
  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(first_registration);
  ASSERT_TRUE(second_browser);
  auto second_registration =
      registry.Register(second_browser, second.binding());
  ASSERT_TRUE(second_registration);
  auto discovery = registry.Discover();
  EXPECT_EQ(ProductCommandWindowLookupStatus::kAmbiguous,
            discovery.lookup_status);
  EXPECT_TRUE(discovery.windows.empty());
  const ProductCommandWindowTarget first_target{
      "11111111-1111-4111-8111-111111111111",
      first_registration->window_instance_id()};
  const ProductCommandWindowTarget second_target{
      "11111111-1111-4111-8111-111111111111",
      second_registration->window_instance_id()};
  EXPECT_EQ(ProductCommandWindowLookupStatus::kAmbiguous,
            registry.GetState(first_target).lookup_status);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kAmbiguous,
            registry.GetState(second_target).lookup_status);
  EXPECT_FALSE(registry.GetState(first_target).state);
  EXPECT_FALSE(registry.GetState(second_target).state);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       TemporaryUnavailableRecoversWithSameInstance) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  authority.available = false;
  auto unavailable = registry.GetState(target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kUnavailable,
            unavailable.lookup_status);
  EXPECT_FALSE(unavailable.state);
  auto unavailable_discovery = registry.Discover();
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound,
            unavailable_discovery.lookup_status);
  EXPECT_TRUE(unavailable_discovery.windows.empty());
  authority.available = true;
  auto recovered = registry.GetState(target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, recovered.lookup_status);
  ASSERT_TRUE(recovered.state);
  EXPECT_EQ(target.window_id, recovered.state->window_id);
  EXPECT_EQ(target.window_instance_id, registration->window_instance_id());
  auto discovery = registry.Discover();
  ASSERT_EQ(1u, discovery.windows.size());
  EXPECT_EQ(target.window_instance_id,
            discovery.windows.front().target.window_instance_id);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       DiscoveryFailsClosedWhenStateCallbackUnregisters) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  authority.on_state = base::BindRepeating(
      [](std::unique_ptr<ProductCommandWindowRegistry::Registration>*
             registration) { registration->reset(); },
      base::Unretained(&registration));
  auto result = registry.Discover();
  EXPECT_FALSE(registration);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kUnavailable,
            result.lookup_status);
  EXPECT_TRUE(result.windows.empty());
  auto second = registry.Discover();
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, second.lookup_status);
  EXPECT_TRUE(second.windows.empty());
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       ExecuteFailsClosedWhenQueryUnregisters) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  authority.on_query = base::BindRepeating(
      [](std::unique_ptr<ProductCommandWindowRegistry::Registration>*
             registration) { registration->reset(); },
      base::Unretained(&registration));
  int guard_count = 0;
  ProductCommandInvocation invocation;
  auto result = registry.ExecuteGuarded(
      target, invocation, ProductCommandOrigin::kExternal,
      base::BindOnce(
          [](int* count, const ProductCommandInvocation&) {
            ++*count;
            return ProductCommandPreExecuteDecision::kAllow;
          },
          &guard_count));
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            result.lookup_status);
  EXPECT_EQ(ProductCommandExecutionStatus::kPreconditionFailed,
            result.command.status);
  EXPECT_FALSE(registration);
  EXPECT_EQ(1, authority.query_count);
  EXPECT_EQ(0, guard_count);
  EXPECT_EQ(0, authority.execute_count);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            registry.GetState(target).lookup_status);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       GuardReregisterDefeatsABAAndPreventsExecution) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget old_target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  std::unique_ptr<ProductCommandWindowRegistry::Registration> replacement;
  ProductCommandInvocation invocation;
  auto result = registry.ExecuteGuarded(
      old_target, invocation, ProductCommandOrigin::kExternal,
      base::BindOnce(
          [](std::unique_ptr<ProductCommandWindowRegistry::Registration>* old,
             ProductCommandWindowRegistry* registry,
             BrowserWindowInterface* browser, TestWindowAuthority* authority,
             std::unique_ptr<ProductCommandWindowRegistry::Registration>*
                 replacement,
             const ProductCommandInvocation&) {
            old->reset();
            *replacement = registry->Register(browser, authority->binding());
            return ProductCommandPreExecuteDecision::kAllow;
          },
          base::Unretained(&registration), base::Unretained(&registry),
          browser(), base::Unretained(&authority),
          base::Unretained(&replacement)));
  ASSERT_TRUE(replacement);
  EXPECT_NE(old_target.window_instance_id, replacement->window_instance_id());
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            result.lookup_status);
  EXPECT_EQ(ProductCommandExecutionStatus::kPreconditionFailed,
            result.command.status);
  EXPECT_EQ(1, authority.query_count);
  EXPECT_EQ(0, authority.execute_count);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            registry.GetState(old_target).lookup_status);
  ProductCommandWindowTarget new_target{old_target.window_id,
                                        replacement->window_instance_id()};
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound,
            registry.GetState(new_target).lookup_status);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       SplitInvocationWindowMismatchRejectedBeforeAuthority) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  const ProductCommandSplitPair pair{"44444444-4444-4444-8444-444444444444",
                                     "55555555-5555-4555-8555-555555555555",
                                     "66666666-6666-4666-8666-666666666666"};
  std::vector<ProductCommandInvocation> invocations = {
      {.command = ProductCommandId::kSplitCreate,
       .arguments = ProductCommandSplitCreate{pair}},
      {.command = ProductCommandId::kSplitFocus,
       .arguments = ProductCommandSplitFocus{pair, pair.first_tab_identifier}},
      {.command = ProductCommandId::kSplitSetLayout,
       .arguments =
           ProductCommandSetSplitLayout{pair,
                                        ProductCommandSplitLayout::kStacked}},
      {.command = ProductCommandId::kSplitSetRatio,
       .arguments = ProductCommandSetSplitRatio{pair, 0.5}},
      {.command = ProductCommandId::kSplitSwap, .arguments = pair},
      {.command = ProductCommandId::kSplitUnsplit, .arguments = pair},
  };
  int guard_count = 0;
  for (const auto& invocation : invocations) {
    auto query = registry.Query(target, invocation);
    EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, query.lookup_status);
    EXPECT_EQ(ProductCommandQueryStatus::kInvalidInvocation,
              query.command.status);
    EXPECT_EQ(ProductCommandValidationStatus::kWindowTargetMismatch,
              query.command.validation);
    auto execution = registry.ExecuteGuarded(
        target, invocation, ProductCommandOrigin::kExternal,
        base::BindOnce(
            [](int* count, const ProductCommandInvocation&) {
              ++*count;
              return ProductCommandPreExecuteDecision::kAllow;
            },
            &guard_count));
    EXPECT_EQ(ProductCommandWindowLookupStatus::kFound,
              execution.lookup_status);
    EXPECT_EQ(ProductCommandExecutionStatus::kInvalidInvocation,
              execution.command.status);
    EXPECT_EQ(ProductCommandValidationStatus::kWindowTargetMismatch,
              execution.command.validation);
  }
  EXPECT_EQ(0, guard_count);
  EXPECT_EQ(0, authority.query_count);
  EXPECT_EQ(0, authority.execute_count);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       BrowserCloseInvalidatesExactTargetOnly) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority first("11111111-1111-4111-8111-111111111111", true);
  TestWindowAuthority second("44444444-4444-4444-8444-444444444444", false);
  auto first_registration = registry.Register(browser(), first.binding());
  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(first_registration);
  ASSERT_TRUE(second_browser);
  auto second_registration =
      registry.Register(second_browser, second.binding());
  ASSERT_TRUE(second_registration);
  const ProductCommandWindowTarget first_target{
      "11111111-1111-4111-8111-111111111111",
      first_registration->window_instance_id()};
  const ProductCommandWindowTarget second_target{
      "44444444-4444-4444-8444-444444444444",
      second_registration->window_instance_id()};
  CloseBrowserSynchronously(second_browser);
  EXPECT_TRUE(second_registration->window_instance_id().empty());
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            registry.GetState(second_target).lookup_status);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound,
            registry.GetState(first_target).lookup_status);
  auto discovery = registry.Discover();
  ASSERT_EQ(1u, discovery.windows.size());
  EXPECT_EQ(first_target.window_instance_id,
            discovery.windows.front().target.window_instance_id);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       RegistrationSafelyOutlivesRegistry) {
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  std::unique_ptr<ProductCommandWindowRegistry::Registration> registration;
  std::string instance;
  {
    ProductCommandWindowRegistry registry(browser()->GetProfile());
    registration = registry.Register(browser(), authority.binding());
    ASSERT_TRUE(registration);
    instance = registration->window_instance_id();
    EXPECT_FALSE(instance.empty());
    EXPECT_EQ(instance, registration->window_instance_id());
  }
  EXPECT_EQ(instance, registration->window_instance_id());
  registration.reset();
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       RegistryOutlivesRegistrationAndRejectsExpiredBinding) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  registration.reset();
  auto discovery = registry.Discover();
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, discovery.lookup_status);
  EXPECT_TRUE(discovery.windows.empty());
  Browser* fresh_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(fresh_browser);
  base::WeakPtr<ProductCommandWindowBinding> expired;
  {
    TestWindowAuthority temporary("44444444-4444-4444-8444-444444444444",
                                  false);
    expired = temporary.binding();
  }
  EXPECT_FALSE(expired);
  EXPECT_FALSE(registry.Register(fresh_browser, expired));
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       RejectsDuplicatePrivateCrossProfileAndPopupWindows) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  EXPECT_FALSE(registry.Register(browser(), authority.binding()));
  Browser* private_browser = CreateIncognitoBrowser(browser()->GetProfile());
  ASSERT_TRUE(private_browser);
  EXPECT_FALSE(registry.Register(private_browser, authority.binding()));
  ProductCommandWindowRegistry private_registry(private_browser->GetProfile());
  EXPECT_FALSE(private_registry.Register(private_browser, authority.binding()));
  Profile* other_profile = &profiles::testing::CreateProfileSync(
      g_browser_process->profile_manager(),
      g_browser_process->profile_manager()->GenerateNextProfileDirectoryPath());
  Browser* other_browser = CreateBrowser(other_profile);
  ASSERT_TRUE(other_browser);
  EXPECT_FALSE(registry.Register(other_browser, authority.binding()));
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP,
                                   browser()->GetProfile(), true);
  BrowserWindowInterface* popup = CreateBrowserWindow(std::move(params));
  ASSERT_TRUE(popup);
  EXPECT_EQ(BrowserWindowInterface::TYPE_POPUP, popup->GetType());
  EXPECT_FALSE(registry.Register(popup, authority.binding()));
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       InvalidTargetsAndChangedPersistentIdentityFailClosed) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111", true);
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const std::string instance = registration->window_instance_id();
  const ProductCommandWindowTarget old_target{
      "11111111-1111-4111-8111-111111111111", instance};
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound,
            registry.GetState(old_target).lookup_status);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kInvalidTarget,
            registry.GetState({"not-a-uuid", instance}).lookup_status);
  EXPECT_EQ(
      ProductCommandWindowLookupStatus::kInvalidTarget,
      registry.GetState({old_target.window_id, "not-a-uuid"}).lookup_status);
  authority.set_window_id("44444444-4444-4444-8444-444444444444");
  auto stale = registry.GetState(old_target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            stale.lookup_status);
  EXPECT_FALSE(stale.state);
  const ProductCommandWindowTarget new_target{
      "44444444-4444-4444-8444-444444444444", instance};
  auto current = registry.GetState(new_target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, current.lookup_status);
  ASSERT_TRUE(current.state);
  EXPECT_EQ(new_target.window_id, current.state->window_id);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       OtherWindowStateMutationCannotMakeStaleTargetLookFound) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority target_authority("11111111-1111-4111-8111-111111111111");
  TestWindowAuthority other_authority("44444444-4444-4444-8444-444444444444");
  auto target_registration =
      registry.Register(browser(), target_authority.binding());
  ASSERT_TRUE(target_registration);
  const ProductCommandWindowTarget old_target{
      "11111111-1111-4111-8111-111111111111",
      target_registration->window_instance_id()};
  other_authority.on_state = base::BindRepeating(
      [](TestWindowAuthority* authority, TestWindowAuthority* target) {
        target->set_window_id("55555555-5555-4555-8555-555555555555");
      },
      &other_authority, &target_authority);
  auto other_registration = registry.Register(
      CreateBrowser(browser()->GetProfile()), other_authority.binding());
  ASSERT_TRUE(other_registration);
  auto stale = registry.GetState(old_target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            stale.lookup_status);
  auto current = registry.GetState(
      {"55555555-5555-4555-8555-555555555555", old_target.window_instance_id});
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, current.lookup_status);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       CoreInvocationValidationPrecedesWindowMismatch) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111");
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  ProductCommandInvocation invocation;
  invocation.command = ProductCommandId::kTabsNext;
  invocation.arguments =
      ProductCommandSplitPair{"44444444-4444-4444-8444-444444444444",
                              "22222222-2222-4222-8222-222222222222",
                              "33333333-3333-4333-8333-333333333333"};
  int guard_count = 0;
  auto query = registry.Query(target, invocation);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, query.lookup_status);
  EXPECT_EQ(ProductCommandQueryStatus::kInvalidInvocation,
            query.command.status);
  EXPECT_EQ(ProductCommandValidationStatus::kArgumentTypeMismatch,
            query.command.validation);
  auto execute = registry.ExecuteGuarded(
      target, invocation, ProductCommandOrigin::kExternal,
      base::BindOnce(
          [](int* count, const ProductCommandInvocation&) {
            ++*count;
            return ProductCommandPreExecuteDecision::kAllow;
          },
          &guard_count));
  EXPECT_EQ(ProductCommandWindowLookupStatus::kFound, execute.lookup_status);
  EXPECT_EQ(ProductCommandExecutionStatus::kInvalidInvocation,
            execute.command.status);
  EXPECT_EQ(ProductCommandValidationStatus::kArgumentTypeMismatch,
            execute.command.validation);
  EXPECT_EQ(0, authority.query_count);
  EXPECT_EQ(0, authority.execute_count);
  EXPECT_EQ(0, guard_count);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       RegistryDestructionDuringStateCallbackFailsClosed) {
  auto registry =
      std::make_unique<ProductCommandWindowRegistry>(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111");
  auto registration = registry->Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  authority.on_state = base::BindRepeating(
      [](std::unique_ptr<ProductCommandWindowRegistry>* owner) {
        owner->reset();
      },
      base::Unretained(&registry));
  auto result = registry->GetState(target);
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            result.lookup_status);
  EXPECT_FALSE(registry);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       DirectQueryUnregisterReentrancyFailsClosed) {
  ProductCommandWindowRegistry registry(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111");
  auto registration = registry.Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  authority.on_query = base::BindRepeating(
      [](std::unique_ptr<ProductCommandWindowRegistry::Registration>* value) {
        value->reset();
      },
      base::Unretained(&registration));
  auto result = registry.Query(target, ProductCommandInvocation{});
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            result.lookup_status);
  EXPECT_EQ(ProductCommandQueryStatus::kDisabled, result.command.status);
  EXPECT_FALSE(registration);
  EXPECT_EQ(0, authority.execute_count);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       RegistryDestructionDuringQueryCallbackFailsClosed) {
  auto registry =
      std::make_unique<ProductCommandWindowRegistry>(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111");
  auto registration = registry->Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  authority.on_query = base::BindRepeating(
      [](std::unique_ptr<ProductCommandWindowRegistry>* owner) {
        owner->reset();
      },
      base::Unretained(&registry));
  auto result = registry->Query(target, ProductCommandInvocation{});
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            result.lookup_status);
  EXPECT_EQ(ProductCommandQueryStatus::kDisabled, result.command.status);
  EXPECT_FALSE(registry);
  EXPECT_EQ(0, authority.execute_count);
}

IN_PROC_BROWSER_TEST_F(ProductCommandWindowRegistryBrowserTest,
                       RegistryDestructionDuringGuardCallbackFailsClosed) {
  auto registry =
      std::make_unique<ProductCommandWindowRegistry>(browser()->GetProfile());
  TestWindowAuthority authority("11111111-1111-4111-8111-111111111111");
  auto registration = registry->Register(browser(), authority.binding());
  ASSERT_TRUE(registration);
  const ProductCommandWindowTarget target{
      "11111111-1111-4111-8111-111111111111",
      registration->window_instance_id()};
  int guard_count = 0;
  auto result = registry->ExecuteGuarded(
      target, ProductCommandInvocation{}, ProductCommandOrigin::kExternal,
      base::BindOnce(
          [](std::unique_ptr<ProductCommandWindowRegistry>* owner, int* count,
             const ProductCommandInvocation&) {
            ++*count;
            owner->reset();
            return ProductCommandPreExecuteDecision::kAllow;
          },
          base::Unretained(&registry), &guard_count));
  EXPECT_EQ(ProductCommandWindowLookupStatus::kStaleTarget,
            result.lookup_status);
  EXPECT_EQ(ProductCommandExecutionStatus::kPreconditionFailed,
            result.command.status);
  EXPECT_FALSE(registry);
  EXPECT_EQ(1, guard_count);
  EXPECT_EQ(0, authority.execute_count);
}

}  // namespace limitless
