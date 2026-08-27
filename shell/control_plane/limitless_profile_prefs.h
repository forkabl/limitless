// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PROFILE_PREFS_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PROFILE_PREFS_H_

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace limitless {

inline constexpr char kRailDensityPref[] = "limitless.rail_density";
inline constexpr char kCompactRailDensity[] = "compact";
inline constexpr char kComfortableRailDensity[] = "comfortable";
inline constexpr char kControlProfileIdentifierPref[] =
    "limitless.control.profile_identifier";
inline constexpr char kPreferenceAuditRecordsPref[] =
    "limitless.control.preference_audit_records";
inline constexpr char kPreferenceAuditSequencePref[] =
    "limitless.control.preference_audit_sequence";

// Registers product-owned profile preferences. Preferences registered here
// stay local to the profile unless an explicit sync registration flag is added.
void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PROFILE_PREFS_H_
