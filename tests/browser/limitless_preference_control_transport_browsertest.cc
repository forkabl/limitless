// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/control_plane/limitless_preference_control_transport.h"

#include <sys/stat.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/scoped_environment_variable_override.h"
#include "base/task/thread_pool.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "shell/control_plane/limitless_profile_prefs.h"
#include "shell/native/swiftui_rail/limitless_swiftui_rail_host.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

constexpr char kCapabilityEnvironmentVariable[] =
    "LIMITLESS_CONTROL_CAPABILITY";

struct CliResult {
  bool launched = false;
  int exit_code = -1;
  std::string output;
};

base::FilePath LimitlessCtlPath() {
  return base::FilePath::FromUTF8Unsafe(__FILE__)
      .DirName()
      .DirName()
      .DirName()
      .AppendASCII("tools")
      .AppendASCII("control_plane")
      .AppendASCII("limitlessctl");
}

CliResult RunCliBlocking(std::vector<std::string> arguments,
                         std::optional<std::string> capability) {
  std::vector<std::string> argv = {
      "/usr/bin/python3",
      LimitlessCtlPath().value(),
  };
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  std::optional<base::ScopedEnvironmentVariableOverride> scoped_capability;
  if (capability) {
    scoped_capability.emplace(kCapabilityEnvironmentVariable, *capability);
  } else {
    scoped_capability.emplace(kCapabilityEnvironmentVariable);
  }

  CliResult result;
  result.launched =
      base::GetAppOutputWithExitCode(argv, &result.output, &result.exit_code);
  return result;
}

CliResult RunCli(std::vector<std::string> arguments,
                 std::optional<std::string> capability = std::nullopt) {
  base::test::TestFuture<CliResult> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&RunCliBlocking, std::move(arguments),
                     std::move(capability)),
      result.GetCallback());
  return result.Take();
}

base::DictValue ParseCliResult(const CliResult& result) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(result.output, base::JSON_PARSE_RFC);
  EXPECT_TRUE(parsed) << result.output;
  EXPECT_TRUE(parsed && parsed->is_dict()) << result.output;
  return parsed && parsed->is_dict() ? std::move(*parsed).TakeDict()
                                     : base::DictValue();
}

std::string_view FindStringOrEmpty(const base::DictValue& value,
                                   std::string_view key) {
  const std::string* found = value.FindString(key);
  return found ? std::string_view(*found) : std::string_view();
}

std::vector<std::string> CliPrefix(const base::FilePath& user_data_directory) {
  return {"--user-data-dir", user_data_directory.value()};
}

void AppendArguments(std::vector<std::string>* destination,
                     std::initializer_list<std::string_view> arguments) {
  for (std::string_view argument : arguments) {
    destination->emplace_back(argument);
  }
}

base::FilePath WriteAliasedDescriptor(
    const PreferenceControlTransport& transport,
    std::string_view profile_identifier,
    std::string_view instance_identifier) {
  std::string serialized;
  EXPECT_TRUE(base::ReadFileToString(transport.descriptor_path(), &serialized));
  std::optional<base::Value> parsed =
      base::JSONReader::Read(serialized, base::JSON_PARSE_RFC);
  EXPECT_TRUE(parsed && parsed->is_dict());
  if (!parsed || !parsed->is_dict()) {
    return {};
  }
  base::DictValue descriptor = std::move(*parsed).TakeDict();
  descriptor.Set("profile_id", profile_identifier);
  descriptor.Set("instance_id", instance_identifier);
  EXPECT_TRUE(base::JSONWriter::Write(descriptor, &serialized));
  const base::FilePath path = transport.runtime_directory().AppendASCII(
      std::string(profile_identifier) + "." + std::string(instance_identifier) +
      ".json");
  EXPECT_TRUE(base::WriteFile(path, serialized));
  EXPECT_EQ(0, chmod(path.value().c_str(), S_IRUSR | S_IWUSR));
  return path;
}

class LimitlessPreferenceControlTransportBrowserTest
    : public InProcessBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(kSwiftUIBoundaryProbeSwitch);
  }
};

