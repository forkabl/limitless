// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_window_binding.h"

#include <cmath>
#include <set>
#include <utility>

#include "base/check.h"
#include "base/uuid.h"

namespace limitless {

bool ValidateProductCommandWindowState(const ProductCommandWindowState& state) {
  const auto canonical = [](const std::string& value) {
    const base::Uuid uuid = base::Uuid::ParseLowercase(value);
    return uuid.is_valid() && uuid.AsLowercaseString() == value;
  };
  if (!canonical(state.window_id) || !state.active_workspace_id ||
      !canonical(*state.active_workspace_id) || !state.active_tab_id) {
    return false;
  }
  std::set<std::string> tabs;
  for (const std::string& tab : state.visible_tab_ids) {
    if (!canonical(tab) || !tabs.insert(tab).second) {
      return false;
    }
  }
  if (!tabs.contains(*state.active_tab_id)) {
    return false;
  }
  std::set<std::string> split_ids;
  std::set<std::string> split_members;
  size_t active_splits = 0;
  for (const ProductCommandSplitState& split : state.splits) {
    if (!canonical(split.split_instance_id) ||
        !split_ids.insert(split.split_instance_id).second ||
        !canonical(split.first_tab_id) || !canonical(split.second_tab_id) ||
        split.first_tab_id == split.second_tab_id ||
        !tabs.contains(split.first_tab_id) ||
        !tabs.contains(split.second_tab_id) ||
        !split_members.insert(split.first_tab_id).second ||
        !split_members.insert(split.second_tab_id).second ||
        !std::isfinite(split.ratio) || split.ratio < 0.25 ||
        split.ratio > 0.75 ||
        (split.active_tab_id && *split.active_tab_id != split.first_tab_id &&
         *split.active_tab_id != split.second_tab_id) ||
        (split.active_tab_id && ++active_splits > 1) ||
        (split.active_tab_id && *split.active_tab_id != *state.active_tab_id)) {
      return false;
    }
  }
  return true;
}

ProductCommandWindowBinding::ProductCommandWindowBinding(
    base::WeakPtr<ProductCommandRegistry> registry,
    StateCallback state)
    : state_(std::move(state)), registry_(std::move(registry)) {
  CHECK(registry_);
  CHECK(state_);
}

ProductCommandWindowBinding::~ProductCommandWindowBinding() = default;

base::WeakPtr<ProductCommandWindowBinding>
ProductCommandWindowBinding::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

std::optional<ProductCommandWindowState> ProductCommandWindowBinding::GetState()
    const {
  StateCallback state = state_;
  std::optional<ProductCommandWindowState> value = state.Run();
  return value && ValidateProductCommandWindowState(*value) ? value
                                                            : std::nullopt;
}

ProductCommandQueryResult ProductCommandWindowBinding::Query(
    const ProductCommandInvocation& invocation) const {
  base::WeakPtr<ProductCommandRegistry> registry = registry_;
  return registry ? registry->Query(invocation)
                  : ProductCommandQueryResult{
                        .status = ProductCommandQueryStatus::kDisabled};
}

ProductCommandExecutionResult ProductCommandWindowBinding::ExecuteGuarded(
    const ProductCommandInvocation& invocation,
    ProductCommandOrigin origin,
    ProductCommandRegistry::PreExecuteGuard guard) const {
  CHECK(guard);
  base::WeakPtr<ProductCommandRegistry> registry = registry_;
  return registry
             ? registry->ExecuteGuarded(invocation, origin, std::move(guard))
             : ProductCommandExecutionResult{
                   .status =
                       ProductCommandExecutionStatus::kPreconditionFailed};
}

}  // namespace limitless
