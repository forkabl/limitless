// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_window_binding.h"

#include <utility>

#include "base/check.h"

namespace limitless {

ProductCommandWindowBinding::ProductCommandWindowBinding(
    StateCallback state,
    ProductCommandRegistry::QueryCallback query,
    ProductCommandRegistry::ExecuteCallback execute)
    : state_(std::move(state)),
      registry_(std::move(query), std::move(execute)) {
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
  return registry_.Query(invocation);
}

ProductCommandExecutionResult ProductCommandWindowBinding::ExecuteGuarded(
    const ProductCommandInvocation& invocation,
    ProductCommandOrigin origin,
    ProductCommandRegistry::PreExecuteGuard guard) const {
  return registry_.ExecuteGuarded(invocation, origin, std::move(guard));
}

}  // namespace limitless
