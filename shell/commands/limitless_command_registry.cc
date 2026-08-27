// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_command_registry.h"

#include <array>
#include <cmath>
#include <utility>

#include "base/check.h"
#include "base/notreached.h"
#include "base/uuid.h"

namespace limitless {
namespace {

constexpr std::array<ProductCommandDescriptor, 11> kDescriptors = {{
    {ProductCommandId::kTabsNext, "tabs.next",
     ProductCommandArgumentKind::kNone,
     ProductCommandAuthority::kChromiumAndWorkspace},
    {ProductCommandId::kTabsPrevious, "tabs.previous",
     ProductCommandArgumentKind::kNone,
     ProductCommandAuthority::kChromiumAndWorkspace},
    {ProductCommandId::kTabsSelectIndex, "tabs.select_index",
     ProductCommandArgumentKind::kTabIndex,
     ProductCommandAuthority::kChromiumAndWorkspace},
    {ProductCommandId::kTabsCloseActive, "tabs.close_active",
     ProductCommandArgumentKind::kNone,
     ProductCommandAuthority::kChromiumAndWorkspace},
    {ProductCommandId::kSidebarToggle, "sidebar.toggle",
     ProductCommandArgumentKind::kNone,
     ProductCommandAuthority::kProductSidebar},
    {ProductCommandId::kSplitCreate, "split.create",
     ProductCommandArgumentKind::kSplitCreate,
     ProductCommandAuthority::kChromiumSplitModel},
    {ProductCommandId::kSplitFocus, "split.focus",
     ProductCommandArgumentKind::kSplitFocus,
     ProductCommandAuthority::kChromiumSplitModel},
    {ProductCommandId::kSplitSetLayout, "split.layout.set",
     ProductCommandArgumentKind::kSplitLayout,
     ProductCommandAuthority::kChromiumSplitModel},
    {ProductCommandId::kSplitSetRatio, "split.ratio.set",
     ProductCommandArgumentKind::kSplitRatio,
     ProductCommandAuthority::kChromiumSplitModel},
    {ProductCommandId::kSplitSwap, "split.swap",
     ProductCommandArgumentKind::kSplitPair,
     ProductCommandAuthority::kChromiumSplitModel},
    {ProductCommandId::kSplitUnsplit, "split.unsplit",
     ProductCommandArgumentKind::kSplitPair,
     ProductCommandAuthority::kChromiumSplitModel},
}};

bool IsCanonicalIdentifier(std::string_view identifier) {
  return base::Uuid::ParseLowercase(identifier).is_valid();
}

ProductCommandValidationStatus ValidatePair(
    const ProductCommandSplitPair& pair) {
  if (!IsCanonicalIdentifier(pair.window_identifier) ||
      !IsCanonicalIdentifier(pair.first_tab_identifier) ||
      !IsCanonicalIdentifier(pair.second_tab_identifier)) {
    return ProductCommandValidationStatus::kInvalidIdentifier;
  }
  if (pair.first_tab_identifier == pair.second_tab_identifier) {
    return ProductCommandValidationStatus::kSameTab;
  }
  return ProductCommandValidationStatus::kValid;
}

ProductCommandExecutionResult ExecutionResultForQuery(
    const ProductCommandQueryResult& query) {
  switch (query.status) {
    case ProductCommandQueryStatus::kEnabled:
      break;
    case ProductCommandQueryStatus::kDisabled:
      return {.status = ProductCommandExecutionStatus::kDisabled,
              .validation = query.validation};
    case ProductCommandQueryStatus::kUnsupported:
      return {.status = ProductCommandExecutionStatus::kUnsupported,
              .validation = query.validation};
    case ProductCommandQueryStatus::kInvalidInvocation:
      return {.status = ProductCommandExecutionStatus::kInvalidInvocation,
              .validation = query.validation};
  }
  return {.status = ProductCommandExecutionStatus::kFailed};
}

}  // namespace

std::span<const ProductCommandDescriptor> GetProductCommandDescriptors() {
  return kDescriptors;
}

const ProductCommandDescriptor& GetProductCommandDescriptor(
    ProductCommandId command) {
  for (const ProductCommandDescriptor& descriptor : kDescriptors) {
    if (descriptor.id == command) {
      return descriptor;
    }
  }
  NOTREACHED();
}

std::optional<ProductCommandId> ParseProductCommandName(std::string_view name) {
  for (const ProductCommandDescriptor& descriptor : kDescriptors) {
    if (descriptor.name == name) {
      return descriptor.id;
    }
  }
  return std::nullopt;
}

ProductCommandValidationStatus ValidateProductCommandInvocation(
    const ProductCommandInvocation& invocation) {
  if (invocation.schema_version != kProductCommandSchemaVersion) {
    return ProductCommandValidationStatus::kUnsupportedSchemaVersion;
  }

  switch (GetProductCommandDescriptor(invocation.command).argument_kind) {
    case ProductCommandArgumentKind::kNone:
      return std::holds_alternative<ProductCommandNoArguments>(
                 invocation.arguments)
                 ? ProductCommandValidationStatus::kValid
                 : ProductCommandValidationStatus::kArgumentTypeMismatch;
    case ProductCommandArgumentKind::kTabIndex: {
      const auto* index =
          std::get_if<ProductCommandTabIndex>(&invocation.arguments);
      if (!index) {
        return ProductCommandValidationStatus::kArgumentTypeMismatch;
      }
      return index->one_based_index >= 1 && index->one_based_index <= 9
                 ? ProductCommandValidationStatus::kValid
                 : ProductCommandValidationStatus::kInvalidTabIndex;
    }
    case ProductCommandArgumentKind::kSplitCreate: {
      const auto* create =
          std::get_if<ProductCommandSplitCreate>(&invocation.arguments);
      return create ? ValidatePair(create->pair)
                    : ProductCommandValidationStatus::kArgumentTypeMismatch;
    }
    case ProductCommandArgumentKind::kSplitFocus: {
      const auto* focus =
          std::get_if<ProductCommandSplitFocus>(&invocation.arguments);
      if (!focus) {
        return ProductCommandValidationStatus::kArgumentTypeMismatch;
      }
      const ProductCommandValidationStatus pair_status =
          ValidatePair(focus->pair);
      if (pair_status != ProductCommandValidationStatus::kValid) {
        return pair_status;
      }
      if (!IsCanonicalIdentifier(focus->member_tab_identifier)) {
        return ProductCommandValidationStatus::kInvalidIdentifier;
      }
      return focus->member_tab_identifier == focus->pair.first_tab_identifier ||
                     focus->member_tab_identifier ==
                         focus->pair.second_tab_identifier
                 ? ProductCommandValidationStatus::kValid
                 : ProductCommandValidationStatus::kMemberNotInPair;
    }
    case ProductCommandArgumentKind::kSplitLayout: {
      const auto* layout =
          std::get_if<ProductCommandSetSplitLayout>(&invocation.arguments);
      return layout ? ValidatePair(layout->pair)
                    : ProductCommandValidationStatus::kArgumentTypeMismatch;
    }
    case ProductCommandArgumentKind::kSplitRatio: {
      const auto* ratio =
          std::get_if<ProductCommandSetSplitRatio>(&invocation.arguments);
      if (!ratio) {
        return ProductCommandValidationStatus::kArgumentTypeMismatch;
      }
      const ProductCommandValidationStatus pair_status =
          ValidatePair(ratio->pair);
      if (pair_status != ProductCommandValidationStatus::kValid) {
        return pair_status;
      }
      return std::isfinite(ratio->ratio) && ratio->ratio >= 0.25 &&
                     ratio->ratio <= 0.75
                 ? ProductCommandValidationStatus::kValid
                 : ProductCommandValidationStatus::kInvalidRatio;
    }
    case ProductCommandArgumentKind::kSplitPair: {
      const auto* pair =
          std::get_if<ProductCommandSplitPair>(&invocation.arguments);
      return pair ? ValidatePair(*pair)
                  : ProductCommandValidationStatus::kArgumentTypeMismatch;
    }
  }
  NOTREACHED();
}

ProductCommandRegistry::ProductCommandRegistry(QueryCallback query,
                                               ExecuteCallback execute)
    : query_(std::move(query)), execute_(std::move(execute)) {
  CHECK(query_);
  CHECK(execute_);
}

ProductCommandRegistry::~ProductCommandRegistry() = default;

ProductCommandQueryResult ProductCommandRegistry::Query(
    const ProductCommandInvocation& invocation) const {
  const ProductCommandValidationStatus validation =
      ValidateProductCommandInvocation(invocation);
  if (validation != ProductCommandValidationStatus::kValid) {
    return {.status = ProductCommandQueryStatus::kInvalidInvocation,
            .validation = validation};
  }
  QueryCallback query = query_;
  return query.Run(invocation);
}

ProductCommandExecutionResult ProductCommandRegistry::Execute(
    const ProductCommandInvocation& invocation,
    ProductCommandOrigin origin) const {
  QueryCallback query_callback = query_;
  ExecuteCallback execute_callback = execute_;
  const ProductCommandValidationStatus validation =
      ValidateProductCommandInvocation(invocation);
  if (validation != ProductCommandValidationStatus::kValid) {
    return {.status = ProductCommandExecutionStatus::kInvalidInvocation,
            .validation = validation};
  }
  const ProductCommandQueryResult query = query_callback.Run(invocation);
  if (query.status != ProductCommandQueryStatus::kEnabled) {
    return ExecutionResultForQuery(query);
  }
  return execute_callback.Run(invocation, origin);
}

ProductCommandExecutionResult ProductCommandRegistry::ExecuteGuarded(
    const ProductCommandInvocation& invocation,
    ProductCommandOrigin origin,
    PreExecuteGuard guard) const {
  CHECK(guard);
  QueryCallback query_callback = query_;
  ExecuteCallback execute_callback = execute_;
  const ProductCommandValidationStatus validation =
      ValidateProductCommandInvocation(invocation);
  if (validation != ProductCommandValidationStatus::kValid) {
    return {.status = ProductCommandExecutionStatus::kInvalidInvocation,
            .validation = validation};
  }
  const ProductCommandQueryResult query = query_callback.Run(invocation);
  if (query.status != ProductCommandQueryStatus::kEnabled) {
    return ExecutionResultForQuery(query);
  }
  const ProductCommandPreExecuteDecision decision =
      std::move(guard).Run(invocation);
  if (decision != ProductCommandPreExecuteDecision::kAllow) {
    return {.status =
                decision == ProductCommandPreExecuteDecision::kDeadlineExceeded
                    ? ProductCommandExecutionStatus::kDeadlineExceeded
                    : ProductCommandExecutionStatus::kPreconditionFailed};
  }
  return execute_callback.Run(invocation, origin);
}

}  // namespace limitless
