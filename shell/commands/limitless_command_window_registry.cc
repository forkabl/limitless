// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_window_registry.h"

#include <set>
#include <type_traits>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"

namespace limitless {

namespace {
bool IsCanonicalUuid(const std::string& value) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(value);
  return uuid.is_valid() && uuid.AsLowercaseString() == value;
}

std::optional<std::string_view> EmbeddedWindowIdentifier(
    const ProductCommandInvocation& invocation) {
  return std::visit(
      [](const auto& arguments) -> std::optional<std::string_view> {
        using T = std::decay_t<decltype(arguments)>;
        if constexpr (std::is_same_v<T, ProductCommandSplitCreate> ||
                      std::is_same_v<T, ProductCommandSplitFocus> ||
                      std::is_same_v<T, ProductCommandSetSplitLayout> ||
                      std::is_same_v<T, ProductCommandSetSplitRatio>) {
          return arguments.pair.window_identifier;
        } else if constexpr (std::is_same_v<T, ProductCommandSplitPair>) {
          return arguments.window_identifier;
        } else {
          return std::nullopt;
        }
      },
      invocation.arguments);
}

bool MatchesWindowTarget(const ProductCommandWindowTarget& target,
                         const ProductCommandInvocation& invocation) {
  const auto identifier = EmbeddedWindowIdentifier(invocation);
  return !identifier || *identifier == target.window_id;
}
}  // namespace

ProductCommandWindowRegistry::Registration::Registration(
    base::WeakPtr<ProductCommandWindowRegistry> owner,
    BrowserWindowInterface* browser,
    std::string instance)
    : owner_(owner),
      browser_(browser),
      window_instance_id_(std::move(instance)) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  close_subscription_ = browser_->RegisterBrowserDidClose(base::BindRepeating(
      [](Registration* registration, BrowserWindowInterface* browser) {
        if (registration->browser_ == browser) {
          registration->Reset();
        }
      },
      base::Unretained(this)));
}
ProductCommandWindowRegistry::Registration::~Registration() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Reset();
}
std::string ProductCommandWindowRegistry::Registration::window_instance_id()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return window_instance_id_;
}
void ProductCommandWindowRegistry::Registration::Reset() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  close_subscription_ = {};
  auto owner = owner_;
  auto browser = browser_;
  auto instance = std::move(window_instance_id_);
  owner_ = nullptr;
  browser_ = nullptr;
  if (owner && browser) {
    owner->Unregister(browser, std::move(instance));
  }
}
ProductCommandWindowRegistry::ProductCommandWindowRegistry(Profile* profile)
    : profile_(profile) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(profile_);
}
ProductCommandWindowRegistry::~ProductCommandWindowRegistry() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::unique_ptr<ProductCommandWindowRegistry::Registration>
ProductCommandWindowRegistry::Register(
    BrowserWindowInterface* browser,
    base::WeakPtr<ProductCommandWindowBinding> binding) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!browser || !binding || browser->GetProfile() != profile_ ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      browser->IsDeleteScheduled()) {
    return nullptr;
  }
  if (browser->GetProfile()->IsOffTheRecord()) {
    return nullptr;
  }
  for (const Entry& entry : entries_) {
    if (entry.browser == browser) {
      return nullptr;
    }
  }
  std::string instance = base::Uuid::GenerateRandomV4().AsLowercaseString();
  entries_.push_back({browser, instance, binding});
  ++mutation_generation_;
  return std::unique_ptr<Registration>(
      new Registration(weak_factory_.GetWeakPtr(), browser, instance));
}

void ProductCommandWindowRegistry::Unregister(BrowserWindowInterface* browser,
                                              std::string instance) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const size_t erased =
      std::erase_if(entries_, [browser, &instance](const Entry& entry) {
        return entry.browser == browser && entry.window_instance_id == instance;
      });
  if (erased) {
    ++mutation_generation_;
  }
}

std::vector<ProductCommandWindowRegistry::EntryToken>
ProductCommandWindowRegistry::CopyTokens() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<EntryToken> tokens;
  for (const Entry& entry : entries_) {
    tokens.push_back({entry.browser, entry.window_instance_id, entry.binding});
  }
  return tokens;
}

std::optional<ProductCommandWindowRegistry::EntryToken>
ProductCommandWindowRegistry::CopyTokenForInstance(
    std::string_view instance) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const Entry& entry : entries_) {
    if (entry.window_instance_id == instance) {
      return EntryToken{entry.browser, entry.window_instance_id, entry.binding};
    }
  }
  return std::nullopt;
}

bool ProductCommandWindowRegistry::IsExactRegistration(
    const EntryToken& token) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const Entry& entry : entries_) {
    if (entry.browser == token.browser &&
        entry.window_instance_id == token.window_instance_id &&
        entry.binding.get() == token.binding.get()) {
      return true;
    }
  }
  return false;
}

