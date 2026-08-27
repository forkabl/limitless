// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_PROTOCOL_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_PROTOCOL_H_

#include <cstddef>
#include <string>

#include "limitless_preference_control_service.h"

namespace limitless {

inline constexpr size_t kMaximumPreferenceTransportResponseBytes = 32 * 1024;

std::string SerializePreferenceRequestParseError(
    PreferenceRequestParseStatus status);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_PROTOCOL_H_
