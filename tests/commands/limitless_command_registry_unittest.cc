// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_registry.h"

#include <limits>
#include <memory>
#include <set>
#include <string>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "shell/commands/limitless_command_window_binding.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

constexpr char kWindowId[] = "10000000-0000-4000-8000-000000000001";
constexpr char kFirstTabId[] = "20000000-0000-4000-8000-000000000002";
constexpr char kSecondTabId[] = "30000000-0000-4000-8000-000000000003";
constexpr char kForeignTabId[] = "40000000-0000-4000-8000-000000000004";
constexpr char kThirdTabId[] = "50000000-0000-4000-8000-000000000005";
constexpr char kFourthTabId[] = "60000000-0000-4000-8000-000000000006";
constexpr char kSecondSplitId[] = "70000000-0000-4000-8000-000000000007";

ProductCommandSplitPair ValidPair() {
  return {
      .window_identifier = kWindowId,
      .first_tab_identifier = kFirstTabId,
      .second_tab_identifier = kSecondTabId,
  };
}

TEST(LimitlessCommandRegistryTest, DescriptorsHaveStableUniqueNames) {
  std::set<std::string_view> names;
  std::set<ProductCommandId> identifiers;
  for (const ProductCommandDescriptor& descriptor :
       GetProductCommandDescriptors()) {
    EXPECT_TRUE(names.insert(descriptor.name).second);
    EXPECT_TRUE(identifiers.insert(descriptor.id).second);
    EXPECT_EQ(descriptor.id, ParseProductCommandName(descriptor.name));
    EXPECT_EQ(descriptor.name, GetProductCommandDescriptor(descriptor.id).name);
  }
  EXPECT_EQ(11u, names.size());
  EXPECT_FALSE(ParseProductCommandName("split.raw_handle"));
}

TEST(LimitlessCommandRegistryTest, ValidatesVersionAndTypedArguments) {
  ProductCommandInvocation invocation;
  EXPECT_EQ(ProductCommandValidationStatus::kValid,
            ValidateProductCommandInvocation(invocation));

  invocation.schema_version = 2;
  EXPECT_EQ(ProductCommandValidationStatus::kUnsupportedSchemaVersion,
            ValidateProductCommandInvocation(invocation));

  invocation = {
      .command = ProductCommandId::kTabsSelectIndex,
      .arguments = ProductCommandTabIndex{.one_based_index = 9},
  };
  EXPECT_EQ(ProductCommandValidationStatus::kValid,
            ValidateProductCommandInvocation(invocation));
  invocation.arguments = ProductCommandTabIndex{.one_based_index = 10};
  EXPECT_EQ(ProductCommandValidationStatus::kInvalidTabIndex,
            ValidateProductCommandInvocation(invocation));
  invocation.arguments = ProductCommandNoArguments{};
  EXPECT_EQ(ProductCommandValidationStatus::kArgumentTypeMismatch,
            ValidateProductCommandInvocation(invocation));
}

TEST(LimitlessCommandRegistryTest, ValidatesCanonicalSplitTargetsAndBounds) {
  ProductCommandInvocation focus{
      .command = ProductCommandId::kSplitFocus,
      .arguments =
          ProductCommandSplitFocus{
              .pair = ValidPair(),
              .member_tab_identifier = kFirstTabId,
          },
  };
  EXPECT_EQ(ProductCommandValidationStatus::kValid,
            ValidateProductCommandInvocation(focus));

  std::get<ProductCommandSplitFocus>(focus.arguments).member_tab_identifier =
      kForeignTabId;
  EXPECT_EQ(ProductCommandValidationStatus::kMemberNotInPair,
            ValidateProductCommandInvocation(focus));

  ProductCommandSplitPair same_tab = ValidPair();
  same_tab.second_tab_identifier = same_tab.first_tab_identifier;
  ProductCommandInvocation swap{
      .command = ProductCommandId::kSplitSwap,
      .arguments = same_tab,
  };
  EXPECT_EQ(ProductCommandValidationStatus::kSameTab,
            ValidateProductCommandInvocation(swap));

  ProductCommandInvocation ratio{
      .command = ProductCommandId::kSplitSetRatio,
      .arguments =
          ProductCommandSetSplitRatio{
              .pair = ValidPair(),
              .ratio = 0.25,
          },
  };
  EXPECT_EQ(ProductCommandValidationStatus::kValid,
            ValidateProductCommandInvocation(ratio));
  std::get<ProductCommandSetSplitRatio>(ratio.arguments).ratio = 0.75;
  EXPECT_EQ(ProductCommandValidationStatus::kValid,
            ValidateProductCommandInvocation(ratio));
  std::get<ProductCommandSetSplitRatio>(ratio.arguments).ratio =
      std::numeric_limits<double>::infinity();
  EXPECT_EQ(ProductCommandValidationStatus::kInvalidRatio,
            ValidateProductCommandInvocation(ratio));
}

