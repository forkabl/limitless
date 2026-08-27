// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_BINDING_H_
#define LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_BINDING_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "shell/commands/limitless_command_registry.h"

namespace limitless {

struct ProductCommandSplitState {
  std::string split_instance_id;
  std::string first_tab_id;
  std::string second_tab_id;
  std::optional<std::string> active_tab_id;
  ProductCommandSplitLayout layout = ProductCommandSplitLayout::kSideBySide;
  double ratio = 0.5;
};

struct ProductCommandWindowState {
  // Report-only live metadata. is_active is never selection authority;
  // ephemeral window instance identity is registry-owned and excluded here.
  std::string window_id;
  std::optional<std::string> active_workspace_id;
  std::vector<std::string> visible_tab_ids;
  std::optional<std::string> active_tab_id;
  std::vector<ProductCommandSplitState> splits;
  bool is_active = false;
};

class ProductCommandWindowBinding {
 public:
  using StateCallback =
      base::RepeatingCallback<std::optional<ProductCommandWindowState>()>;
  ProductCommandWindowBinding(base::WeakPtr<ProductCommandRegistry> registry,
                              StateCallback state);
  ProductCommandWindowBinding(const ProductCommandWindowBinding&) = delete;
  ProductCommandWindowBinding& operator=(const ProductCommandWindowBinding&) =
      delete;
  ~ProductCommandWindowBinding();
  base::WeakPtr<ProductCommandWindowBinding> GetWeakPtr();
  std::optional<ProductCommandWindowState> GetState() const;
  ProductCommandQueryResult Query(const ProductCommandInvocation&) const;
  ProductCommandExecutionResult ExecuteGuarded(
      const ProductCommandInvocation&,
      ProductCommandOrigin,
      ProductCommandRegistry::PreExecuteGuard) const;

 private:
  StateCallback state_;
  base::WeakPtr<ProductCommandRegistry> registry_;
  base::WeakPtrFactory<ProductCommandWindowBinding> weak_factory_{this};
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_BINDING_H_
