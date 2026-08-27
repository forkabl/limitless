// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_profile_prefs.h"

#include "components/pref_registry/pref_registry_syncable.h"

namespace limitless {

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterStringPref(kRailDensityPref, kCompactRailDensity);
  registry->RegisterStringPref(kControlProfileIdentifierPref, std::string());
  registry->RegisterListPref(kPreferenceAuditRecordsPref);
  registry->RegisterInt64Pref(kPreferenceAuditSequencePref, 0);
}

}  // namespace limitless
