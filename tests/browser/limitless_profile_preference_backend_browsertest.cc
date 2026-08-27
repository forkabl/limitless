// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/control_plane/limitless_profile_preference_backend.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

PreferenceRequest MakeWriteRequest(
    const ProfilePreferenceControlBackend& backend,
    RailDensity density = RailDensity::kComfortable) {
  return {
      .request_identifier = base::Uuid::GenerateRandomV4().AsLowercaseString(),
      .operation = PreferenceOperation::kWrite,
      .target = {.profile_identifier =
                     std::string(backend.profile_identifier()),
                 .profile_instance_identifier =
                     std::string(backend.profile_instance_identifier())},
      .actor = {.type = PreferenceActorType::kCli,
                .identifier = "profile-backend-browser-test"},
      .capability = std::string(backend.capability()),
      .key = PreferenceKey::kRailDensity,
      .value = PreferenceValue{.rail_density = density},
  };
}

class LimitlessProfilePreferenceBackendBrowserTest
    : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(LimitlessProfilePreferenceBackendBrowserTest,
                       WritesLivePersistsAndRejectsStaleInstance) {
  content::WebContents* const contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  Profile* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  PrefService* prefs = profile->GetPrefs();
  ASSERT_TRUE(prefs->FindPreference(kRailDensityPref));
  ASSERT_TRUE(prefs->FindPreference(kControlProfileIdentifierPref));

  int change_count = 0;
  EffectivePreferenceValue observed;
  auto backend = std::make_unique<ProfilePreferenceControlBackend>(
      profile, base::BindRepeating(
                   [](int* count, EffectivePreferenceValue* last,
                      const EffectivePreferenceValue& value) {
                     ++*count;
                     *last = value;
                   },
                   &change_count, &observed));
  const std::string persistent_profile_id(backend->profile_identifier());
  const std::string stale_instance_id(backend->profile_instance_identifier());
  PreferenceControlService service(backend.get());

  const PreferenceControlResult result =
      service.Execute(MakeWriteRequest(*backend));
  ASSERT_TRUE(result.succeeded());
  EXPECT_EQ(kComfortableRailDensity, prefs->GetString(kRailDensityPref));
  EXPECT_EQ(1, change_count);
  EXPECT_EQ(RailDensity::kComfortable, observed.value.rail_density);
  EXPECT_EQ(contents, browser()->tab_strip_model()->GetActiveWebContents());
  ASSERT_EQ(1u, prefs->GetList(kPreferenceAuditRecordsPref).size());

  backend.reset();
  auto restarted_backend =
      std::make_unique<ProfilePreferenceControlBackend>(profile);
  EXPECT_EQ(persistent_profile_id, restarted_backend->profile_identifier());
  EXPECT_NE(stale_instance_id,
            restarted_backend->profile_instance_identifier());
  EXPECT_EQ(RailDensity::kComfortable,
            restarted_backend->GetEffectiveValue(PreferenceKey::kRailDensity)
                .value.rail_density);

  PreferenceRequest stale =
      MakeWriteRequest(*restarted_backend, RailDensity::kCompact);
  stale.target.profile_instance_identifier = stale_instance_id;
  PreferenceControlService restarted_service(restarted_backend.get());
  EXPECT_EQ(PreferenceControlStatus::kStaleProfile,
            restarted_service.Execute(stale).status);
  EXPECT_EQ(kComfortableRailDensity, prefs->GetString(kRailDensityPref));
  EXPECT_EQ(contents, browser()->tab_strip_model()->GetActiveWebContents());
}

IN_PROC_BROWSER_TEST_F(LimitlessProfilePreferenceBackendBrowserTest,
                       PrivateProfileCannotPersistRegularMetadata) {
  content::WebContents* const regular_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  Profile* regular_profile =
      Profile::FromBrowserContext(regular_contents->GetBrowserContext());
  PrefService* regular_prefs = regular_profile->GetPrefs();
  const size_t regular_audit_count =
      regular_prefs->GetList(kPreferenceAuditRecordsPref).size();
  ProfilePreferenceControlBackend regular_backend(regular_profile);
  const std::string regular_profile_id(regular_backend.profile_identifier());

  Browser* private_browser = CreateIncognitoBrowser(regular_profile);
  ASSERT_TRUE(private_browser);
  content::WebContents* const private_contents =
      private_browser->tab_strip_model()->GetActiveWebContents();
  ProfilePreferenceControlBackend private_backend(
      Profile::FromBrowserContext(private_contents->GetBrowserContext()));
  EXPECT_TRUE(private_backend.is_off_the_record());
  EXPECT_NE(regular_profile_id, private_backend.profile_identifier());
  PreferenceControlService service(&private_backend);

  EXPECT_EQ(PreferenceControlStatus::kPrivateProfile,
            service.Execute(MakeWriteRequest(private_backend)).status);
  EXPECT_EQ(regular_audit_count,
            regular_prefs->GetList(kPreferenceAuditRecordsPref).size());
  EXPECT_EQ(kCompactRailDensity, regular_prefs->GetString(kRailDensityPref));
}

IN_PROC_BROWSER_TEST_F(LimitlessProfilePreferenceBackendBrowserTest,
                       AuditLogRetainsNewest256RecordsWithMonotonicSequence) {
  Profile* profile = browser()->GetProfile();
  ProfilePreferenceControlBackend backend(profile);
  PreferenceControlService service(&backend);
  for (int i = 0; i < 260; ++i) {
    PreferenceControlResult result = service.Execute(MakeWriteRequest(
        backend, i % 2 ? RailDensity::kCompact : RailDensity::kComfortable));
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(static_cast<uint64_t>(i + 1), result.audit_sequence);
  }
  const base::ListValue& records =
      profile->GetPrefs()->GetList(kPreferenceAuditRecordsPref);
  ASSERT_EQ(256u, records.size());
  EXPECT_EQ("5", records.front().GetDict().FindString("sequence"));
  EXPECT_EQ("260", records.back().GetDict().FindString("sequence"));
}

IN_PROC_BROWSER_TEST_F(LimitlessProfilePreferenceBackendBrowserTest,
                       CorruptedRailDensityFailsSafeToCompact) {
  Profile* profile = browser()->GetProfile();
  profile->GetPrefs()->SetString(kRailDensityPref, "corrupt-value");
  ProfilePreferenceControlBackend backend(profile);
  EffectivePreferenceValue effective =
      backend.GetEffectiveValue(PreferenceKey::kRailDensity);
  EXPECT_EQ(RailDensity::kCompact, effective.value.rail_density);
  EXPECT_EQ(PreferenceValueSource::kUser, effective.source);
}

}  // namespace
}  // namespace limitless