TEST(LimitlessCommandRegistryTest, InvalidInvocationNeverReachesAdapter) {
  int query_calls = 0;
  int execute_calls = 0;
  ProductCommandRegistry registry(
      base::BindRepeating(
          [](int* calls, const ProductCommandInvocation&) {
            ++*calls;
            return ProductCommandQueryResult{
                .status = ProductCommandQueryStatus::kEnabled};
          },
          &query_calls),
      base::BindRepeating(
          [](int* calls, const ProductCommandInvocation&,
             ProductCommandOrigin) {
            ++*calls;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          },
          &execute_calls));

  ProductCommandInvocation invalid{
      .command = ProductCommandId::kTabsSelectIndex,
      .arguments = ProductCommandTabIndex{.one_based_index = 0},
  };
  EXPECT_EQ(ProductCommandQueryStatus::kInvalidInvocation,
            registry.Query(invalid).status);
  EXPECT_EQ(ProductCommandExecutionStatus::kInvalidInvocation,
            registry.Execute(invalid, ProductCommandOrigin::kAgent).status);
  EXPECT_EQ(0, query_calls);
  EXPECT_EQ(0, execute_calls);
}

TEST(LimitlessCommandRegistryTest, ExecuteQueriesLiveBeforeEveryDispatch) {
  bool enabled = false;
  int query_calls = 0;
  int execute_calls = 0;
  std::optional<ProductCommandOrigin> last_origin;
  ProductCommandRegistry registry(
      base::BindRepeating(
          [](bool* enabled, int* calls, const ProductCommandInvocation&) {
            ++*calls;
            return ProductCommandQueryResult{
                .status = *enabled ? ProductCommandQueryStatus::kEnabled
                                   : ProductCommandQueryStatus::kDisabled};
          },
          &enabled, &query_calls),
      base::BindRepeating(
          [](int* calls, std::optional<ProductCommandOrigin>* last_origin,
             const ProductCommandInvocation&, ProductCommandOrigin origin) {
            ++*calls;
            *last_origin = origin;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          },
          &execute_calls, &last_origin));

  const ProductCommandInvocation invocation{
      .command = ProductCommandId::kSidebarToggle,
  };
  EXPECT_EQ(
      ProductCommandExecutionStatus::kDisabled,
      registry.Execute(invocation, ProductCommandOrigin::kNativeUi).status);
  EXPECT_EQ(1, query_calls);
  EXPECT_EQ(0, execute_calls);

  enabled = true;
  EXPECT_EQ(ProductCommandExecutionStatus::kSucceeded,
            registry.Execute(invocation, ProductCommandOrigin::kLeaderShortcut)
                .status);
  EXPECT_EQ(2, query_calls);
  EXPECT_EQ(1, execute_calls);
  EXPECT_EQ(ProductCommandOrigin::kLeaderShortcut, last_origin);
}

TEST(LimitlessCommandRegistryTest,
     GuardedExecutionQueriesThenGuardsThenExecutes) {
  std::string order;
  ProductCommandOrigin seen = ProductCommandOrigin::kNativeUi;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        order += "q";
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin origin) {
            order += "e";
            seen = origin;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));
  EXPECT_EQ(
      ProductCommandExecutionStatus::kSucceeded,
      registry
          .ExecuteGuarded({.command = ProductCommandId::kSidebarToggle},
                          ProductCommandOrigin::kExternal,
                          base::BindLambdaForTesting(
                              [&](const ProductCommandInvocation&) mutable {
                                order += "g";
                                return ProductCommandPreExecuteDecision::kAllow;
                              }))
          .status);
  EXPECT_EQ("qge", order);
  EXPECT_EQ(ProductCommandOrigin::kExternal, seen);
}

