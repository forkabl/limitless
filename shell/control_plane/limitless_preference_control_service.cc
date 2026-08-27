// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_preference_control_service.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"

namespace limitless {
namespace {

constexpr size_t kMaximumActorIdentifierBytes = 128;
constexpr size_t kMaximumCapabilityBytes = 256;

bool HasExactFields(const base::DictValue& dict,
                    std::initializer_list<std::string_view> fields) {
  if (dict.size() != fields.size()) {
    return false;
  }
  return std::ranges::all_of(
      fields, [&dict](std::string_view field) { return dict.contains(field); });
}

bool IsCanonicalIdentifier(std::string_view candidate) {
  const base::Uuid identifier = base::Uuid::ParseLowercase(candidate);
  return identifier.is_valid() && identifier.AsLowercaseString() == candidate;
}

bool IsBoundedSafeString(std::string_view candidate, size_t maximum_bytes) {
  if (candidate.empty() || candidate.size() > maximum_bytes ||
      !base::IsStringUTF8(candidate)) {
    return false;
  }
  return std::ranges::none_of(candidate, [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

std::optional<PreferenceOperation> ParseOperation(std::string_view value) {
  if (value == "read") {
    return PreferenceOperation::kRead;
  }
  if (value == "write") {
    return PreferenceOperation::kWrite;
  }
  return std::nullopt;
}

std::optional<PreferenceActorType> ParseActorType(std::string_view value) {
  if (value == "cli") {
    return PreferenceActorType::kCli;
  }
  if (value == "agent") {
    return PreferenceActorType::kAgent;
  }
  return std::nullopt;
}

std::optional<PreferenceKey> ParseKey(std::string_view value) {
  if (value == "rail_density") {
    return PreferenceKey::kRailDensity;
  }
  return std::nullopt;
}

std::optional<PreferenceValue> ParseValue(PreferenceKey key,
                                          const base::Value& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  switch (key) {
    case PreferenceKey::kRailDensity:
      if (value.GetString() == "compact") {
        return PreferenceValue{.rail_density = RailDensity::kCompact};
      }
      if (value.GetString() == "comfortable") {
        return PreferenceValue{.rail_density = RailDensity::kComfortable};
      }
      return std::nullopt;
  }
}

std::string_view OperationToString(PreferenceOperation operation) {
  switch (operation) {
    case PreferenceOperation::kRead:
      return "read";
    case PreferenceOperation::kWrite:
      return "write";
  }
}

std::string_view ActorTypeToString(PreferenceActorType type) {
  switch (type) {
    case PreferenceActorType::kCli:
      return "cli";
    case PreferenceActorType::kAgent:
      return "agent";
  }
}

std::string_view KeyToString(PreferenceKey key) {
  switch (key) {
    case PreferenceKey::kRailDensity:
      return "rail_density";
  }
}

std::string_view ValueToString(const PreferenceValue& value) {
  switch (value.rail_density) {
    case RailDensity::kCompact:
      return "compact";
    case RailDensity::kComfortable:
      return "comfortable";
  }
}

std::string_view ValueSourceToString(PreferenceValueSource source) {
  switch (source) {
    case PreferenceValueSource::kDefault:
      return "default";
    case PreferenceValueSource::kUser:
      return "user";
    case PreferenceValueSource::kManaged:
      return "managed";
  }
}

base::DictValue EffectiveValueToValue(
    const EffectivePreferenceValue& effective) {
  base::DictValue value;
  value.Set("value", ValueToString(effective.value));
  value.Set("source", ValueSourceToString(effective.source));
  return value;
}

}  // namespace

PreferenceControlService::PreferenceControlService(
    PreferenceControlBackend* backend)
    : backend_(backend) {
  CHECK(backend_);
}

PreferenceControlService::~PreferenceControlService() = default;

PreferenceControlResult PreferenceControlService::Execute(
    const PreferenceRequest& request) {
  if (!backend_->Authenticate(request.capability)) {
    return Finish(request, PreferenceControlStatus::kUnauthenticated, false);
  }
  if (request.target.profile_identifier != backend_->profile_identifier()) {
    return Finish(request, PreferenceControlStatus::kProfileMismatch, false);
  }
  if (backend_->is_off_the_record()) {
    return Finish(request, PreferenceControlStatus::kPrivateProfile, false);
  }
  if (request.target.profile_instance_identifier !=
      backend_->profile_instance_identifier()) {
    return Finish(request, PreferenceControlStatus::kStaleProfile, false);
  }
  if (request.operation == PreferenceOperation::kRead) {
    return Finish(request, PreferenceControlStatus::kSuccess, true);
  }
  CHECK(request.value);
  if (backend_->IsManaged(request.key)) {
    return Finish(request, PreferenceControlStatus::kManaged, true);
  }
  if (!backend_->SetUserValue(request.key, *request.value)) {
    return Finish(request, PreferenceControlStatus::kWriteFailed, false);
  }
  return Finish(request, PreferenceControlStatus::kSuccess, true);
}

PreferenceControlResult PreferenceControlService::Finish(
    const PreferenceRequest& request,
    PreferenceControlStatus status,
    bool include_effective_value) {
  PreferenceControlResult result{
      .request_identifier = request.request_identifier,
      .status = status,
  };
  if (include_effective_value) {
    result.effective_value = backend_->GetEffectiveValue(request.key);
  }
  PreferenceAuditRecord audit{
      .request_identifier = request.request_identifier,
      .operation = request.operation,
      .actor = request.actor,
      .target = request.target,
      .key = request.key,
      .requested_value = request.value,
      .status = status,
      .effective_value = result.effective_value,
  };
  result.audit_sequence = backend_->AppendAuditRecord(audit);
  return result;
}

PreferenceRequestParseResult ParsePreferenceRequest(
    std::string_view serialized) {
  if (serialized.size() > kMaximumPreferenceRequestBytes) {
    return {.status = PreferenceRequestParseStatus::kOversized};
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(serialized, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return {.status = PreferenceRequestParseStatus::kMalformedJson};
  }
  const base::DictValue& root = value->GetDict();
  if (root.FindInt("version") != kPreferenceControlSchemaVersion) {
    return {.status = PreferenceRequestParseStatus::kUnknownVersion};
  }
  const std::string* request_identifier = root.FindString("request_id");
  if (!request_identifier || !IsCanonicalIdentifier(*request_identifier)) {
    return {.status = PreferenceRequestParseStatus::kInvalidRequestId};
  }
  const std::string* operation_string = root.FindString("operation");
  const std::optional<PreferenceOperation> operation =
      operation_string ? ParseOperation(*operation_string) : std::nullopt;
  if (!operation) {
    return {.status = PreferenceRequestParseStatus::kInvalidOperation};
  }
  const bool is_read = *operation == PreferenceOperation::kRead;
  if (!HasExactFields(
          root,
          is_read
              ? std::initializer_list<std::string_view>{"version", "request_id",
                                                        "operation", "target",
                                                        "actor",
                                                        "authentication", "key"}
              : std::initializer_list<std::string_view>{
                    "version", "request_id", "operation", "target", "actor",
                    "authentication", "key", "value"})) {
    return {.status = PreferenceRequestParseStatus::kUnexpectedField};
  }

  const base::DictValue* target = root.FindDict("target");
  if (!target || !HasExactFields(*target, {"profile_id", "instance_id"})) {
    return {.status = PreferenceRequestParseStatus::kInvalidTarget};
  }
  const std::string* profile_identifier = target->FindString("profile_id");
  const std::string* instance_identifier = target->FindString("instance_id");
  if (!profile_identifier || !instance_identifier ||
      !IsCanonicalIdentifier(*profile_identifier) ||
      !IsCanonicalIdentifier(*instance_identifier)) {
    return {.status = PreferenceRequestParseStatus::kInvalidTarget};
  }

  const base::DictValue* actor = root.FindDict("actor");
  if (!actor || !HasExactFields(*actor, {"type", "id"})) {
    return {.status = PreferenceRequestParseStatus::kInvalidActor};
  }
  const std::string* actor_type_string = actor->FindString("type");
  const std::string* actor_identifier = actor->FindString("id");
  const std::optional<PreferenceActorType> actor_type =
      actor_type_string ? ParseActorType(*actor_type_string) : std::nullopt;
  if (!actor_type || !actor_identifier ||
      !IsBoundedSafeString(*actor_identifier, kMaximumActorIdentifierBytes)) {
    return {.status = PreferenceRequestParseStatus::kInvalidActor};
  }

  const base::DictValue* authentication = root.FindDict("authentication");
  const std::string* authentication_scheme =
      authentication ? authentication->FindString("scheme") : nullptr;
  if (!authentication ||
      !HasExactFields(*authentication, {"scheme", "credential"}) ||
      !authentication_scheme || *authentication_scheme != "capability") {
    return {.status = PreferenceRequestParseStatus::kInvalidAuthentication};
  }
  const std::string* capability = authentication->FindString("credential");
  if (!capability ||
      !IsBoundedSafeString(*capability, kMaximumCapabilityBytes)) {
    return {.status = PreferenceRequestParseStatus::kInvalidAuthentication};
  }

  const std::string* key_string = root.FindString("key");
  const std::optional<PreferenceKey> key =
      key_string ? ParseKey(*key_string) : std::nullopt;
  if (!key) {
    return {.status = PreferenceRequestParseStatus::kInvalidKey};
  }

  std::optional<PreferenceValue> preference_value;
  if (*operation == PreferenceOperation::kWrite) {
    const base::Value* raw_value = root.Find("value");
    preference_value = raw_value ? ParseValue(*key, *raw_value) : std::nullopt;
    if (!preference_value) {
      return {.status = PreferenceRequestParseStatus::kInvalidValue};
    }
  }

  return {
      .status = PreferenceRequestParseStatus::kSuccess,
      .request =
          PreferenceRequest{
              .request_identifier = *request_identifier,
              .operation = *operation,
              .target = {.profile_identifier = *profile_identifier,
                         .profile_instance_identifier = *instance_identifier},
              .actor = {.type = *actor_type, .identifier = *actor_identifier},
              .capability = *capability,
              .key = *key,
              .value = preference_value,
          },
  };
}

std::string SerializePreferenceControlResult(
    const PreferenceControlResult& result) {
  base::DictValue root;
  root.Set("version", result.version);
  root.Set("request_id", result.request_identifier);
  root.Set("status", PreferenceControlStatusToString(result.status));
  if (result.effective_value) {
    root.Set("effective", EffectiveValueToValue(*result.effective_value));
  }
  if (result.audit_sequence) {
    root.Set("audit_sequence", base::NumberToString(*result.audit_sequence));
  }
  std::string serialized;
  CHECK(base::JSONWriter::Write(root, &serialized));
  return serialized;
}

base::DictValue PreferenceAuditRecordToValue(
    const PreferenceAuditRecord& record) {
  base::DictValue root;
  root.Set("version", record.version);
  root.Set("sequence", base::NumberToString(record.sequence));
  root.Set("request_id", record.request_identifier);
  root.Set("operation", OperationToString(record.operation));
  base::DictValue actor;
  actor.Set("type", ActorTypeToString(record.actor.type));
  actor.Set("id", record.actor.identifier);
  root.Set("actor", std::move(actor));
  base::DictValue target;
  target.Set("profile_id", record.target.profile_identifier);
  target.Set("instance_id", record.target.profile_instance_identifier);
  root.Set("target", std::move(target));
  root.Set("key", KeyToString(record.key));
  if (record.requested_value) {
    root.Set("requested_value", ValueToString(*record.requested_value));
  }
  root.Set("status", PreferenceControlStatusToString(record.status));
  if (record.effective_value) {
    root.Set("effective", EffectiveValueToValue(*record.effective_value));
  }
  return root;
}

std::string_view PreferenceControlStatusToString(
    PreferenceControlStatus status) {
  switch (status) {
    case PreferenceControlStatus::kSuccess:
      return "success";
    case PreferenceControlStatus::kUnauthenticated:
      return "unauthenticated";
    case PreferenceControlStatus::kProfileMismatch:
      return "profile_mismatch";
    case PreferenceControlStatus::kPrivateProfile:
      return "private_profile";
    case PreferenceControlStatus::kStaleProfile:
      return "stale_profile";
    case PreferenceControlStatus::kManaged:
      return "managed";
    case PreferenceControlStatus::kWriteFailed:
      return "write_failed";
  }
}

std::string_view PreferenceRequestParseStatusToString(
    PreferenceRequestParseStatus status) {
  switch (status) {
    case PreferenceRequestParseStatus::kSuccess:
      return "success";
    case PreferenceRequestParseStatus::kOversized:
      return "oversized";
    case PreferenceRequestParseStatus::kMalformedJson:
      return "malformed_json";
    case PreferenceRequestParseStatus::kUnknownVersion:
      return "unknown_version";
    case PreferenceRequestParseStatus::kInvalidRequestId:
      return "invalid_request_id";
    case PreferenceRequestParseStatus::kInvalidOperation:
      return "invalid_operation";
    case PreferenceRequestParseStatus::kInvalidTarget:
      return "invalid_target";
    case PreferenceRequestParseStatus::kInvalidActor:
      return "invalid_actor";
    case PreferenceRequestParseStatus::kInvalidAuthentication:
      return "invalid_authentication";
    case PreferenceRequestParseStatus::kInvalidKey:
      return "invalid_key";
    case PreferenceRequestParseStatus::kInvalidValue:
      return "invalid_value";
    case PreferenceRequestParseStatus::kUnexpectedField:
      return "unexpected_field";
  }
}

}  // namespace limitless
