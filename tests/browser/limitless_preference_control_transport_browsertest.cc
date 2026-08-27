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
#include "base/task/thread_pool.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "shell/control_plane/limitless_profile_prefs.h"
#include "shell/control_plane/limitless_running_profile_control_registry.h"
#include "shell/native/swiftui_rail/limitless_swiftui_rail_host.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

struct CliResult {
  bool launched = false;
  int exit_code = -1;
  std::string output;
};

struct AliasedDescriptor {
  base::FilePath descriptor;
  base::FilePath credential;
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

CliResult RunCliBlocking(std::vector<std::string> arguments) {
  std::vector<std::string> argv = {
      "/usr/bin/python3",
      LimitlessCtlPath().value(),
  };
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  CliResult result;
  result.launched =
      base::GetAppOutputWithExitCode(argv, &result.output, &result.exit_code);
  return result;
}

CliResult RunCli(std::vector<std::string> arguments) {
  base::test::TestFuture<CliResult> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&RunCliBlocking, std::move(arguments)),
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

AliasedDescriptor WriteAliasedDescriptor(
    const base::FilePath& source_descriptor,
    const base::FilePath& runtime_directory,
    std::string_view profile_identifier,
    std::string_view instance_identifier) {
  std::string serialized;
  EXPECT_TRUE(base::ReadFileToString(source_descriptor, &serialized));
  std::optional<base::Value> parsed =
      base::JSONReader::Read(serialized, base::JSON_PARSE_RFC);
  EXPECT_TRUE(parsed && parsed->is_dict());
  if (!parsed || !parsed->is_dict()) {
    return {};
  }
  base::DictValue descriptor = std::move(*parsed).TakeDict();
  const std::string* source_credential =
      descriptor.FindString("credential_path");
  EXPECT_TRUE(source_credential);
  std::string capability;
  EXPECT_TRUE(
      source_credential &&
      base::ReadFileToString(base::FilePath(*source_credential), &capability));
  if (!source_credential || capability.empty()) {
    return {};
  }

  const std::string endpoint_stem =
      std::string(profile_identifier) + "." + std::string(instance_identifier);
  AliasedDescriptor alias{
      .descriptor = runtime_directory.AppendASCII(endpoint_stem + ".json"),
      .credential = runtime_directory.AppendASCII(endpoint_stem + ".token"),
  };
  EXPECT_TRUE(base::WriteFile(alias.credential, capability));
  EXPECT_EQ(0, chmod(alias.credential.value().c_str(), S_IRUSR | S_IWUSR));
  descriptor.Set("profile_id", profile_identifier);
  descriptor.Set("instance_id", instance_identifier);
  descriptor.Set("credential_path", alias.credential.value());
  EXPECT_TRUE(base::JSONWriter::Write(descriptor, &serialized));
  EXPECT_TRUE(base::WriteFile(alias.descriptor, serialized));
  EXPECT_EQ(0, chmod(alias.descriptor.value().c_str(), S_IRUSR | S_IWUSR));
  return alias;
}

void DeleteAlias(const AliasedDescriptor& alias) {
  EXPECT_TRUE(base::DeleteFile(alias.descriptor));
  EXPECT_TRUE(base::DeleteFile(alias.credential));
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

  RunningProfileControlRegistry& registry = GetRunningProfileControlRegistry();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return registry.DiscoverProfiles().size() == 1; }));
  const RunningProfileControlDescriptor running_profile =
      registry.DiscoverProfiles().front();
  ProfilePreferenceControlBackend* const backend =
      registry.FindBackend(running_profile.profile_identifier);
  ASSERT_TRUE(backend);
  const base::FilePath user_data_directory = profile->GetPath().DirName();
  const base::FilePath descriptor_path =
      user_data_directory.AppendASCII(kPreferenceControlRuntimeDirectory)
          .AppendASCII(running_profile.profile_identifier + "." +
                       running_profile.profile_instance_identifier + ".json");
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return base::PathExists(descriptor_path); }));

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
  EXPECT_EQ(backend->profile_identifier(),
            FindStringOrEmpty(discovered_profile, "profile_id"));
  EXPECT_EQ(backend->profile_instance_identifier(),
            FindStringOrEmpty(discovered_profile, "instance_id"));
  EXPECT_FALSE(discovered_profile.contains("capability"));
  EXPECT_FALSE(discovered_profile.contains("credential_path"));

  const auto run_preference =
      [&](std::initializer_list<std::string_view> tail) {
        std::vector<std::string> arguments = CliPrefix(user_data_directory);
        AppendArguments(&arguments,
                        {"--profile", backend->profile_identifier(),
                         "--instance", backend->profile_instance_identifier()});
        AppendArguments(&arguments, tail);
        return RunCli(std::move(arguments));
      };

  const CliResult queried =
      run_preference({"preferences", "get", "rail_density"});
  ASSERT_TRUE(queried.launched);
  ASSERT_EQ(0, queried.exit_code) << queried.output;
  base::DictValue query_response = ParseCliResult(queried);
  EXPECT_EQ("success", FindStringOrEmpty(query_response, "status"));
  ASSERT_TRUE(query_response.FindDict("effective"));
  EXPECT_EQ("compact",
            FindStringOrEmpty(*query_response.FindDict("effective"), "value"));

  const CliResult executed =
      run_preference({"preferences", "set", "rail_density", "comfortable"});
  ASSERT_TRUE(executed.launched);
  ASSERT_EQ(0, executed.exit_code) << executed.output;
  EXPECT_EQ("success", FindStringOrEmpty(ParseCliResult(executed), "status"));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = GetSwiftUITabRailSnapshotForTesting(rail);
    return profile->GetPrefs()->GetString(kRailDensityPref) == "comfortable" &&
           snapshot && snapshot->rail_density == "comfortable" &&
           snapshot->first_tab_row_height == 34;
  }));
  EXPECT_EQ(authoritative_contents,
            browser()->tab_strip_model()->GetActiveWebContents());

  const CliResult queried_after_write =
      run_preference({"preferences", "get", "rail_density"});
  ASSERT_EQ(0, queried_after_write.exit_code) << queried_after_write.output;
  query_response = ParseCliResult(queried_after_write);
  ASSERT_TRUE(query_response.FindDict("effective"));
  EXPECT_EQ("comfortable",
            FindStringOrEmpty(*query_response.FindDict("effective"), "value"));
  EXPECT_EQ(authoritative_contents,
            browser()->tab_strip_model()->GetActiveWebContents());
}

