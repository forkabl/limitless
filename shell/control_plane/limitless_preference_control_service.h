// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_SERVICE_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/values.h"

namespace limitless {

inline constexpr int kPreferenceControlSchemaVersion = 1;
inline constexpr size_t kMaximumPreferenceRequestBytes = 16 * 1024;

enum class PreferenceActorType {
  kCli,
  kAgent,
};

enum class PreferenceOperation {
  kRead,
  kWrite,
};

enum class PreferenceKey {
  kRailDensity,
};

enum class RailDensity {
  kCompact,
  kComfortable,
};

enum class PreferenceValueSource {
  kDefault,
  kUser,
  kManaged,
};

enum class PreferenceRequestParseStatus {
  kSuccess,
  kOversized,
  kMalformedJson,
  kUnknownVersion,
  kInvalidRequestId,
  kInvalidOperation,
  kInvalidTarget,
  kInvalidActor,
  kInvalidAuthentication,
  kInvalidKey,
  kInvalidValue,
  kUnexpectedField,
};

enum class PreferenceControlStatus {
  kSuccess,
  kUnauthenticated,
  kProfileMismatch,
  kPrivateProfile,
  kStaleProfile,
  kManaged,
  kWriteFailed,
};

struct PreferenceValue {
  RailDensity rail_density = RailDensity::kCompact;

  bool operator==(const PreferenceValue&) const = default;
};

struct PreferenceTarget {
  std::string profile_identifier;
  std::string profile_instance_identifier;
};

struct PreferenceActor {
  PreferenceActorType type = PreferenceActorType::kCli;
  std::string identifier;
};

struct PreferenceRequest {
  int version = kPreferenceControlSchemaVersion;
  std::string request_identifier;
  PreferenceOperation operation = PreferenceOperation::kRead;
  PreferenceTarget target;
  PreferenceActor actor;
  std::string capability;
  PreferenceKey key = PreferenceKey::kRailDensity;
  std::optional<PreferenceValue> value;
};

struct PreferenceRequestParseResult {
  PreferenceRequestParseStatus status =
      PreferenceRequestParseStatus::kMalformedJson;
  std::optional<PreferenceRequest> request;

  bool succeeded() const {
    return status == PreferenceRequestParseStatus::kSuccess && request;
  }
};

struct EffectivePreferenceValue {
  PreferenceValue value;
  PreferenceValueSource source = PreferenceValueSource::kDefault;
};

struct PreferenceControlResult {
  int version = kPreferenceControlSchemaVersion;
  std::string request_identifier;
  PreferenceControlStatus status = PreferenceControlStatus::kWriteFailed;
  std::optional<EffectivePreferenceValue> effective_value;
  std::optional<uint64_t> audit_sequence;

  bool succeeded() const { return status == PreferenceControlStatus::kSuccess; }
};

struct PreferenceAuditRecord {
  int version = kPreferenceControlSchemaVersion;
  uint64_t sequence = 0;
  std::string request_identifier;
  PreferenceOperation operation = PreferenceOperation::kRead;
  PreferenceActor actor;
  PreferenceTarget target;
  PreferenceKey key = PreferenceKey::kRailDensity;
  std::optional<PreferenceValue> requested_value;
  PreferenceControlStatus status = PreferenceControlStatus::kWriteFailed;
  std::optional<EffectivePreferenceValue> effective_value;
};

// Product and transport adapters supply profile authority, persistence,
// authentication, policy, and audit storage. The schema service never resolves
// profiles, stores credentials, or owns browser state.
class PreferenceControlBackend {
 public:
  virtual ~PreferenceControlBackend() = default;

  virtual std::string_view profile_identifier() const = 0;
  virtual std::string_view profile_instance_identifier() const = 0;
  virtual bool is_off_the_record() const = 0;
  virtual bool Authenticate(std::string_view capability) const = 0;
  virtual bool IsManaged(PreferenceKey key) const = 0;
  virtual EffectivePreferenceValue GetEffectiveValue(
      PreferenceKey key) const = 0;
  virtual bool SetUserValue(PreferenceKey key,
                            const PreferenceValue& value) = 0;
  virtual uint64_t AppendAuditRecord(const PreferenceAuditRecord& record) = 0;
};

class PreferenceControlService {
 public:
  explicit PreferenceControlService(PreferenceControlBackend* backend);

  PreferenceControlService(const PreferenceControlService&) = delete;
  PreferenceControlService& operator=(const PreferenceControlService&) = delete;
  ~PreferenceControlService();

  PreferenceControlResult Execute(const PreferenceRequest& request);

 private:
  PreferenceControlResult Finish(const PreferenceRequest& request,
                                 PreferenceControlStatus status,
                                 bool include_effective_value);

  const raw_ptr<PreferenceControlBackend> backend_;
};

PreferenceRequestParseResult ParsePreferenceRequest(
    std::string_view serialized);
std::string SerializePreferenceControlResult(
    const PreferenceControlResult& result);
base::DictValue PreferenceAuditRecordToValue(
    const PreferenceAuditRecord& record);

std::string_view PreferenceControlStatusToString(
    PreferenceControlStatus status);
std::string_view PreferenceRequestParseStatusToString(
    PreferenceRequestParseStatus status);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_SERVICE_H_