ProductCommandWindowRegistry::ResolvedEntry
ProductCommandWindowRegistry::ResolveExact(
    const ProductCommandWindowTarget& target) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ResolvedEntry result;
  if (!IsCanonicalUuid(target.window_id) ||
      !IsCanonicalUuid(target.window_instance_id)) {
    result.lookup_status = ProductCommandWindowLookupStatus::kInvalidTarget;
    return result;
  }
  auto token = CopyTokenForInstance(target.window_instance_id);
  if (!token || !token->binding || !IsExactRegistration(*token)) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  base::WeakPtr<const ProductCommandWindowRegistry> self =
      weak_factory_.GetWeakPtr();
  auto state = token->binding->GetState();
  if (!self) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  if (!token->binding || !IsExactRegistration(*token)) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  if (!state) {
    result.lookup_status = ProductCommandWindowLookupStatus::kUnavailable;
    return result;
  }
  if (state->window_id != target.window_id) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  const uint64_t generation = self->mutation_generation_;
  const std::vector<EntryToken> tokens = self->CopyTokens();
  for (const EntryToken& other : tokens) {
    if (other.browser == token->browser &&
        other.window_instance_id == token->window_instance_id) {
      continue;
    }
    if (!other.binding || !IsExactRegistration(other)) {
      continue;
    }
    auto other_state = other.binding->GetState();
    if (!self) {
      result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
      return result;
    }
    if (self->mutation_generation_ != generation) {
      result.lookup_status =
          token->binding && self->IsExactRegistration(*token)
              ? ProductCommandWindowLookupStatus::kUnavailable
              : ProductCommandWindowLookupStatus::kStaleTarget;
      return result;
    }
    if (!token->binding || !self->IsExactRegistration(*token)) {
      result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
      return result;
    }
    if (!other.binding || !self->IsExactRegistration(other)) {
      continue;
    }
    if (other_state && other_state->window_id == target.window_id) {
      result.lookup_status = ProductCommandWindowLookupStatus::kAmbiguous;
      return result;
    }
  }
  base::WeakPtr<const ProductCommandWindowRegistry> final_self = self;
  auto final_state = token->binding->GetState();
  if (!final_self) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  if (final_self->mutation_generation_ != generation || !token->binding ||
      !final_self->IsExactRegistration(*token)) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  if (!final_state) {
    result.lookup_status = ProductCommandWindowLookupStatus::kUnavailable;
    return result;
  }
  if (final_state->window_id != target.window_id) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  if (!token->binding || !self->IsExactRegistration(*token)) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  result.lookup_status = ProductCommandWindowLookupStatus::kFound;
  result.state = std::move(final_state);
  result.token = std::move(token);
  return result;
}

ProductCommandWindowDiscoveryResult ProductCommandWindowRegistry::Discover()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ProductCommandWindowDiscoveryResult result;
  base::WeakPtr<const ProductCommandWindowRegistry> self =
      weak_factory_.GetWeakPtr();
  const uint64_t generation = mutation_generation_;
  const std::vector<EntryToken> tokens = CopyTokens();
  std::set<std::string> ids;
  for (const EntryToken& token : tokens) {
    if (!token.binding || !IsExactRegistration(token)) {
      continue;
    }
    auto state = token.binding->GetState();
    if (!self) {
      return result;
    }
    if (mutation_generation_ != generation) {
      return result;
    }
    if (!token.binding || !IsExactRegistration(token) || !state) {
      continue;
    }
    if (!ids.insert(state->window_id).second) {
      result.lookup_status = ProductCommandWindowLookupStatus::kAmbiguous;
      result.windows.clear();
      return result;
    }
    result.windows.push_back(
        {{state->window_id, token.window_instance_id}, std::move(*state)});
  }
  if (mutation_generation_ != generation) {
    result.windows.clear();
    return result;
  }
  result.lookup_status = ProductCommandWindowLookupStatus::kFound;
  return result;
}

ProductCommandWindowStateResult ProductCommandWindowRegistry::GetState(
    const ProductCommandWindowTarget& target) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const ResolvedEntry resolved = ResolveExact(target);
  ProductCommandWindowStateResult result;
  result.lookup_status = resolved.lookup_status;
  if (resolved.lookup_status == ProductCommandWindowLookupStatus::kFound &&
      resolved.state) {
    result.state = resolved.state;
  } else if (result.lookup_status == ProductCommandWindowLookupStatus::kFound) {
    result.lookup_status = ProductCommandWindowLookupStatus::kUnavailable;
  }
  return result;
}

