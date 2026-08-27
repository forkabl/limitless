// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/commands/limitless_command_window_registry.h"

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
}  // namespace

ProductCommandWindowRegistry::Registration::Registration(
    base::WeakPtr<ProductCommandWindowRegistry> owner,
    BrowserWindowInterface* browser,
    std::string instance,
    base::WeakPtr<ProductCommandWindowBinding> binding)
    : owner_(owner),
      browser_(browser),
      window_instance_id_(std::move(instance)),
      binding_(binding) {
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
void ProductCommandWindowRegistry::Registration::Reset() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  close_subscription_ = {};
  auto owner = owner_;
  auto browser = browser_;
  auto instance = std::move(window_instance_id_);
  owner_ = nullptr;
  browser_ = nullptr;
  binding_ = nullptr;
  if (owner && browser) {
    owner->Unregister(browser, std::move(instance));
  }
}
ProductCommandWindowRegistry::ProductCommandWindowRegistry(Profile* profile)
    : profile_(profile) {
  CHECK(profile_);
}
ProductCommandWindowRegistry::~ProductCommandWindowRegistry() = default;

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
  for (const Entry& entry : entries_) {
    if (entry.browser == browser) {
      return nullptr;
    }
  }
  std::string instance = base::Uuid::GenerateRandomV4().AsLowercaseString();
  entries_.push_back({browser, instance, binding});
  return std::unique_ptr<Registration>(
      new Registration(weak_factory_.GetWeakPtr(), browser, instance, binding));
}

std::vector<ProductCommandWindowDescriptor>
ProductCommandWindowRegistry::Discover() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<ProductCommandWindowDescriptor> result;
  for (const Entry& entry : entries_) {
    if (auto state = entry.binding ? entry.binding->GetState() : std::nullopt) {
      result.push_back({{state->window_id, entry.window_instance_id}, *state});
    }
  }
  return result;
}

void ProductCommandWindowRegistry::Unregister(BrowserWindowInterface* browser,
                                              std::string instance) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::erase_if(entries_, [browser, &instance](const Entry& entry) {
    return entry.browser == browser && entry.window_instance_id == instance;
  });
}

ProductCommandWindowRegistry::Entry* ProductCommandWindowRegistry::Find(
    const ProductCommandWindowTarget& target) const {
  for (const Entry& entry : entries_) {
    if (entry.window_instance_id == target.window_instance_id) {
      return const_cast<Entry*>(&entry);
    }
  }
  return nullptr;
}

ProductCommandWindowLookupStatus ProductCommandWindowRegistry::GetState(
    const ProductCommandWindowTarget& target,
    ProductCommandWindowState* state) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsCanonicalUuid(target.window_id) ||
      !IsCanonicalUuid(target.window_instance_id)) {
    return ProductCommandWindowLookupStatus::kInvalidTarget;
  }
  Entry* entry = Find(target);
  if (!entry || !entry->binding) {
    return ProductCommandWindowLookupStatus::kStaleTarget;
  }
  auto value = entry->binding->GetState();
  if (!value || value->window_id != target.window_id) {
    return ProductCommandWindowLookupStatus::kUnavailable;
  }
  if (state) {
    *state = std::move(*value);
  }
  return ProductCommandWindowLookupStatus::kFound;
}

ProductCommandQueryResult ProductCommandWindowRegistry::Query(
    const ProductCommandWindowTarget& target,
    const ProductCommandInvocation& invocation) const {
  Entry* entry = Find(target);
  return entry && entry->binding
             ? entry->binding->Query(invocation)
             : ProductCommandQueryResult{
                   .status = ProductCommandQueryStatus::kDisabled};
}

ProductCommandExecutionResult ProductCommandWindowRegistry::ExecuteGuarded(
    const ProductCommandWindowTarget& target,
    const ProductCommandInvocation& invocation,
    ProductCommandOrigin origin,
    ProductCommandRegistry::PreExecuteGuard guard) {
  Entry* entry = Find(target);
  return entry && entry->binding
             ? entry->binding->ExecuteGuarded(invocation, origin,
                                              std::move(guard))
             : ProductCommandExecutionResult{
                   .status =
                       ProductCommandExecutionStatus::kPreconditionFailed};
}

}  // namespace limitless
