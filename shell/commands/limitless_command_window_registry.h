// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_REGISTRY_H_
#define LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_REGISTRY_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

struct ProductCommandWindowDiscoveryResult {
  ProductCommandWindowLookupStatus lookup_status =
      ProductCommandWindowLookupStatus::kUnavailable;
  std::vector<ProductCommandWindowDescriptor> windows;
};

struct ProductCommandWindowStateResult {
  ProductCommandWindowLookupStatus lookup_status =
      ProductCommandWindowLookupStatus::kUnavailable;
  std::optional<ProductCommandWindowState> state;
};

struct ProductCommandWindowQueryResult {
  ProductCommandWindowLookupStatus lookup_status =
      ProductCommandWindowLookupStatus::kUnavailable;
  ProductCommandQueryResult command;
};

struct ProductCommandWindowExecutionResult {
  ProductCommandWindowLookupStatus lookup_status =
      ProductCommandWindowLookupStatus::kUnavailable;
  ProductCommandExecutionResult command;
};

class ProductCommandWindowRegistry {
 public:
  class Registration {
   public:
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    ~Registration();
    void Reset();
    std::string window_instance_id() const;

   private:
    friend class ProductCommandWindowRegistry;
    Registration(base::WeakPtr<ProductCommandWindowRegistry> owner,
                 BrowserWindowInterface* browser,
                 std::string window_instance_id);
    base::WeakPtr<ProductCommandWindowRegistry> owner_;
    raw_ptr<BrowserWindowInterface> browser_ = nullptr;
    std::string window_instance_id_;
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
  ProductCommandWindowDiscoveryResult Discover() const;
  ProductCommandWindowStateResult GetState(
      const ProductCommandWindowTarget& target) const;
  ProductCommandWindowQueryResult Query(
      const ProductCommandWindowTarget& target,
      const ProductCommandInvocation& invocation) const;
  ProductCommandWindowExecutionResult ExecuteGuarded(
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
  struct EntryToken {
    raw_ptr<BrowserWindowInterface> browser;
    std::string window_instance_id;
    base::WeakPtr<ProductCommandWindowBinding> binding;
  };
  struct ResolvedEntry {
    ProductCommandWindowLookupStatus lookup_status =
        ProductCommandWindowLookupStatus::kUnavailable;
    std::optional<ProductCommandWindowState> state;
    std::optional<EntryToken> token;
  };
  std::vector<EntryToken> CopyTokens() const;
  std::optional<EntryToken> CopyTokenForInstance(
      std::string_view window_instance_id) const;
  bool IsExactRegistration(const EntryToken& token) const;
  ResolvedEntry ResolveExact(const ProductCommandWindowTarget& target) const;
  void Unregister(BrowserWindowInterface* browser, std::string instance_id);
  raw_ptr<Profile> profile_;
  std::vector<Entry> entries_;
  uint64_t mutation_generation_ = 0;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ProductCommandWindowRegistry> weak_factory_{this};
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_COMMANDS_LIMITLESS_COMMAND_WINDOW_REGISTRY_H_