TEST(LimitlessCommandRegistryTest,
     GuardFailuresReturnExactStatusAndDoNotExecute) {
  int query_calls = 0;
  int guard_calls = 0;
  int execute_calls = 0;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        ++query_calls;
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++execute_calls;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));

  const ProductCommandInvocation invocation{
      .command = ProductCommandId::kSidebarToggle};
  EXPECT_EQ(
      ProductCommandExecutionStatus::kDeadlineExceeded,
      registry
          .ExecuteGuarded(
              invocation, ProductCommandOrigin::kExternal,
              base::BindLambdaForTesting(
                  [&](const ProductCommandInvocation&) mutable {
                    ++guard_calls;
                    return ProductCommandPreExecuteDecision::kDeadlineExceeded;
                  }))
          .status);
  EXPECT_EQ(
      ProductCommandExecutionStatus::kPreconditionFailed,
      registry
          .ExecuteGuarded(invocation, ProductCommandOrigin::kExternal,
                          base::BindLambdaForTesting(
                              [&](const ProductCommandInvocation&) mutable {
                                ++guard_calls;
                                return ProductCommandPreExecuteDecision::
                                    kPreconditionFailed;
                              }))
          .status);
  EXPECT_EQ(2, query_calls);
  EXPECT_EQ(2, guard_calls);
  EXPECT_EQ(0, execute_calls);
}

TEST(LimitlessCommandRegistryTest, DisabledQuerySkipsGuardAndExecution) {
  int query_calls = 0;
  int guard_calls = 0;
  int execute_calls = 0;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        ++query_calls;
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kDisabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++execute_calls;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));

  EXPECT_EQ(
      ProductCommandExecutionStatus::kDisabled,
      registry
          .ExecuteGuarded({.command = ProductCommandId::kSidebarToggle},
                          ProductCommandOrigin::kExternal,
                          base::BindLambdaForTesting(
                              [&](const ProductCommandInvocation&) mutable {
                                ++guard_calls;
                                return ProductCommandPreExecuteDecision::kAllow;
                              }))
          .status);
  EXPECT_EQ(1, query_calls);
  EXPECT_EQ(0, guard_calls);
  EXPECT_EQ(0, execute_calls);
}

TEST(LimitlessCommandRegistryTest,
     InvalidInvocationSkipsQueryGuardAndExecution) {
  int query_calls = 0;
  int guard_calls = 0;
  int execute_calls = 0;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        ++query_calls;
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++execute_calls;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));

  EXPECT_EQ(
      ProductCommandExecutionStatus::kInvalidInvocation,
      registry
          .ExecuteGuarded({.command = ProductCommandId::kTabsSelectIndex,
                           .arguments = ProductCommandNoArguments{}},
                          ProductCommandOrigin::kExternal,
                          base::BindLambdaForTesting(
                              [&](const ProductCommandInvocation&) mutable {
                                ++guard_calls;
                                return ProductCommandPreExecuteDecision::kAllow;
                              }))
          .status);
  EXPECT_EQ(0, query_calls);
  EXPECT_EQ(0, guard_calls);
  EXPECT_EQ(0, execute_calls);
}

TEST(LimitlessCommandRegistryTest,
     GuardedExecutionSurvivesOwnerDestructionDuringQuery) {
  int guard_calls = 0;
  int execute_calls = 0;
  std::unique_ptr<ProductCommandRegistry> registry;
  registry = std::make_unique<ProductCommandRegistry>(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        registry.reset();
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++execute_calls;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));

  EXPECT_EQ(ProductCommandExecutionStatus::kSucceeded,
            registry
                ->ExecuteGuarded(
                    {.command = ProductCommandId::kSidebarToggle},
                    ProductCommandOrigin::kExternal,
                    base::BindLambdaForTesting(
                        [&](const ProductCommandInvocation&) mutable {
                          ++guard_calls;
                          return ProductCommandPreExecuteDecision::kAllow;
                        }))
                .status);
  EXPECT_FALSE(registry);
  EXPECT_EQ(1, guard_calls);
  EXPECT_EQ(1, execute_calls);
}