IN_PROC_BROWSER_TEST_F(
    LimitlessPreferenceControlTransportBrowserTest,
    CliRejectsUnauthenticatedStaleForeignAndPrivateProfileRequests) {
  Profile* const profile = browser()->GetProfile();
  const base::FilePath user_data_directory = profile->GetPath().DirName();
  const base::FilePath runtime_directory =
      user_data_directory.AppendASCII(kPreferenceControlRuntimeDirectory);
  RunningProfileControlRegistry& registry = GetRunningProfileControlRegistry();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return registry.DiscoverProfiles().size() == 1; }));
  const RunningProfileControlDescriptor running_profile =
      registry.DiscoverProfiles().front();
  ProfilePreferenceControlBackend* const backend =
      registry.FindBackend(running_profile.profile_identifier);
  ASSERT_TRUE(backend);
  const base::FilePath descriptor_path = runtime_directory.AppendASCII(
      running_profile.profile_identifier + "." +
      running_profile.profile_instance_identifier + ".json");
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return base::PathExists(descriptor_path); }));

  const auto run_profile = [&](std::string_view profile_identifier,
                               std::string_view instance_identifier) {
    std::vector<std::string> arguments = CliPrefix(user_data_directory);
    AppendArguments(&arguments, {"--profile", profile_identifier, "--instance",
                                 instance_identifier, "preferences", "set",
                                 "rail_density", "comfortable"});
    return RunCli(std::move(arguments));
  };

  std::string serialized_descriptor;
  ASSERT_TRUE(base::ReadFileToString(descriptor_path, &serialized_descriptor));
  std::optional<base::Value> parsed_descriptor =
      base::JSONReader::Read(serialized_descriptor, base::JSON_PARSE_RFC);
  ASSERT_TRUE(parsed_descriptor && parsed_descriptor->is_dict());
  const std::string* credential_path_value =
      parsed_descriptor->GetDict().FindString("credential_path");
  ASSERT_TRUE(credential_path_value);
  const base::FilePath credential_path(*credential_path_value);
  std::string real_capability;
  ASSERT_TRUE(base::ReadFileToString(credential_path, &real_capability));
  ASSERT_TRUE(base::WriteFile(
      credential_path, base::Uuid::GenerateRandomV4().AsLowercaseString()));
  const CliResult unauthenticated = run_profile(
      backend->profile_identifier(), backend->profile_instance_identifier());
  ASSERT_TRUE(base::WriteFile(credential_path, real_capability));
  ASSERT_TRUE(unauthenticated.launched);
  EXPECT_EQ(3, unauthenticated.exit_code) << unauthenticated.output;
  EXPECT_EQ("unauthenticated",
            FindStringOrEmpty(ParseCliResult(unauthenticated), "status"));
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  const std::string stale_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const AliasedDescriptor stale_descriptor =
      WriteAliasedDescriptor(descriptor_path, runtime_directory,
                             backend->profile_identifier(), stale_instance);
  ASSERT_FALSE(stale_descriptor.descriptor.empty());
  const CliResult stale =
      run_profile(backend->profile_identifier(), stale_instance);
  ASSERT_TRUE(stale.launched);
  EXPECT_EQ(3, stale.exit_code) << stale.output;
  EXPECT_EQ("stale_profile",
            FindStringOrEmpty(ParseCliResult(stale), "status"));

  const std::string foreign_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const AliasedDescriptor foreign_descriptor = WriteAliasedDescriptor(
      descriptor_path, runtime_directory, foreign_profile,
      backend->profile_instance_identifier());
  ASSERT_FALSE(foreign_descriptor.descriptor.empty());
  const CliResult foreign =
      run_profile(foreign_profile, backend->profile_instance_identifier());
  ASSERT_TRUE(foreign.launched);
  EXPECT_EQ(3, foreign.exit_code) << foreign.output;
  EXPECT_EQ("profile_mismatch",
            FindStringOrEmpty(ParseCliResult(foreign), "status"));
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  DeleteAlias(stale_descriptor);
  DeleteAlias(foreign_descriptor);
  Browser* const private_browser = CreateIncognitoBrowser(profile);
  ASSERT_TRUE(private_browser);
  EXPECT_TRUE(private_browser->GetProfile()->IsOffTheRecord());
  std::vector<std::string> discover = CliPrefix(user_data_directory);
  AppendArguments(&discover, {"discover"});
  const CliResult after_private = RunCli(std::move(discover));
  ASSERT_EQ(0, after_private.exit_code) << after_private.output;
  const base::DictValue after_private_result = ParseCliResult(after_private);
  const base::ListValue* profiles = after_private_result.FindList("profiles");
  ASSERT_TRUE(profiles);
  EXPECT_EQ(1u, profiles->size());
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));
}

}  // namespace
}  // namespace limitless
