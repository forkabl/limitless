// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_registry.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "shell/control_plane/limitless_preference_control_service.h"
#include "shell/control_plane/limitless_profile_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

constexpr char kProfileId[] = "10000000-0000-4000-8000-000000000001";
constexpr char kInstanceId[] = "20000000-0000-4000-8000-000000000002";
constexpr char kRequestId[] = "30000000-0000-4000-8000-000000000003";

class FakePreferenceBackend final : public PreferenceControlBackend {
 public:
  std::string_view profile_identifier() const override { return kProfileId; }
  std::string_view profile_instance_identifier() const override {
    return kInstanceId;
  }
  bool is_off_the_record() const override { return off_the_record; }
  bool Authenticate(std::string_view capability) const override {
    return capability == "capability-token";
  }
  bool IsManaged(PreferenceKey) const override { return managed; }
  EffectivePreferenceValue GetEffectiveValue(PreferenceKey) const override {
    return {.value = {.rail_density = value},
            .source = managed ? PreferenceValueSource::kManaged
                              : (changed ? PreferenceValueSource::kUser
                                         : PreferenceValueSource::kDefault)};
  }
  bool SetUserValue(PreferenceKey, const PreferenceValue& next) override {
    value = next.rail_density == RailDensity::kComfortable
                ? RailDensity::kComfortable
                : RailDensity::kCompact;
    changed = true;
    return true;
  }
  uint64_t AppendAuditRecord(const PreferenceAuditRecord& record) override {
    audit.push_back(record);
    return audit.size();
  }

  bool off_the_record = false;
  bool managed = false;
  bool changed = false;
  RailDensity value = RailDensity::kCompact;
  std::vector<PreferenceAuditRecord> audit;
};

std::string ValidRequest(std::string_view operation = "write",
                         std::string_view value = "comfortable") {
  std::string request = R"json({"version":1,"request_id":")json";
  request += kRequestId;
  request += R"json(","operation":")json";
  request += operation;
  request += R"json(","target":{"profile_id":")json";
  request += kProfileId;
  request += R"json(","instance_id":")json";
  request += kInstanceId;
  request += R"json("},"actor":{"type":"cli","id":"test-cli"},)json";
  request +=
      R"json("authentication":{"scheme":"capability","credential":"capability-token"},)json";
  request += R"json("key":"rail_density")json";
  if (operation == "write") {
    request += R"json(,"value":")json";
    request += value;
    request += '"';
  }
  request += "}";
  return request;
}

TEST(LimitlessControlPlaneBrowserTest,
     TypedWritePublishesEffectiveValueAndAudit) {
  const PreferenceRequestParseResult parsed =
      ParsePreferenceRequest(ValidRequest());
  ASSERT_TRUE(parsed.succeeded());

  FakePreferenceBackend backend;
  PreferenceControlService service(&backend);
  const PreferenceControlResult result = service.Execute(*parsed.request);

  ASSERT_TRUE(result.succeeded());
  ASSERT_TRUE(result.effective_value);
  EXPECT_EQ(RailDensity::kComfortable,
            result.effective_value->value.rail_density);
  ASSERT_EQ(1u, backend.audit.size());
  EXPECT_EQ(PreferenceControlStatus::kSuccess, backend.audit.front().status);
  EXPECT_EQ(RailDensity::kComfortable,
            backend.audit.front().effective_value->value.rail_density);
}

TEST(LimitlessControlPlaneBrowserTest,
     RejectsInvalidAndForeignWritesBeforeMutation) {
  FakePreferenceBackend backend;
  PreferenceControlService service(&backend);

  PreferenceRequestParseResult invalid =
      ParsePreferenceRequest(ValidRequest("write", "unknown"));
  EXPECT_EQ(PreferenceRequestParseStatus::kInvalidValue, invalid.status);
  EXPECT_FALSE(backend.changed);

  ASSERT_TRUE(ParsePreferenceRequest(ValidRequest()).succeeded());
  PreferenceRequest foreign = *ParsePreferenceRequest(ValidRequest()).request;
  foreign.target.profile_identifier = "40000000-0000-4000-8000-000000000004";
  EXPECT_EQ(PreferenceControlStatus::kProfileMismatch,
            service.Execute(foreign).status);
  EXPECT_FALSE(backend.changed);

  backend.off_the_record = true;
  PreferenceRequest private_request =
      *ParsePreferenceRequest(ValidRequest()).request;
  EXPECT_EQ(PreferenceControlStatus::kPrivateProfile,
            service.Execute(private_request).status);
  EXPECT_FALSE(backend.changed);

  PreferenceRequest unauthenticated = private_request;
  unauthenticated.capability = "";
  backend.off_the_record = false;
  EXPECT_EQ(PreferenceControlStatus::kUnauthenticated,
            service.Execute(unauthenticated).status);
  EXPECT_FALSE(backend.changed);
  EXPECT_EQ(3u, backend.audit.size());
}

TEST(LimitlessControlPlaneBrowserTest,
     ManagedWriteReturnsEffectivePolicyValue) {
  FakePreferenceBackend backend;
  backend.managed = true;
  PreferenceControlService service(&backend);
  PreferenceRequest request = *ParsePreferenceRequest(ValidRequest()).request;

  EXPECT_EQ(PreferenceControlStatus::kManaged, service.Execute(request).status);
  EXPECT_FALSE(backend.changed);
  ASSERT_EQ(1u, backend.audit.size());
  EXPECT_EQ(PreferenceValueSource::kManaged,
            backend.audit.front().effective_value->source);
}

class LimitlessProfilePrefsBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(LimitlessProfilePrefsBrowserTest,
                       RegistersRailDensityAsNonSyncProfilePref) {
  PrefService* profile_prefs = browser()->GetProfile()->GetPrefs();
  const PrefService::Preference* rail_density =
      profile_prefs->FindPreference(kRailDensityPref);

  ASSERT_NE(nullptr, rail_density);
  EXPECT_EQ(base::Value::Type::STRING, rail_density->GetType());
  EXPECT_EQ(kCompactRailDensity, profile_prefs->GetString(kRailDensityPref));
  EXPECT_TRUE(rail_density->IsDefaultValue());
  EXPECT_EQ(PrefRegistry::NO_REGISTRATION_FLAGS,
            rail_density->registration_flags());
}

}  // namespace
}  // namespace limitless
