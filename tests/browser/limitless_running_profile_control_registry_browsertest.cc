// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/control_plane/limitless_running_profile_control_registry.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/run_until.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

class LimitlessRunningProfileControlRegistryBrowserTest
    : public InProcessBrowserTest {};

void RegisterReentrantWindow(
    RunningProfileControlRegistry* registry,
    Browser* second_browser,
    std::unique_ptr<RunningProfileControlRegistry::Registration>* registration,
    std::shared_ptr<std::atomic<bool>> once) {
  if (!once->exchange(true)) {
    *registration = registry->RegisterWindow(second_browser);
  }
}

IN_PROC_BROWSER_TEST_F(LimitlessRunningProfileControlRegistryBrowserTest,
                       SharesOneBackendAndEndsItAfterLastWindow) {
  RunningProfileControlRegistry registry;
  int first_changes = 0;
  int second_changes = 0;
  auto first = registry.RegisterWindow(
      browser(),
      base::BindRepeating(
          [](int* count, const EffectivePreferenceValue&) { ++*count; },
          &first_changes));
  ASSERT_TRUE(first);
  ProfilePreferenceControlBackend* const backend = first->backend();
  ASSERT_TRUE(backend);
  const std::string profile_identifier(backend->profile_identifier());
  const std::string instance_identifier(backend->profile_instance_identifier());

  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  auto second = registry.RegisterWindow(
      second_browser,
      base::BindRepeating(
          [](int* count, const EffectivePreferenceValue&) { ++*count; },
          &second_changes));
  ASSERT_TRUE(second);
  EXPECT_EQ(backend, second->backend());
  EXPECT_EQ(1u, registry.profile_count_for_testing());
  ASSERT_TRUE(base::test::RunUntil(
      [&registry] { return registry.DiscoverProfiles().size() == 1; }));
  EXPECT_EQ(2u, registry.window_count_for_testing(profile_identifier));
  const std::vector<RunningProfileControlDescriptor> discovered_profiles =
      registry.DiscoverProfiles();
  ASSERT_EQ(1u, discovered_profiles.size());
  EXPECT_EQ(profile_identifier, discovered_profiles[0].profile_identifier);
  EXPECT_EQ(instance_identifier,
            discovered_profiles[0].profile_instance_identifier);

  ASSERT_TRUE(backend->SetUserValue(
      PreferenceKey::kRailDensity,
      PreferenceValue{.rail_density = RailDensity::kComfortable}));
  EXPECT_EQ(1, first_changes);
  EXPECT_EQ(1, second_changes);

  CloseBrowserSynchronously(second_browser);
  EXPECT_EQ(nullptr, second->backend());
  EXPECT_EQ(nullptr, second->browser_window());
  EXPECT_EQ(1u, registry.window_count_for_testing(profile_identifier));
  EXPECT_EQ(backend, registry.FindBackend(profile_identifier));
  second.reset();
  first.reset();
  EXPECT_TRUE(base::test::RunUntil(
      [&registry] { return registry.profile_count_for_testing() == 0; }));
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
  EXPECT_TRUE(base::test::RunUntil(
      [&registry] { return registry.DiscoverProfiles().size() == 1; }));
}

IN_PROC_BROWSER_TEST_F(LimitlessRunningProfileControlRegistryBrowserTest,
                       RegistrationSafelyOutlivesStackRegistry) {
  std::unique_ptr<RunningProfileControlRegistry::Registration> registration;
  {
    RunningProfileControlRegistry registry;
    registration = registry.RegisterWindow(browser());
    ASSERT_TRUE(registration);
    ASSERT_TRUE(registration->backend());
    ASSERT_TRUE(base::test::RunUntil(
        [&registry] { return registry.DiscoverProfiles().size() == 1; }));
  }

  EXPECT_EQ(nullptr, registration->backend());
  EXPECT_EQ(browser(), registration->browser_window());
  registration.reset();
}

IN_PROC_BROWSER_TEST_F(LimitlessRunningProfileControlRegistryBrowserTest,
                       ReentrantWindowRegistrationQueuesStoppingRecovery) {
  RunningProfileControlRegistry registry;
  Browser* second_browser = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(second_browser);
  std::unique_ptr<RunningProfileControlRegistry::Registration>
      second_registration;
  auto attempts = std::make_shared<std::atomic<int>>(0);
  auto once = std::make_shared<std::atomic<bool>>(false);
  PreferenceControlTransport::TestingHooks hooks;
  hooks.should_exit_run = base::BindRepeating(
      [](std::shared_ptr<std::atomic<int>> attempts) {
        return attempts->fetch_add(1) == 0;
      },
      attempts);
  hooks.entered_stopping_after_unexpected_run_exit =
      base::BindRepeating(&RegisterReentrantWindow, &registry, second_browser,
                          &second_registration, once);
  registry.SetTransportTestingHooksForTesting(std::move(hooks));
  auto first_registration = registry.RegisterWindow(browser());
  ASSERT_TRUE(first_registration);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return second_registration && attempts->load() == 2 &&
           registry.DiscoverProfiles().size() == 1;
  }));
  EXPECT_EQ(1u, registry.profile_count_for_testing());
  EXPECT_EQ(2u, registry.window_count_for_testing(
                    first_registration->backend()->profile_identifier()));
  second_registration.reset();
  first_registration.reset();
  ASSERT_TRUE(base::test::RunUntil(
      [&registry] { return registry.profile_count_for_testing() == 0; }));
}

}  // namespace
}  // namespace limitless
