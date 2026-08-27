// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_running_profile_control_registry.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "limitless_preference_control_transport.h"

namespace limitless {

struct RunningProfileControlRegistry::ProfileEntry {
  ProfileEntry(Profile* profile,
               PreferenceControlTransport::TestingHooks testing_hooks)
      : profile(profile),
        backend(profile,
                base::BindRepeating(&ProfileEntry::NotifyValueChanged,
                                    base::Unretained(this))),
        transport(&backend, profile->GetPath().DirName()) {
    transport.SetTestingHooksForTesting(std::move(testing_hooks));
  }

  void NotifyValueChanged(const EffectivePreferenceValue& value) {
    value_changed_callbacks.Notify(value);
  }

  const raw_ptr<Profile> profile;
  ProfilePreferenceControlBackend backend;
  PreferenceControlTransport transport;
  bool ready = false;
  std::vector<raw_ptr<BrowserWindowInterface>> windows;
  base::RepeatingCallbackList<void(const EffectivePreferenceValue&)>
      value_changed_callbacks;
};

RunningProfileControlRegistry::Registration::Registration(
    base::WeakPtr<RunningProfileControlRegistry> registry,
    Profile* profile,
    BrowserWindowInterface* browser_window,
    std::string instance_identifier,
    base::RepeatingCallback<void(const EffectivePreferenceValue&)>
        value_changed)
    : registry_(registry),
      profile_(profile),
      browser_window_(browser_window),
      instance_identifier_(std::move(instance_identifier)) {
  if (value_changed) {
    value_changed_subscription_ =
        registry_->FindEntry(profile_)->value_changed_callbacks.Add(
            std::move(value_changed));
  }
  browser_did_close_subscription_ =
      browser_window_->RegisterBrowserDidClose(base::BindRepeating(
          &Registration::OnBrowserDidClose, base::Unretained(this)));
}

RunningProfileControlRegistry::Registration::~Registration() {
  Reset();
}

ProfilePreferenceControlBackend*
RunningProfileControlRegistry::Registration::backend() const {
  RunningProfileControlRegistry::ProfileEntry* entry =
      registry_ && profile_ ? registry_->FindEntry(profile_) : nullptr;
  if (!entry ||
      entry->backend.profile_instance_identifier() != instance_identifier_) {
    return nullptr;
  }
  return &entry->backend;
}

void RunningProfileControlRegistry::Registration::OnBrowserDidClose(
    BrowserWindowInterface* browser_window) {
  if (browser_window == browser_window_) {
    Reset();
  }
}

void RunningProfileControlRegistry::Registration::Reset() {
  browser_did_close_subscription_ = {};
  value_changed_subscription_ = {};
  base::WeakPtr<RunningProfileControlRegistry> registry = registry_;
  Profile* profile = profile_;
  BrowserWindowInterface* browser_window = browser_window_;
  registry_ = nullptr;
  profile_ = nullptr;
  browser_window_ = nullptr;
  if (registry && profile && browser_window) {
    registry->UnregisterWindow(profile, browser_window, instance_identifier_);
  }
  instance_identifier_.clear();
}

RunningProfileControlRegistry::RunningProfileControlRegistry() = default;
void RunningProfileControlRegistry::SetTransportTestingHooksForTesting(
    PreferenceControlTransport::TestingHooks hooks) {
  CHECK(entries_.empty());
  transport_testing_hooks_ = std::move(hooks);
}
RunningProfileControlRegistry::~RunningProfileControlRegistry() = default;

std::unique_ptr<RunningProfileControlRegistry::Registration>
RunningProfileControlRegistry::RegisterWindow(
    BrowserWindowInterface* browser_window,
    base::RepeatingCallback<void(const EffectivePreferenceValue&)>
        value_changed) {
  if (!browser_window ||
      browser_window->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      browser_window->IsDeleteScheduled()) {
    return nullptr;
  }
  Profile* profile = browser_window->GetProfile();
  if (!profile || profile->IsOffTheRecord()) {
    return nullptr;
  }

  ProfileEntry* entry = FindEntry(profile);
  if (!entry) {
    entries_.push_back(
        std::make_unique<ProfileEntry>(profile, transport_testing_hooks_));
    entry = entries_.back().get();
    ProfileEntry* started_entry = entry;
    entry->transport.StartAsync(base::BindOnce(
        &RunningProfileControlRegistry::OnTransportReady,
        weak_factory_.GetWeakPtr(), profile,
        std::string(started_entry->backend.profile_instance_identifier()),
        true));
  }
  if (std::ranges::contains(entry->windows, browser_window)) {
    return nullptr;
  }
  if (entry->transport.state() == PreferenceControlTransport::State::kStopped) {
    entry->ready = false;
    entry->transport.StartAsync(base::BindOnce(
        &RunningProfileControlRegistry::OnTransportReady,
        weak_factory_.GetWeakPtr(), profile,
        std::string(entry->backend.profile_instance_identifier()), false));
  } else if (entry->transport.state() ==
             PreferenceControlTransport::State::kStopping) {
    entry->ready = false;
    entry->transport.StartWhenStoppedAsync(base::BindOnce(
        &RunningProfileControlRegistry::OnTransportReady,
        weak_factory_.GetWeakPtr(), profile,
        std::string(entry->backend.profile_instance_identifier()), false));
  }
  entry->windows.push_back(browser_window);
  return std::unique_ptr<Registration>(new Registration(
      weak_factory_.GetWeakPtr(), profile, browser_window,
      std::string(entry->backend.profile_instance_identifier()),
      std::move(value_changed)));
}

std::vector<RunningProfileControlDescriptor>
RunningProfileControlRegistry::DiscoverProfiles() const {
  std::vector<RunningProfileControlDescriptor> profiles;
  profiles.reserve(entries_.size());
  for (const auto& entry : entries_) {
    if (!entry->ready ||
        entry->transport.state() != PreferenceControlTransport::State::kReady) {
      continue;
    }
    profiles.push_back(
        {.profile_identifier = std::string(entry->backend.profile_identifier()),
         .profile_instance_identifier =
             std::string(entry->backend.profile_instance_identifier())});
  }
  return profiles;
}

ProfilePreferenceControlBackend* RunningProfileControlRegistry::FindBackend(
    std::string_view profile_identifier) const {
  ProfileEntry* entry = FindEntry(profile_identifier);
  return entry ? &entry->backend : nullptr;
}

BrowserWindowInterface* RunningProfileControlRegistry::FindSelectedWindow(
    std::string_view profile_identifier) const {
  ProfileEntry* entry = FindEntry(profile_identifier);
  if (!entry) {
    return nullptr;
  }
  ProfileBrowserCollection* collection =
      ProfileBrowserCollection::GetForProfile(entry->profile);
  BrowserWindowInterface* selected =
      collection ? collection->FindTabbedBrowser(false) : nullptr;
  if (!selected || selected->GetProfile() != entry->profile ||
      selected->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      selected->IsDeleteScheduled() ||
      !std::ranges::contains(entry->windows, selected)) {
    return nullptr;
  }
  return selected;
}

size_t RunningProfileControlRegistry::profile_count_for_testing() const {
  return entries_.size();
}

size_t RunningProfileControlRegistry::window_count_for_testing(
    std::string_view profile_identifier) const {
  ProfileEntry* entry = FindEntry(profile_identifier);
  return entry ? entry->windows.size() : 0;
}

RunningProfileControlRegistry::ProfileEntry*
RunningProfileControlRegistry::FindEntry(Profile* profile) const {
  auto entry = std::ranges::find_if(entries_, [profile](const auto& candidate) {
    return candidate->profile == profile;
  });
  return entry == entries_.end() ? nullptr : entry->get();
}

RunningProfileControlRegistry::ProfileEntry*
RunningProfileControlRegistry::FindEntry(
    std::string_view profile_identifier) const {
  auto entry = std::ranges::find_if(entries_, [&](const auto& candidate) {
    return candidate->ready &&
           candidate->transport.state() ==
               PreferenceControlTransport::State::kReady &&
           candidate->backend.profile_identifier() == profile_identifier;
  });
  return entry == entries_.end() ? nullptr : entry->get();
}

void RunningProfileControlRegistry::UnregisterWindow(
    Profile* profile,
    BrowserWindowInterface* browser_window,
    std::string_view instance_identifier) {
  ProfileEntry* entry = FindEntry(profile);
  if (!entry ||
      entry->backend.profile_instance_identifier() != instance_identifier) {
    return;
  }
  std::erase(entry->windows, browser_window);
  if (entry->windows.empty()) {
    entry->transport.Stop();
    std::erase_if(entries_, [entry](const auto& candidate) {
      return candidate.get() == entry;
    });
  }
}

void RunningProfileControlRegistry::OnTransportReady(
    Profile* profile,
    std::string instance_identifier,
    bool initial_start,
    PreferenceControlTransportStartStatus status) {
  ProfileEntry* entry = FindEntry(profile);
  if (!entry ||
      entry->backend.profile_instance_identifier() != instance_identifier) {
    return;
  }
  if (status != PreferenceControlTransportStartStatus::kSuccess) {
    if (!initial_start) {
      entry->ready = false;
      return;
    }
    std::erase_if(entries_, [entry](const auto& candidate) {
      return candidate.get() == entry;
    });
    return;
  }
  entry->ready = true;
}

RunningProfileControlRegistry& GetRunningProfileControlRegistry() {
  static base::NoDestructor<RunningProfileControlRegistry> registry;
  return *registry;
}

}  // namespace limitless
