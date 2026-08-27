// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_window_binding.h"

#include <utility>

#include "base/check.h"

namespace limitless {

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
  return state.Run();
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
