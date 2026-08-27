// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_RUNNING_PROFILE_CONTROL_REGISTRY_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_RUNNING_PROFILE_CONTROL_REGISTRY_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "limitless_profile_preference_backend.h"

class BrowserWindowInterface;
class Profile;

namespace limitless {

struct RunningProfileControlDescriptor {
  std::string profile_identifier;
  std::string profile_instance_identifier;
};

// Owns one control backend for each regular profile with an eligible live
// browser window. It is product-owned and does not duplicate browser state:
// the selected window is resolved from Chromium immediately before use.
class RunningProfileControlRegistry {
 public:
  class Registration {
   public:
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    ~Registration();

    ProfilePreferenceControlBackend* backend() const;
    BrowserWindowInterface* browser_window() const { return browser_window_; }

   private:
    friend class RunningProfileControlRegistry;

    Registration(RunningProfileControlRegistry* registry,
                 Profile* profile,
                 BrowserWindowInterface* browser_window,
                 base::RepeatingCallback<void(
                     const EffectivePreferenceValue&)> value_changed);
    void OnBrowserDidClose(BrowserWindowInterface* browser_window);
    void Reset();

    raw_ptr<RunningProfileControlRegistry> registry_;
    raw_ptr<Profile> profile_;
    raw_ptr<BrowserWindowInterface> browser_window_;
    base::CallbackListSubscription value_changed_subscription_;
    base::CallbackListSubscription browser_did_close_subscription_;
  };

  RunningProfileControlRegistry();
  RunningProfileControlRegistry(const RunningProfileControlRegistry&) =
      delete;
  RunningProfileControlRegistry& operator=(
      const RunningProfileControlRegistry&) = delete;
  ~RunningProfileControlRegistry();

  // Returns null for incognito, non-normal, closing, or duplicate windows.
  std::unique_ptr<Registration> RegisterWindow(
      BrowserWindowInterface* browser_window,
      base::RepeatingCallback<void(const EffectivePreferenceValue&)>
          value_changed = {});

  std::vector<RunningProfileControlDescriptor> DiscoverProfiles() const;
  ProfilePreferenceControlBackend* FindBackend(
      std::string_view profile_identifier) const;
  BrowserWindowInterface* FindSelectedWindow(
      std::string_view profile_identifier) const;

  size_t profile_count_for_testing() const;
  size_t window_count_for_testing(std::string_view profile_identifier) const;

 private:
  struct ProfileEntry;

  ProfileEntry* FindEntry(Profile* profile) const;
  ProfileEntry* FindEntry(std::string_view profile_identifier) const;
  void UnregisterWindow(Profile* profile,
                        BrowserWindowInterface* browser_window);

  std::vector<std::unique_ptr<ProfileEntry>> entries_;
};

RunningProfileControlRegistry& GetRunningProfileControlRegistry();

}  // namespace limitless

#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_RUNNING_PROFILE_CONTROL_REGISTRY_H_
