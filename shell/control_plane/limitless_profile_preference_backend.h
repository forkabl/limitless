// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PROFILE_PREFERENCE_BACKEND_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PROFILE_PREFERENCE_BACKEND_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "limitless_preference_control_service.h"
#include "limitless_profile_prefs.h"

class Profile;
class PrefService;

namespace limitless {

// A running-profile adapter over Chromium's PrefService. It owns no browser,
// tab, page, or window state. The profile ID persists, while the instance ID
// and capability are regenerated with every adapter lifetime so stale or
// replayed requests fail closed.
class ProfilePreferenceControlBackend final : public PreferenceControlBackend {
 public:
  using ValueChangedCallback =
      base::RepeatingCallback<void(const EffectivePreferenceValue&)>;

  explicit ProfilePreferenceControlBackend(
      Profile* profile,
      ValueChangedCallback value_changed = ValueChangedCallback());

  ProfilePreferenceControlBackend(const ProfilePreferenceControlBackend&) =
      delete;
  ProfilePreferenceControlBackend& operator=(
      const ProfilePreferenceControlBackend&) = delete;
  ~ProfilePreferenceControlBackend() override;

  std::string_view profile_identifier() const override;
  std::string_view profile_instance_identifier() const override;
  bool is_off_the_record() const override;
  bool Authenticate(std::string_view capability) const override;
  bool IsManaged(PreferenceKey key) const override;
  EffectivePreferenceValue GetEffectiveValue(PreferenceKey key) const override;
  bool SetUserValue(PreferenceKey key, const PreferenceValue& value) override;
  uint64_t AppendAuditRecord(const PreferenceAuditRecord& record) override;

  // The transport may disclose these only over its authenticated local
  // channel. They are exposed here so that transport ownership remains
  // separate from profile persistence and SwiftUI projection.
  std::string_view capability() const { return capability_; }

 private:
  void OnRailDensityChanged();

  const raw_ptr<Profile> profile_;
  const raw_ptr<PrefService> prefs_;
  const bool is_off_the_record_;
  const std::string profile_identifier_;
  const std::string profile_instance_identifier_;
  const std::string capability_;
  const ValueChangedCallback value_changed_;
  PrefChangeRegistrar pref_change_registrar_;
  uint64_t private_audit_sequence_ = 0;
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PROFILE_PREFERENCE_BACKEND_H_