TEST(LimitlessCommandRegistryTest,
     GuardedExecutionSurvivesOwnerDestructionDuringGuard) {
  int execute_calls = 0;
  std::unique_ptr<ProductCommandRegistry> registry;
  registry = std::make_unique<ProductCommandRegistry>(
      base::BindLambdaForTesting([](const ProductCommandInvocation&) {
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++execute_calls;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));

  EXPECT_EQ(ProductCommandExecutionStatus::kSucceeded,
            registry
                ->ExecuteGuarded(
                    {.command = ProductCommandId::kSidebarToggle},
                    ProductCommandOrigin::kExternal,
                    base::BindLambdaForTesting(
                        [&](const ProductCommandInvocation&) mutable {
                          registry.reset();
                          return ProductCommandPreExecuteDecision::kAllow;
                        }))
                .status);
  EXPECT_FALSE(registry);
  EXPECT_EQ(1, execute_calls);
}

TEST(ProductCommandWindowBindingTest, ReturnsAbsentWindowState) {
  int state_calls = 0;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([](const ProductCommandInvocation&) {
        return ProductCommandQueryResult{};
      }),
      base::BindLambdaForTesting(
          [](const ProductCommandInvocation&, ProductCommandOrigin) {
            return ProductCommandExecutionResult{};
          }));
  ProductCommandWindowBinding binding(
      registry.GetWeakPtr(),
      base::BindLambdaForTesting(
          [&]() -> std::optional<ProductCommandWindowState> {
            ++state_calls;
            return std::nullopt;
          }));
  EXPECT_FALSE(binding.GetState());
  EXPECT_EQ(1, state_calls);
}

TEST(ProductCommandWindowBindingTest, DelegatesGuardedExecution) {
  std::string order;
  ProductCommandOrigin seen = ProductCommandOrigin::kNativeUi;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        order += "q";
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin origin) {
            order += "e";
            seen = origin;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));
  ProductCommandWindowBinding binding(
      registry.GetWeakPtr(),
      base::BindLambdaForTesting(
          []() -> std::optional<ProductCommandWindowState> {
            return ProductCommandWindowState{.window_id = kWindowId,
                                             .active_workspace_id = kWindowId,
                                             .visible_tab_ids = {kFirstTabId},
                                             .active_tab_id = kFirstTabId};
          }));

  EXPECT_EQ(
      ProductCommandExecutionStatus::kSucceeded,
      binding
          .ExecuteGuarded({.command = ProductCommandId::kSidebarToggle},
                          ProductCommandOrigin::kExternal,
                          base::BindLambdaForTesting(
                              [&](const ProductCommandInvocation&) mutable {
                                order += "g";
                                return ProductCommandPreExecuteDecision::kAllow;
                              }))
          .status);
  EXPECT_EQ("qge", order);
  EXPECT_EQ(ProductCommandOrigin::kExternal, seen);
}

TEST(ProductCommandWindowBindingTest, WeakPointerInvalidatesAfterDestruction) {
  base::WeakPtr<ProductCommandWindowBinding> weak;
  {
    ProductCommandRegistry registry(
        base::BindLambdaForTesting([](const ProductCommandInvocation&) {
          return ProductCommandQueryResult{};
        }),
        base::BindLambdaForTesting(
            [](const ProductCommandInvocation&, ProductCommandOrigin) {
              return ProductCommandExecutionResult{};
            }));
    auto binding = std::make_unique<ProductCommandWindowBinding>(
        registry.GetWeakPtr(),
        base::BindLambdaForTesting(
            []() -> std::optional<ProductCommandWindowState> {
              return ProductCommandWindowState{.window_id = kWindowId};
            }));
    weak = binding->GetWeakPtr();
    EXPECT_TRUE(weak);
  }
  EXPECT_FALSE(weak);
}

