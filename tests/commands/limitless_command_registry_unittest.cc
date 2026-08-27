// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_registry.h"

#include <limits>
#include <set>
#include <string>

#include "base/functional/bind.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

constexpr char kWindowId[] = "10000000-0000-4000-8000-000000000001";
constexpr char kFirstTabId[] = "20000000-0000-4000-8000-000000000002";
constexpr char kSecondTabId[] = "30000000-0000-4000-8000-000000000003";
constexpr char kForeignTabId[] = "40000000-0000-4000-8000-000000000004";

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

}  // namespace
}  // namespace limitless
