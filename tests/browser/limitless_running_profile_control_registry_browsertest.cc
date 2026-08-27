// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/control_plane/limitless_running_profile_control_registry.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

class LimitlessRunningProfileControlRegistryBrowserTest
    : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(LimitlessRunningProfileControlRegistryBrowserTest,
                       SharesOneBackendAndEndsItAfterLastWindow) {
  RunningProfileControlRegistry registry;
  int first_changes = 0;
  int second_changes = 0;
  auto first = registry.RegisterWindow(
      browser(), base::BindRepeating(
                     [](int* count, const EffectivePreferenceValue&) {
                       ++*count;
                     },
                     &first_changes));
  ASSERT_TRUE(first);
  ProfilePreferenceControlBackend* const backend = first->backend();
  ASSERT_TRUE(backend);
  const std::string profile_identifier(backend->profile_identifier());
  const std::string instance_identifier(
      backend->profile_instance_identifier());

  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  auto second = registry.RegisterWindow(
      second_browser, base::BindRepeating(
                          [](int* count, const EffectivePreferenceValue&) {
                            ++*count;
                          },
                          &second_changes));
  ASSERT_TRUE(second);
  EXPECT_EQ(backend, second->backend());
  EXPECT_EQ(1u, registry.profile_count_for_testing());
  EXPECT_EQ(2u, registry.window_count_for_testing(profile_identifier));
  ASSERT_EQ(1u, registry.DiscoverProfiles().size());
  EXPECT_EQ(profile_identifier,
            registry.DiscoverProfiles()[0].profile_identifier);
  EXPECT_EQ(instance_identifier,
            registry.DiscoverProfiles()[0].profile_instance_identifier);

  ASSERT_TRUE(backend->SetUserValue(
      PreferenceKey::kRailDensity,
      PreferenceValue{.rail_density = RailDensity::kComfortable}));
  EXPECT_EQ(1, first_changes);
  EXPECT_EQ(1, second_changes);

  second.reset();
  EXPECT_EQ(1u, registry.window_count_for_testing(profile_identifier));
  EXPECT_EQ(backend, registry.FindBackend(profile_identifier));
  first.reset();
  EXPECT_EQ(0u, registry.profile_count_for_testing());
  EXPECT_EQ(nullptr, registry.FindBackend(profile_identifier));
}

IN_PROC_BROWSER_TEST_F(LimitlessRunningProfileControlRegistryBrowserTest,
                       RejectsPrivateAndDuplicateWindowRegistrations) {
  RunningProfileControlRegistry registry;
  auto regular = registry.RegisterWindow(browser());
  ASSERT_TRUE(regular);
  EXPECT_FALSE(registry.RegisterWindow(browser()));

  Browser* private_browser = CreateIncognitoBrowser(browser()->GetProfile());
  ASSERT_TRUE(private_browser);
  EXPECT_FALSE(registry.RegisterWindow(private_browser));
  EXPECT_EQ(1u, registry.profile_count_for_testing());
}

}  // namespace
}  // namespace limitless