ProductCommandWindowQueryResult ProductCommandWindowRegistry::Query(
    const ProductCommandWindowTarget& target,
    const ProductCommandInvocation& invocation) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ProductCommandWindowQueryResult result;
  result.command.status = ProductCommandQueryStatus::kDisabled;
  const ResolvedEntry before = ResolveExact(target);
  result.lookup_status = before.lookup_status;
  if (before.lookup_status != ProductCommandWindowLookupStatus::kFound ||
      !before.token || !before.token->binding) {
    return result;
  }
  const ProductCommandValidationStatus validation =
      ValidateProductCommandInvocation(invocation);
  if (validation != ProductCommandValidationStatus::kValid) {
    result.command.status = ProductCommandQueryStatus::kInvalidInvocation;
    result.command.validation = validation;
    return result;
  }
  if (!MatchesWindowTarget(target, invocation)) {
    result.command.status = ProductCommandQueryStatus::kInvalidInvocation;
    result.command.validation =
        ProductCommandValidationStatus::kWindowTargetMismatch;
    return result;
  }
  const EntryToken token = *before.token;
  const auto binding = token.binding;
  base::WeakPtr<const ProductCommandWindowRegistry> self =
      weak_factory_.GetWeakPtr();
  const ProductCommandQueryResult command = binding->Query(invocation);
  if (!self) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  const ResolvedEntry after = self->ResolveExact(target);
  result.lookup_status = after.lookup_status;
  if (after.lookup_status == ProductCommandWindowLookupStatus::kFound &&
      after.token && after.token->browser == token.browser &&
      after.token->window_instance_id == token.window_instance_id &&
      after.token->binding.get() == binding.get()) {
    result.command = command;
  }
  return result;
}

ProductCommandWindowExecutionResult
ProductCommandWindowRegistry::ExecuteGuarded(
    const ProductCommandWindowTarget& target,
    const ProductCommandInvocation& invocation,
    ProductCommandOrigin origin,
    ProductCommandRegistry::PreExecuteGuard guard) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(guard);
  ProductCommandWindowExecutionResult result;
  result.command.status = ProductCommandExecutionStatus::kPreconditionFailed;
  ResolvedEntry before = ResolveExact(target);
  result.lookup_status = before.lookup_status;
  if (before.lookup_status != ProductCommandWindowLookupStatus::kFound ||
      !before.token || !before.token->binding) {
    return result;
  }
  const ProductCommandValidationStatus validation =
      ValidateProductCommandInvocation(invocation);
  if (validation != ProductCommandValidationStatus::kValid) {
    result.command.status = ProductCommandExecutionStatus::kInvalidInvocation;
    result.command.validation = validation;
    return result;
  }
  if (!MatchesWindowTarget(target, invocation)) {
    result.command.status = ProductCommandExecutionStatus::kInvalidInvocation;
    result.command.validation =
        ProductCommandValidationStatus::kWindowTargetMismatch;
    return result;
  }
  const EntryToken token = *before.token;
  const auto binding = token.binding;
  const base::WeakPtr<ProductCommandWindowRegistry> weak =
      weak_factory_.GetWeakPtr();
  auto wrapped_guard = base::BindOnce(
      [](base::WeakPtr<ProductCommandWindowRegistry> weak,
         ProductCommandWindowTarget target, EntryToken expected,
         ProductCommandRegistry::PreExecuteGuard guard,
         const ProductCommandInvocation& invocation) {
        if (!weak) {
          return ProductCommandPreExecuteDecision::kPreconditionFailed;
        }
        ResolvedEntry before = weak->ResolveExact(target);
        if (before.lookup_status != ProductCommandWindowLookupStatus::kFound ||
            !before.token || before.token->browser != expected.browser ||
            before.token->window_instance_id != expected.window_instance_id ||
            before.token->binding.get() != expected.binding.get()) {
          return ProductCommandPreExecuteDecision::kPreconditionFailed;
        }
        ProductCommandPreExecuteDecision decision =
            std::move(guard).Run(invocation);
        if (decision != ProductCommandPreExecuteDecision::kAllow) {
          return decision;
        }
        if (!weak) {
          return ProductCommandPreExecuteDecision::kPreconditionFailed;
        }
        ResolvedEntry after = weak->ResolveExact(target);
        if (after.lookup_status != ProductCommandWindowLookupStatus::kFound ||
            !after.token || after.token->browser != expected.browser ||
            after.token->window_instance_id != expected.window_instance_id ||
            after.token->binding.get() != expected.binding.get()) {
          return ProductCommandPreExecuteDecision::kPreconditionFailed;
        }
        return ProductCommandPreExecuteDecision::kAllow;
      },
      weak, target, token, std::move(guard));
  result.command =
      binding->ExecuteGuarded(invocation, origin, std::move(wrapped_guard));
  if (!weak) {
    result.lookup_status = ProductCommandWindowLookupStatus::kStaleTarget;
    return result;
  }
  result.lookup_status = weak->ResolveExact(target).lookup_status;
  return result;
}

}  // namespace limitless