TEST(ProductCommandWindowBindingTest, ExpiredRegistryFailsClosed) {
  int guards = 0;
  int executes = 0;
  auto registry = std::make_unique<ProductCommandRegistry>(
      base::BindLambdaForTesting([](const ProductCommandInvocation&) {
        return ProductCommandQueryResult{
            .status = ProductCommandQueryStatus::kEnabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++executes;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));
  ProductCommandWindowBinding binding(
      registry->GetWeakPtr(),
      base::BindLambdaForTesting(
          []() -> std::optional<ProductCommandWindowState> {
            return ProductCommandWindowState{.window_id = kWindowId,
                                             .active_workspace_id = kWindowId,
                                             .visible_tab_ids = {kFirstTabId},
                                             .active_tab_id = kFirstTabId};
          }));
  registry.reset();
  EXPECT_FALSE(binding.GetState());
  ProductCommandInvocation invocation{.command = ProductCommandId::kTabsNext};
  EXPECT_EQ(ProductCommandQueryStatus::kDisabled,
            binding.Query(invocation).status);
  EXPECT_EQ(
      ProductCommandExecutionStatus::kPreconditionFailed,
      binding
          .ExecuteGuarded(
              invocation, ProductCommandOrigin::kExternal,
              base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
                ++guards;
                return ProductCommandPreExecuteDecision::kAllow;
              }))
          .status);
  EXPECT_EQ(0, guards);
  EXPECT_EQ(0, executes);
}

TEST(ProductCommandWindowBindingTest, MalformedStateFailsClosed) {
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([](const ProductCommandInvocation&) {
        return ProductCommandQueryResult{};
      }),
      base::BindLambdaForTesting(
          [](const ProductCommandInvocation&, ProductCommandOrigin) {
            return ProductCommandExecutionResult{};
          }));
  ProductCommandWindowBinding binding(
      registry.GetWeakPtr(),
      base::BindLambdaForTesting(
          []() -> std::optional<ProductCommandWindowState> {
            return ProductCommandWindowState{.window_id = "malformed"};
          }));
  EXPECT_FALSE(binding.GetState());
}

TEST(ProductCommandWindowBindingTest,
     WindowStateValidatorRejectsMalformedShapes) {
  ProductCommandWindowState valid{
      .window_id = kWindowId,
      .active_workspace_id = kWindowId,
      .visible_tab_ids = {kFirstTabId, kSecondTabId},
      .active_tab_id = kFirstTabId};
  EXPECT_TRUE(ValidateProductCommandWindowState(valid));
  ProductCommandWindowState duplicate_tab = valid;
  duplicate_tab.visible_tab_ids.push_back(kFirstTabId);
  EXPECT_FALSE(ValidateProductCommandWindowState(duplicate_tab));
  ProductCommandWindowState missing_active = valid;
  missing_active.active_tab_id = kForeignTabId;
  EXPECT_FALSE(ValidateProductCommandWindowState(missing_active));
  ProductCommandWindowState bad_split = valid;
  bad_split.splits.push_back({.split_instance_id = kWindowId,
                              .first_tab_id = kFirstTabId,
                              .second_tab_id = kForeignTabId,
                              .ratio = 0.5});
  EXPECT_FALSE(ValidateProductCommandWindowState(bad_split));
  ProductCommandWindowState bad_ratio = valid;
  bad_ratio.splits.push_back({.split_instance_id = kWindowId,
                              .first_tab_id = kFirstTabId,
                              .second_tab_id = kSecondTabId,
                              .ratio = 0.9});
  EXPECT_FALSE(ValidateProductCommandWindowState(bad_ratio));
  ProductCommandWindowState valid_split = valid;
  valid_split.splits.push_back({.split_instance_id = kWindowId,
                                .first_tab_id = kFirstTabId,
                                .second_tab_id = kSecondTabId,
                                .active_tab_id = kFirstTabId,
                                .ratio = 0.25});
  EXPECT_TRUE(ValidateProductCommandWindowState(valid_split));
  valid_split.splits[0].ratio = 0.75;
  EXPECT_TRUE(ValidateProductCommandWindowState(valid_split));
  valid_split.splits[0].ratio = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(ValidateProductCommandWindowState(valid_split));
  ProductCommandWindowState duplicate_split = valid;
  duplicate_split.splits = {valid_split.splits[0], valid_split.splits[0]};
  duplicate_split.splits[0].ratio = 0.5;
  EXPECT_FALSE(ValidateProductCommandWindowState(duplicate_split));
  ProductCommandWindowState same_member = valid;
  same_member.splits.push_back({.split_instance_id = kWindowId,
                                .first_tab_id = kFirstTabId,
                                .second_tab_id = kFirstTabId});
  EXPECT_FALSE(ValidateProductCommandWindowState(same_member));
  ProductCommandWindowState active_outside = valid_split;
  active_outside.splits[0].active_tab_id = kSecondTabId;
  active_outside.active_tab_id = kFirstTabId;
  EXPECT_FALSE(ValidateProductCommandWindowState(active_outside));
  ProductCommandWindowState reused_member = valid;
  reused_member.visible_tab_ids.push_back(kForeignTabId);
  reused_member.splits = {valid_split.splits[0],
                          {.split_instance_id = kForeignTabId,
                           .first_tab_id = kFirstTabId,
                           .second_tab_id = kForeignTabId}};
  EXPECT_FALSE(ValidateProductCommandWindowState(reused_member));
  ProductCommandWindowState two_active = valid_split;
  two_active.splits.push_back({.split_instance_id = kForeignTabId,
                               .first_tab_id = kThirdTabId,
                               .second_tab_id = kFourthTabId,
                               .active_tab_id = kThirdTabId});
  two_active.visible_tab_ids.insert(two_active.visible_tab_ids.end(),
                                    {kThirdTabId, kFourthTabId});
  two_active.splits[0].split_instance_id = kSecondSplitId;
  EXPECT_FALSE(ValidateProductCommandWindowState(two_active));
  ProductCommandWindowState invalid_layout = valid_split;
  invalid_layout.splits[0].layout = static_cast<ProductCommandSplitLayout>(999);
  EXPECT_FALSE(ValidateProductCommandWindowState(invalid_layout));
  ProductCommandWindowState marker_outside = valid_split;
  marker_outside.visible_tab_ids.push_back(kThirdTabId);
  marker_outside.splits[0].active_tab_id = kThirdTabId;
  EXPECT_FALSE(ValidateProductCommandWindowState(marker_outside));
  ProductCommandWindowState missing_marker = valid_split;
  missing_marker.splits[0].active_tab_id.reset();
  EXPECT_FALSE(ValidateProductCommandWindowState(missing_marker));
}

