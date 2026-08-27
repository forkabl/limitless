// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_REGISTRY_H_
#define LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_REGISTRY_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "base/functional/callback.h"

namespace limitless {

inline constexpr uint32_t kProductCommandSchemaVersion = 1;

enum class ProductCommandId {
  kTabsNext,
  kTabsPrevious,
  kTabsSelectIndex,
  kTabsCloseActive,
  kSidebarToggle,
  kSplitCreate,
  kSplitFocus,
  kSplitSetLayout,
  kSplitSetRatio,
  kSplitSwap,
  kSplitUnsplit,
};

enum class ProductCommandArgumentKind {
  kNone,
  kTabIndex,
  kSplitCreate,
  kSplitFocus,
  kSplitLayout,
  kSplitRatio,
  kSplitPair,
};

// Identifies the model that must be queried immediately before execution.
// The registry never caches state from any authority.
enum class ProductCommandAuthority {
  kChromiumAndWorkspace,
  kProductSidebar,
  kChromiumSplitModel,
};

struct ProductCommandDescriptor {
  ProductCommandId id;
  std::string_view name;
  ProductCommandArgumentKind argument_kind;
  ProductCommandAuthority authority;
};

struct ProductCommandNoArguments {};

struct ProductCommandTabIndex {
  uint32_t one_based_index = 0;
};

struct ProductCommandSplitPair {
  std::string window_identifier;
  std::string first_tab_identifier;
  std::string second_tab_identifier;
};

struct ProductCommandSplitCreate {
  ProductCommandSplitPair pair;
};

struct ProductCommandSplitFocus {
  ProductCommandSplitPair pair;
  std::string member_tab_identifier;
};

enum class ProductCommandSplitLayout {
  kSideBySide,
  kStacked,
};

struct ProductCommandSetSplitLayout {
  ProductCommandSplitPair pair;
  ProductCommandSplitLayout layout = ProductCommandSplitLayout::kSideBySide;
};

struct ProductCommandSetSplitRatio {
  ProductCommandSplitPair pair;
  double ratio = 0.5;
};

using ProductCommandArguments = std::variant<ProductCommandNoArguments,
                                             ProductCommandTabIndex,
                                             ProductCommandSplitCreate,
                                             ProductCommandSplitFocus,
                                             ProductCommandSetSplitLayout,
                                             ProductCommandSetSplitRatio,
                                             ProductCommandSplitPair>;

struct ProductCommandInvocation {
  uint32_t schema_version = kProductCommandSchemaVersion;
  ProductCommandId command = ProductCommandId::kTabsNext;
  ProductCommandArguments arguments = ProductCommandNoArguments{};
};

enum class ProductCommandOrigin {
  kNativeUi,
  kLeaderShortcut,
  kCommandPalette,
  kCli,
  kAgent,
};

enum class ProductCommandValidationStatus {
  kValid,
  kUnsupportedSchemaVersion,
  kArgumentTypeMismatch,
  kInvalidTabIndex,
  kInvalidIdentifier,
  kSameTab,
  kMemberNotInPair,
  kInvalidRatio,
};

enum class ProductCommandQueryStatus {
  kEnabled,
  kDisabled,
  kUnsupported,
  kInvalidInvocation,
};

struct ProductCommandQueryResult {
  ProductCommandQueryStatus status = ProductCommandQueryStatus::kUnsupported;
  ProductCommandValidationStatus validation =
      ProductCommandValidationStatus::kValid;
};

enum class ProductCommandExecutionStatus {
  kSucceeded,
  kDisabled,
  kUnsupported,
  kInvalidInvocation,
  kFailed,
};

struct ProductCommandExecutionResult {
  ProductCommandExecutionStatus status = ProductCommandExecutionStatus::kFailed;
  ProductCommandValidationStatus validation =
      ProductCommandValidationStatus::kValid;
};

std::span<const ProductCommandDescriptor> GetProductCommandDescriptors();
const ProductCommandDescriptor& GetProductCommandDescriptor(
    ProductCommandId command);
std::optional<ProductCommandId> ParseProductCommandName(std::string_view name);
ProductCommandValidationStatus ValidateProductCommandInvocation(
    const ProductCommandInvocation& invocation);

// Stateless dispatch boundary for menus, shortcuts, palettes, CLI, and agents.
// It stores adapters but no browser or command state. Query is evaluated for
// every request, including immediately before Execute calls the authoritative
// executor. The executor must still revalidate because authority can change
// during callbacks.
class ProductCommandRegistry {
 public:
  using QueryCallback = base::RepeatingCallback<ProductCommandQueryResult(
      const ProductCommandInvocation& invocation)>;
  using ExecuteCallback = base::RepeatingCallback<ProductCommandExecutionResult(
      const ProductCommandInvocation& invocation,
      ProductCommandOrigin origin)>;

  ProductCommandRegistry(QueryCallback query, ExecuteCallback execute);
  ProductCommandRegistry(const ProductCommandRegistry&) = delete;
  ProductCommandRegistry& operator=(const ProductCommandRegistry&) = delete;
  ~ProductCommandRegistry();

  ProductCommandQueryResult Query(
      const ProductCommandInvocation& invocation) const;
  ProductCommandExecutionResult Execute(
      const ProductCommandInvocation& invocation,
      ProductCommandOrigin origin) const;

 private:
  QueryCallback query_;
  ExecuteCallback execute_;
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_REGISTRY_H_
