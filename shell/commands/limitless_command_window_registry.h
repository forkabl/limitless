// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_REGISTRY_H_
#define LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_REGISTRY_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "shell/commands/limitless_command_registry.h"
#include "shell/commands/limitless_command_window_binding.h"

class BrowserWindowInterface;
class Profile;

namespace limitless {

enum class ProductCommandWindowLookupStatus {
  kInvalidTarget,
  kStaleTarget,
  kUnavailable,
  kAmbiguous,
  kFound,
};

struct ProductCommandWindowTarget {
  std::string window_id;
  std::string window_instance_id;
};

struct ProductCommandWindowDescriptor {
  ProductCommandWindowTarget target;
  ProductCommandWindowState state;
};

class ProductCommandWindowRegistry {
 public:
  class Registration {
   public:
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    ~Registration();
    void Reset();
    std::string window_instance_id() const { return window_instance_id_; }

   private:
    friend class ProductCommandWindowRegistry;
    Registration(base::WeakPtr<ProductCommandWindowRegistry> owner,
                 BrowserWindowInterface* browser,
                 std::string window_instance_id,
                 base::WeakPtr<ProductCommandWindowBinding> binding);
    base::WeakPtr<ProductCommandWindowRegistry> owner_;
    raw_ptr<BrowserWindowInterface> browser_ = nullptr;
    std::string window_instance_id_;
    base::WeakPtr<ProductCommandWindowBinding> binding_;
    base::CallbackListSubscription close_subscription_;
    SEQUENCE_CHECKER(sequence_checker_);
  };

  explicit ProductCommandWindowRegistry(Profile* profile);
  ProductCommandWindowRegistry(const ProductCommandWindowRegistry&) = delete;
  ProductCommandWindowRegistry& operator=(const ProductCommandWindowRegistry&) =
      delete;
  ~ProductCommandWindowRegistry();

  std::unique_ptr<Registration> Register(
      BrowserWindowInterface* browser,
      base::WeakPtr<ProductCommandWindowBinding> binding);
  std::vector<ProductCommandWindowDescriptor> Discover() const;
  ProductCommandWindowLookupStatus GetState(
      const ProductCommandWindowTarget& target,
      ProductCommandWindowState* state) const;
  ProductCommandQueryResult Query(
      const ProductCommandWindowTarget& target,
      const ProductCommandInvocation& invocation) const;
  ProductCommandExecutionResult ExecuteGuarded(
      const ProductCommandWindowTarget& target,
      const ProductCommandInvocation& invocation,
      ProductCommandOrigin origin,
      ProductCommandRegistry::PreExecuteGuard guard);

 private:
  struct Entry {
    raw_ptr<BrowserWindowInterface> browser;
    std::string window_instance_id;
    base::WeakPtr<ProductCommandWindowBinding> binding;
  };
  void Unregister(BrowserWindowInterface* browser, std::string instance_id);
  Entry* Find(const ProductCommandWindowTarget& target) const;
  raw_ptr<Profile> profile_;
  std::vector<Entry> entries_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ProductCommandWindowRegistry> weak_factory_{this};
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_REGISTRY_H_