TEST(ProductCommandWindowBindingTest, StateCallbackCanExpireRegistry) {
  auto registry = std::make_unique<ProductCommandRegistry>(
      base::BindLambdaForTesting([](const ProductCommandInvocation&) {
        return ProductCommandQueryResult{};
      }),
      base::BindLambdaForTesting(
          [](const ProductCommandInvocation&, ProductCommandOrigin) {
            return ProductCommandExecutionResult{};
          }));
  ProductCommandWindowBinding binding(
      registry->GetWeakPtr(),
      base::BindLambdaForTesting(
          [&]() -> std::optional<ProductCommandWindowState> {
            registry.reset();
            return ProductCommandWindowState{.window_id = kWindowId,
                                             .active_workspace_id = kWindowId,
                                             .visible_tab_ids = {kFirstTabId},
                                             .active_tab_id = kFirstTabId};
          }));
  EXPECT_FALSE(binding.GetState());
}

TEST(LimitlessCommandRegistryTest, LiveRequeryRejectsChangedState) {
  bool enabled = true;
  int guards = 0;
  int executes = 0;
  ProductCommandRegistry registry(
      base::BindLambdaForTesting([&](const ProductCommandInvocation&) {
        return ProductCommandQueryResult{
            .status = enabled ? ProductCommandQueryStatus::kEnabled
                              : ProductCommandQueryStatus::kDisabled};
      }),
      base::BindLambdaForTesting(
          [&](const ProductCommandInvocation&, ProductCommandOrigin) {
            ++executes;
            return ProductCommandExecutionResult{
                .status = ProductCommandExecutionStatus::kSucceeded};
          }));
  EXPECT_EQ(
      ProductCommandQueryStatus::kEnabled,
      registry.Query({.command = ProductCommandId::kSidebarToggle}).status);
  enabled = false;
  EXPECT_EQ(
      ProductCommandExecutionStatus::kDisabled,
      registry
          .ExecuteGuarded({.command = ProductCommandId::kSidebarToggle},
                          ProductCommandOrigin::kExternal,
                          base::BindLambdaForTesting(
                              [&](const ProductCommandInvocation&) mutable {
                                ++guards;
                                return ProductCommandPreExecuteDecision::kAllow;
                              }))
          .status);
  EXPECT_EQ(0, guards);
  EXPECT_EQ(0, executes);
}

}  // namespace
}  // namespace limitless