IN_PROC_BROWSER_TEST_F(
    LimitlessPreferenceControlTransportBrowserTest,
    CliDiscoversQueriesAndExecutesAgainstExactRunningRegularProfile) {
  Profile* const profile = browser()->GetProfile();
  ASSERT_TRUE(profile);
  content::WebContents* const authoritative_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(authoritative_contents);
  BrowserView* const browser_view =
      BrowserView::GetBrowserViewForBrowser(browser());
  ASSERT_TRUE(browser_view);
  views::View* const rail = browser_view->limitless_swiftui_rail_for_testing();
  ASSERT_TRUE(rail);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = GetSwiftUITabRailSnapshotForTesting(rail);
    return snapshot && snapshot->rail_density == "compact" &&
           snapshot->first_tab_row_height == 28;
  }));

  ProfilePreferenceControlBackend backend(profile);
  const base::FilePath user_data_directory = profile->GetPath().DirName();
  PreferenceControlTransport transport(&backend, user_data_directory);
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess, transport.Start());
  ASSERT_TRUE(transport.running());
  ASSERT_TRUE(base::PathExists(transport.descriptor_path()));
  ASSERT_TRUE(base::PathExists(transport.socket_path()));

  std::vector<std::string> discover = CliPrefix(user_data_directory);
  AppendArguments(&discover, {"discover"});
  const CliResult discovered = RunCli(std::move(discover));
  ASSERT_TRUE(discovered.launched);
  ASSERT_EQ(0, discovered.exit_code) << discovered.output;
  const base::DictValue discovery = ParseCliResult(discovered);
  EXPECT_EQ(1, discovery.FindInt("version"));
  EXPECT_EQ("success", FindStringOrEmpty(discovery, "status"));
  const base::ListValue* profiles = discovery.FindList("profiles");
  ASSERT_TRUE(profiles);
  ASSERT_EQ(1u, profiles->size());
  const base::DictValue& discovered_profile = profiles->front().GetDict();
  EXPECT_EQ(backend.profile_identifier(),
            FindStringOrEmpty(discovered_profile, "profile_id"));
  EXPECT_EQ(backend.profile_instance_identifier(),
            FindStringOrEmpty(discovered_profile, "instance_id"));
  EXPECT_FALSE(discovered_profile.contains("capability"));

  const auto run_preference = [&](std::initializer_list<std::string_view> tail,
                                  std::optional<std::string> capability =
                                      std::nullopt) {
    std::vector<std::string> arguments = CliPrefix(user_data_directory);
    AppendArguments(&arguments,
                    {"--profile", backend.profile_identifier(), "--instance",
                     backend.profile_instance_identifier()});
    AppendArguments(&arguments, tail);
    return RunCli(std::move(arguments), std::move(capability));
  };

  const CliResult queried =
      run_preference({"preferences", "get", "rail_density"},
                     std::string(backend.capability()));
  ASSERT_TRUE(queried.launched);
  ASSERT_EQ(0, queried.exit_code) << queried.output;
  base::DictValue query_response = ParseCliResult(queried);
  EXPECT_EQ("success", FindStringOrEmpty(query_response, "status"));
  ASSERT_TRUE(query_response.FindDict("effective"));
  EXPECT_EQ("compact",
            FindStringOrEmpty(*query_response.FindDict("effective"), "value"));

  const CliResult executed =
      run_preference({"preferences", "set", "rail_density", "comfortable"},
                     std::string(backend.capability()));
  ASSERT_TRUE(executed.launched);
  ASSERT_EQ(0, executed.exit_code) << executed.output;
  base::DictValue execute_response = ParseCliResult(executed);
  EXPECT_EQ("success", FindStringOrEmpty(execute_response, "status"));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = GetSwiftUITabRailSnapshotForTesting(rail);
    return profile->GetPrefs()->GetString(kRailDensityPref) == "comfortable" &&
           snapshot && snapshot->rail_density == "comfortable" &&
           snapshot->first_tab_row_height == 34;
  }));
  EXPECT_EQ(authoritative_contents,
            browser()->tab_strip_model()->GetActiveWebContents());

  const CliResult queried_after_write =
      run_preference({"preferences", "get", "rail_density"},
                     std::string(backend.capability()));
  ASSERT_EQ(0, queried_after_write.exit_code) << queried_after_write.output;
  query_response = ParseCliResult(queried_after_write);
  ASSERT_TRUE(query_response.FindDict("effective"));
  EXPECT_EQ("comfortable",
            FindStringOrEmpty(*query_response.FindDict("effective"), "value"));

  transport.Stop();
  EXPECT_FALSE(transport.running());
  EXPECT_FALSE(base::PathExists(transport.descriptor_path()));
  EXPECT_FALSE(base::PathExists(transport.socket_path()));
}

