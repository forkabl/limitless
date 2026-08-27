// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_profile_preference_backend.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "crypto/secure_util.h"

namespace limitless {
namespace {
constexpr size_t kMaximumPersistedAuditRecords = 256;

constexpr char kCompactValue[] = "compact";
constexpr char kComfortableValue[] = "comfortable";

std::string GenerateIdentifier() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

bool IsCanonicalIdentifier(std::string_view candidate) {
  const base::Uuid identifier = base::Uuid::ParseLowercase(candidate);
  return identifier.is_valid() && identifier.AsLowercaseString() == candidate;
}

std::string GetOrCreateProfileIdentifier(Profile* profile) {
  if (profile->IsOffTheRecord()) {
    return GenerateIdentifier();
  }

  PrefService* prefs = profile->GetPrefs();
  const base::Value* stored =
      prefs->GetUserPrefValue(kControlProfileIdentifierPref);
  if (stored && stored->is_string() &&
      IsCanonicalIdentifier(stored->GetString())) {
    return stored->GetString();
  }

  std::string identifier = GenerateIdentifier();
  prefs->SetString(kControlProfileIdentifierPref, identifier);
  return identifier;
}

RailDensity RailDensityFromString(std::string_view value) {
  return value == kComfortableValue ? RailDensity::kComfortable
                                    : RailDensity::kCompact;
}

std::string_view RailDensityToString(RailDensity density) {
  switch (density) {
    case RailDensity::kCompact:
      return kCompactValue;
    case RailDensity::kComfortable:
      return kComfortableValue;
  }
}

}  // namespace

ProfilePreferenceControlBackend::ProfilePreferenceControlBackend(
    Profile* profile,
    ValueChangedCallback value_changed)
    : profile_(profile),
      prefs_(profile ? profile->GetPrefs() : nullptr),
      is_off_the_record_(profile && profile->IsOffTheRecord()),
      profile_identifier_(profile ? GetOrCreateProfileIdentifier(profile)
                                  : std::string()),
      profile_instance_identifier_(GenerateIdentifier()),
      capability_(GenerateIdentifier()),
      value_changed_(std::move(value_changed)) {
  CHECK(profile_);
  CHECK(prefs_);
  if (!is_off_the_record_) {
    pref_change_registrar_.Init(prefs_);
    pref_change_registrar_.Add(
        kRailDensityPref,
        base::BindRepeating(
            &ProfilePreferenceControlBackend::OnRailDensityChanged,
            base::Unretained(this)));
  }
}

ProfilePreferenceControlBackend::~ProfilePreferenceControlBackend() = default;

std::string_view ProfilePreferenceControlBackend::profile_identifier() const {
  return profile_identifier_;
}

std::string_view ProfilePreferenceControlBackend::profile_instance_identifier()
    const {
  return profile_instance_identifier_;
}

bool ProfilePreferenceControlBackend::is_off_the_record() const {
  return is_off_the_record_;
}

bool ProfilePreferenceControlBackend::Authenticate(
    std::string_view capability) const {
  return crypto::SecureMemEqual(base::as_byte_span(capability),
                                base::as_byte_span(capability_));
}

bool ProfilePreferenceControlBackend::IsManaged(PreferenceKey key) const {
  switch (key) {
    case PreferenceKey::kRailDensity:
      return prefs_->IsManagedPreference(kRailDensityPref);
  }
}

EffectivePreferenceValue ProfilePreferenceControlBackend::GetEffectiveValue(
    PreferenceKey key) const {
  switch (key) {
    case PreferenceKey::kRailDensity: {
      const PrefService::Preference* preference =
          prefs_->FindPreference(kRailDensityPref);
      CHECK(preference);
      PreferenceValueSource source = PreferenceValueSource::kDefault;
      if (preference->IsManaged()) {
        source = PreferenceValueSource::kManaged;
      } else if (preference->HasUserSetting()) {
        source = PreferenceValueSource::kUser;
      }
      return {
          .value = {.rail_density = RailDensityFromString(
                        prefs_->GetString(kRailDensityPref))},
          .source = source,
      };
    }
  }
}

bool ProfilePreferenceControlBackend::SetUserValue(
    PreferenceKey key,
    const PreferenceValue& value) {
  if (is_off_the_record_) {
    return false;
  }
  switch (key) {
    case PreferenceKey::kRailDensity:
      if (!prefs_->IsUserModifiablePreference(kRailDensityPref)) {
        return false;
      }
      prefs_->SetString(kRailDensityPref,
                        RailDensityToString(value.rail_density));
      return RailDensityFromString(prefs_->GetString(kRailDensityPref)) ==
             value.rail_density;
  }
}

uint64_t ProfilePreferenceControlBackend::AppendAuditRecord(
    const PreferenceAuditRecord& record) {
  if (is_off_the_record_) {
    return ++private_audit_sequence_;
  }

  int64_t sequence = prefs_->GetInt64(kPreferenceAuditSequencePref);
  if (sequence < 0 || sequence == std::numeric_limits<int64_t>::max()) {
    sequence = 0;
  }
  ++sequence;

  PreferenceAuditRecord stored_record = record;
  stored_record.sequence = static_cast<uint64_t>(sequence);
  {
    ScopedListPrefUpdate audit(prefs_, kPreferenceAuditRecordsPref);
    audit->Append(PreferenceAuditRecordToValue(stored_record));
    while (audit->size() > kMaximumPersistedAuditRecords)
      audit->Erase(audit->begin());
  }
  prefs_->SetInt64(kPreferenceAuditSequencePref, sequence);
  return static_cast<uint64_t>(sequence);
}

void ProfilePreferenceControlBackend::OnRailDensityChanged() {
  if (value_changed_) {
    value_changed_.Run(GetEffectiveValue(PreferenceKey::kRailDensity));
  }
}

}  // namespace limitless
