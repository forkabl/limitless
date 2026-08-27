// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_preference_control_protocol.h"

#include "base/check.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace limitless {

std::string SerializePreferenceRequestParseError(
    PreferenceRequestParseStatus status) {
  base::DictValue result;
  result.Set("version", kPreferenceControlSchemaVersion);
  result.Set("status", "invalid_request");
  result.Set("error", PreferenceRequestParseStatusToString(status));
  std::string serialized;
  CHECK(base::JSONWriter::Write(result, &serialized));
  return serialized;
}

}  // namespace limitless