IN_PROC_BROWSER_TEST_F(
    LimitlessPreferenceControlTransportBrowserTest,
    CliRejectsUnauthenticatedStaleForeignAndPrivateProfileRequests) {
  Profile* const profile = browser()->GetProfile();
  ProfilePreferenceControlBackend backend(profile);
  const base::FilePath user_data_directory = profile->GetPath().DirName();
  PreferenceControlTransport transport(&backend, user_data_directory);
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess, transport.Start());

  const auto run_profile = [&](std::string_view profile_identifier,
                               std::string_view instance_identifier,
                               std::string capability) {
    std::vector<std::string> arguments = CliPrefix(user_data_directory);
    AppendArguments(&arguments, {"--profile", profile_identifier, "--instance",
                                 instance_identifier, "preferences", "set",
                                 "rail_density", "comfortable"});
    return RunCli(std::move(arguments), std::move(capability));
  };

  const CliResult unauthenticated = run_profile(
      backend.profile_identifier(), backend.profile_instance_identifier(),
      base::Uuid::GenerateRandomV4().AsLowercaseString());
  ASSERT_TRUE(unauthenticated.launched);
  EXPECT_NE(0, unauthenticated.exit_code) << unauthenticated.output;
  EXPECT_EQ("unauthenticated",
            FindStringOrEmpty(ParseCliResult(unauthenticated), "status"));
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  const std::string stale_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const base::FilePath stale_descriptor = WriteAliasedDescriptor(
      transport, backend.profile_identifier(), stale_instance);
  ASSERT_FALSE(stale_descriptor.empty());
  const CliResult stale =
      run_profile(backend.profile_identifier(), stale_instance,
                  std::string(backend.capability()));
  ASSERT_TRUE(stale.launched);
  EXPECT_NE(0, stale.exit_code) << stale.output;
  EXPECT_EQ("stale_profile",
            FindStringOrEmpty(ParseCliResult(stale), "status"));

  const std::string foreign_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const base::FilePath foreign_descriptor = WriteAliasedDescriptor(
      transport, foreign_profile, backend.profile_instance_identifier());
  ASSERT_FALSE(foreign_descriptor.empty());
  const CliResult foreign =
      run_profile(foreign_profile, backend.profile_instance_identifier(),
                  std::string(backend.capability()));
  ASSERT_TRUE(foreign.launched);
  EXPECT_NE(0, foreign.exit_code) << foreign.output;
  EXPECT_EQ("profile_mismatch",
            FindStringOrEmpty(ParseCliResult(foreign), "status"));
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  Browser* const private_browser = CreateIncognitoBrowser(profile);
  ASSERT_TRUE(private_browser);
  ProfilePreferenceControlBackend private_backend(
      private_browser->GetProfile());
  PreferenceControlTransport private_transport(&private_backend,
                                               user_data_directory);
  EXPECT_EQ(PreferenceControlTransportStartStatus::kPrivateProfile,
            private_transport.Start());
  EXPECT_FALSE(private_transport.running());
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  EXPECT_TRUE(base::DeleteFile(stale_descriptor));
  EXPECT_TRUE(base::DeleteFile(foreign_descriptor));
  transport.Stop();
}

}  // namespace
}  // namespace limitless
