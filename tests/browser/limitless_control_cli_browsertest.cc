// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "shell/control_plane/limitless_preference_control_protocol.h"
#include "shell/control_plane/limitless_preference_control_transport.h"
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

CliResult RunDiscovery(const base::FilePath& user_data_directory) {
  std::vector<std::string> arguments = CliPrefix(user_data_directory);
  AppendArguments(&arguments, {"discover"});
  return RunCli(std::move(arguments));
}

base::FilePath DescriptorPath(const base::FilePath& runtime_directory,
                              std::string_view profile_identifier,
                              std::string_view instance_identifier) {
  return runtime_directory.AppendASCII(std::string(profile_identifier) + "." +
                                       std::string(instance_identifier) +
                                       ".json");
}

base::DictValue MakeTestDescriptor(std::string profile_identifier,
                                   std::string instance_identifier) {
  base::DictValue descriptor;
  descriptor.Set("version", 1);
  descriptor.Set("profile_id", std::move(profile_identifier));
  descriptor.Set("instance_id", std::move(instance_identifier));
  descriptor.Set("socket_path", "/invalid-unless-rejected-first");
  descriptor.Set("credential_path", "/invalid-unless-rejected-first");
  descriptor.Set("pid", static_cast<int>(base::GetCurrentProcId()));
  return descriptor;
}

bool WritePrivateTestFile(const base::FilePath& path,
                          std::string_view contents) {
  return base::WriteFile(path, contents) &&
         base::SetPosixFilePermissions(path, 0600);
}

bool WritePrivateTestDescriptor(const base::FilePath& path,
                                const base::DictValue& descriptor) {
  std::string serialized;
  return base::JSONWriter::Write(descriptor, &serialized) &&
         WritePrivateTestFile(path, serialized);
}

bool ReadExactly(int descriptor, void* output, size_t length) {
  char* cursor = static_cast<char*>(output);
  while (length > 0) {
    const ssize_t count = read(descriptor, cursor, length);
    if (count <= 0) {
      return false;
    }
    cursor += count;
    length -= static_cast<size_t>(count);
  }
  return true;
}

bool WriteExactly(int descriptor, const void* input, size_t length) {
  const char* cursor = static_cast<const char*>(input);
  while (length > 0) {
    const ssize_t count = send(descriptor, cursor, length, MSG_NOSIGNAL);
    if (count <= 0) {
      return false;
    }
    cursor += count;
    length -= static_cast<size_t>(count);
  }
  return true;
}

std::string RawPreferenceWriteBlocking(base::FilePath socket_path,
                                       std::string profile_identifier,
                                       std::string instance_identifier,
                                       std::string capability) {
  base::ScopedFD connection(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!connection.is_valid()) {
    return {};
  }
  const timeval timeout = {.tv_sec = 3, .tv_usec = 0};
  if (setsockopt(connection.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0 ||
      setsockopt(connection.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    return {};
  }
  sockaddr_un address = {};
  if (socket_path.value().size() >= sizeof(address.sun_path)) {
    return {};
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.value().c_str(),
              socket_path.value().size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.value().size() + 1);
  if (connect(connection.get(), reinterpret_cast<sockaddr*>(&address),
              address_length) != 0) {
    return {};
  }

  base::DictValue request;
  request.Set("version", 1);
  request.Set("request_id", base::Uuid::GenerateRandomV4().AsLowercaseString());
  request.Set("operation", "write");
  base::DictValue target;
  target.Set("profile_id", profile_identifier);
  target.Set("instance_id", instance_identifier);
  request.Set("target", std::move(target));
  base::DictValue actor;
  actor.Set("type", "cli");
  actor.Set("id", "limitless-control-browser-test");
  request.Set("actor", std::move(actor));
  base::DictValue authentication;
  authentication.Set("scheme", "capability");
  authentication.Set("credential", capability);
  request.Set("authentication", std::move(authentication));
  request.Set("key", "rail_density");
  request.Set("value", "comfortable");
  std::string serialized;
  if (!base::JSONWriter::Write(request, &serialized)) {
    return {};
  }
  const uint32_t network_size = htonl(static_cast<uint32_t>(serialized.size()));
  if (!WriteExactly(connection.get(), &network_size, sizeof(network_size)) ||
      !WriteExactly(connection.get(), serialized.data(), serialized.size())) {
    return {};
  }
  uint32_t response_size = 0;
  if (!ReadExactly(connection.get(), &response_size, sizeof(response_size))) {
    return {};
  }
  response_size = ntohl(response_size);
  if (response_size == 0 ||
      response_size > kMaximumPreferenceTransportResponseBytes) {
    return {};
  }
  std::string response(response_size, '\0');
  return ReadExactly(connection.get(), response.data(), response.size())
             ? response
             : std::string();
}

std::string RawPreferenceWrite(base::FilePath socket_path,
                               std::string profile_identifier,
                               std::string instance_identifier,
                               std::string capability) {
  base::test::TestFuture<std::string> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&RawPreferenceWriteBlocking, std::move(socket_path),
                     std::move(profile_identifier),
                     std::move(instance_identifier), std::move(capability)),
      result.GetCallback());
  return result.Take();
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
  base::ScopedAllowBlockingForTesting allow_blocking;
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

  std::string serialized_descriptor;
  ASSERT_TRUE(base::ReadFileToString(descriptor_path, &serialized_descriptor));
  std::optional<base::Value> parsed_descriptor =
      base::JSONReader::Read(serialized_descriptor, base::JSON_PARSE_RFC);
  ASSERT_TRUE(parsed_descriptor && parsed_descriptor->is_dict());
  const std::string* socket_path_value =
      parsed_descriptor->GetDict().FindString("socket_path");
  ASSERT_TRUE(socket_path_value);
  const base::FilePath socket_path(*socket_path_value);
  const base::FilePath expected_socket_parent =
      base::FilePath(FILE_PATH_LITERAL("/tmp"))
          .AppendASCII("limitless-" + std::to_string(geteuid()));
  const base::FilePath expected_socket = expected_socket_parent.AppendASCII(
      "s-" + running_profile.profile_identifier.substr(0, 12) + "-" +
      running_profile.profile_instance_identifier);
  EXPECT_EQ(expected_socket, socket_path);

  struct stat socket_parent_metadata = {};
  ASSERT_EQ(
      0, stat(expected_socket_parent.value().c_str(), &socket_parent_metadata));
  EXPECT_TRUE(S_ISDIR(socket_parent_metadata.st_mode));
  EXPECT_EQ(geteuid(), socket_parent_metadata.st_uid);
  EXPECT_EQ(0700, socket_parent_metadata.st_mode & 0777);
  struct stat socket_metadata = {};
  ASSERT_EQ(0, stat(socket_path.value().c_str(), &socket_metadata));
  EXPECT_TRUE(S_ISSOCK(socket_metadata.st_mode));
  EXPECT_EQ(geteuid(), socket_metadata.st_uid);
  EXPECT_EQ(0600, socket_metadata.st_mode & 0777);

  const CliResult discovered = RunDiscovery(user_data_directory);
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
  base::ScopedAllowBlockingForTesting allow_blocking;
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
  const std::string* socket_path_value =
      parsed_descriptor->GetDict().FindString("socket_path");
  ASSERT_TRUE(socket_path_value);
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
  const CliResult stale_cli =
      run_profile(backend->profile_identifier(), stale_instance);
  ASSERT_TRUE(stale_cli.launched);
  EXPECT_EQ(2, stale_cli.exit_code) << stale_cli.output;
  const base::DictValue stale_cli_result = ParseCliResult(stale_cli);
  EXPECT_EQ("error", FindStringOrEmpty(stale_cli_result, "status"));
  EXPECT_EQ("profile_unavailable",
            FindStringOrEmpty(stale_cli_result, "error"));

  // The CLI rejects an undiscoverable stale instance locally. Exercise the
  // authenticated wire boundary directly to prove that the running browser
  // independently rejects the same stale target.
  const CliResult stale{
      .launched = true,
      .exit_code = 3,
      .output = RawPreferenceWrite(base::FilePath(*socket_path_value),
                                   std::string(backend->profile_identifier()),
                                   stale_instance,
                                   std::string(backend->capability())),
  };
  ASSERT_FALSE(stale.output.empty());
  EXPECT_EQ("stale_profile",
            FindStringOrEmpty(ParseCliResult(stale), "status"));

  const std::string foreign_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const CliResult foreign_cli =
      run_profile(foreign_profile, backend->profile_instance_identifier());
  ASSERT_TRUE(foreign_cli.launched);
  EXPECT_EQ(2, foreign_cli.exit_code) << foreign_cli.output;
  const base::DictValue foreign_cli_result = ParseCliResult(foreign_cli);
  EXPECT_EQ("error", FindStringOrEmpty(foreign_cli_result, "status"));
  EXPECT_EQ("profile_unavailable",
            FindStringOrEmpty(foreign_cli_result, "error"));

  const CliResult foreign{
      .launched = true,
      .exit_code = 3,
      .output = RawPreferenceWrite(
          base::FilePath(*socket_path_value), foreign_profile,
          std::string(backend->profile_instance_identifier()),
          std::string(backend->capability())),
  };
  ASSERT_FALSE(foreign.output.empty());
  EXPECT_EQ("profile_mismatch",
            FindStringOrEmpty(ParseCliResult(foreign), "status"));
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  Browser* const private_browser = CreateIncognitoBrowser(profile);
  ASSERT_TRUE(private_browser);
  EXPECT_TRUE(private_browser->GetProfile()->IsOffTheRecord());
  private_browser->GetWindow()->Show();
  BrowserView* const private_browser_view =
      BrowserView::GetBrowserViewForBrowser(private_browser);
  ASSERT_TRUE(private_browser_view);
  views::View* const private_rail =
      private_browser_view->limitless_swiftui_rail_for_testing();
  ASSERT_TRUE(private_rail);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = GetSwiftUITabRailSnapshotForTesting(private_rail);
    return snapshot && snapshot->tab_identifiers.size() == 1 &&
           snapshot->active_tab_identifier.has_value();
  }));
  const CliResult after_private = RunDiscovery(user_data_directory);
  ASSERT_EQ(0, after_private.exit_code) << after_private.output;
  const base::DictValue after_private_result = ParseCliResult(after_private);
  const base::ListValue* profiles = after_private_result.FindList("profiles");
  ASSERT_TRUE(profiles);
  EXPECT_EQ(1u, profiles->size());
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));
  CloseBrowserSynchronously(private_browser);
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportBrowserTest,
                       CliHardensDiscoveryAndRecoversAfterUnsafeEntry) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  Profile* const profile = browser()->GetProfile();
  ASSERT_TRUE(profile);
  const base::FilePath user_data_directory = profile->GetPath().DirName();
  const base::FilePath runtime_directory =
      user_data_directory.AppendASCII(kPreferenceControlRuntimeDirectory);
  RunningProfileControlRegistry& registry = GetRunningProfileControlRegistry();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return registry.DiscoverProfiles().size() == 1; }));
  const RunningProfileControlDescriptor running_profile =
      registry.DiscoverProfiles().front();
  const base::FilePath live_descriptor =
      DescriptorPath(runtime_directory, running_profile.profile_identifier,
                     running_profile.profile_instance_identifier);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return base::PathExists(live_descriptor); }));

  std::vector<base::FilePath> test_artifacts;
  base::ScopedClosureRunner cleanup_test_artifacts(base::BindOnce(
      [](std::vector<base::FilePath>* paths) {
        for (const base::FilePath& path : *paths) {
          base::DeleteFile(path);
        }
      },
      &test_artifacts));

  const std::string null_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string null_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue null_descriptor =
      MakeTestDescriptor(null_profile, null_instance);
  null_descriptor.Set("socket_path", base::Value());
  const base::FilePath null_path =
      DescriptorPath(runtime_directory, null_profile, null_instance);
  ASSERT_TRUE(WritePrivateTestDescriptor(null_path, null_descriptor));
  test_artifacts.push_back(null_path);

  const std::string nul_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string nul_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue nul_descriptor =
      MakeTestDescriptor(nul_profile, nul_instance);
  std::string embedded_nul_path = "embedded";
  embedded_nul_path.push_back('\0');
  embedded_nul_path.append("path");
  nul_descriptor.Set("credential_path", std::move(embedded_nul_path));
  const base::FilePath nul_path =
      DescriptorPath(runtime_directory, nul_profile, nul_instance);
  ASSERT_TRUE(WritePrivateTestDescriptor(nul_path, nul_descriptor));
  test_artifacts.push_back(nul_path);

  const std::string huge_pid_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string huge_pid_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue huge_pid_descriptor =
      MakeTestDescriptor(huge_pid_profile, huge_pid_instance);
  std::string huge_pid_serialized;
  ASSERT_TRUE(
      base::JSONWriter::Write(huge_pid_descriptor, &huge_pid_serialized));
  const std::string kPidCurrent =
      "\"pid\":" + base::NumberToString(base::GetCurrentProcId());
  const size_t pid_position = huge_pid_serialized.find(kPidCurrent);
  ASSERT_NE(std::string::npos, pid_position);
  huge_pid_serialized.replace(pid_position, kPidCurrent.size(),
                              "\"pid\":2147483648");
  const base::FilePath huge_pid_path =
      DescriptorPath(runtime_directory, huge_pid_profile, huge_pid_instance);
  ASSERT_TRUE(WritePrivateTestFile(huge_pid_path, huge_pid_serialized));
  test_artifacts.push_back(huge_pid_path);

  const std::string bool_version_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string bool_version_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue bool_version_descriptor =
      MakeTestDescriptor(bool_version_profile, bool_version_instance);
  bool_version_descriptor.Set("version", true);
  const base::FilePath bool_version_path = DescriptorPath(
      runtime_directory, bool_version_profile, bool_version_instance);
  ASSERT_TRUE(
      WritePrivateTestDescriptor(bool_version_path, bool_version_descriptor));
  test_artifacts.push_back(bool_version_path);

  const std::string recursive_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string recursive_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const base::FilePath recursive_path =
      DescriptorPath(runtime_directory, recursive_profile, recursive_instance);
  ASSERT_TRUE(WritePrivateTestFile(
      recursive_path, std::string(2000, '[') + "0" + std::string(2000, ']')));
  test_artifacts.push_back(recursive_path);

  const std::string future_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string future_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  base::DictValue future_descriptor =
      MakeTestDescriptor(future_profile, future_instance);
  future_descriptor.Set("version", 2);
  const base::FilePath future_path =
      DescriptorPath(runtime_directory, future_profile, future_instance);
  ASSERT_TRUE(WritePrivateTestDescriptor(future_path, future_descriptor));
  test_artifacts.push_back(future_path);

  const CliResult mixed_discovery = RunDiscovery(user_data_directory);
  ASSERT_TRUE(mixed_discovery.launched);
  ASSERT_EQ(0, mixed_discovery.exit_code) << mixed_discovery.output;
  EXPECT_EQ(std::string::npos, mixed_discovery.output.find("Traceback"));
  const base::DictValue mixed_result = ParseCliResult(mixed_discovery);
  const base::ListValue* mixed_profiles = mixed_result.FindList("profiles");
  ASSERT_TRUE(mixed_profiles);
  ASSERT_EQ(1u, mixed_profiles->size());
  EXPECT_EQ(running_profile.profile_identifier,
            FindStringOrEmpty(mixed_profiles->front().GetDict(), "profile_id"));
  EXPECT_EQ(
      running_profile.profile_instance_identifier,
      FindStringOrEmpty(mixed_profiles->front().GetDict(), "instance_id"));

  const std::string unsafe_profile =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string unsafe_instance =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const base::FilePath unsafe_path =
      DescriptorPath(runtime_directory, unsafe_profile, unsafe_instance);
  ASSERT_TRUE(base::CreateSymbolicLink(live_descriptor, unsafe_path));
  test_artifacts.push_back(unsafe_path);

  const CliResult unsafe_discovery = RunDiscovery(user_data_directory);
  ASSERT_TRUE(unsafe_discovery.launched);
  EXPECT_EQ(2, unsafe_discovery.exit_code) << unsafe_discovery.output;
  const base::DictValue unsafe_result = ParseCliResult(unsafe_discovery);
  EXPECT_EQ("error", FindStringOrEmpty(unsafe_result, "status"));
  EXPECT_EQ("unsafe_discovery_entry",
            FindStringOrEmpty(unsafe_result, "error"));
  EXPECT_EQ(kCompactRailDensity,
            profile->GetPrefs()->GetString(kRailDensityPref));

  ASSERT_TRUE(base::DeleteFile(unsafe_path));
  const CliResult recovered_discovery = RunDiscovery(user_data_directory);
  ASSERT_TRUE(recovered_discovery.launched);
  ASSERT_EQ(0, recovered_discovery.exit_code) << recovered_discovery.output;
  const base::DictValue recovered_result = ParseCliResult(recovered_discovery);
  const base::ListValue* recovered_profiles =
      recovered_result.FindList("profiles");
  ASSERT_TRUE(recovered_profiles);
  ASSERT_EQ(1u, recovered_profiles->size());
  EXPECT_EQ(
      running_profile.profile_identifier,
      FindStringOrEmpty(recovered_profiles->front().GetDict(), "profile_id"));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportBrowserTest,
                       CliRequiresExactTargetForMultipleRegularProfiles) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  Profile* const original_profile = browser()->GetProfile();
  ASSERT_TRUE(original_profile);
  const base::FilePath user_data_directory =
      original_profile->GetPath().DirName();
  RunningProfileControlRegistry& registry = GetRunningProfileControlRegistry();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return registry.DiscoverProfiles().size() == 1; }));
  const RunningProfileControlDescriptor original_running_profile =
      registry.DiscoverProfiles().front();

  ProfileManager* const profile_manager = g_browser_process->profile_manager();
  ASSERT_TRUE(profile_manager);
  Profile* const other_profile = &profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  Browser* const other_browser = CreateBrowser(other_profile);
  ASSERT_TRUE(other_browser);
  other_browser->GetWindow()->Show();
  BrowserView* const other_browser_view =
      BrowserView::GetBrowserViewForBrowser(other_browser);
  ASSERT_TRUE(other_browser_view);
  views::View* const other_rail =
      other_browser_view->limitless_swiftui_rail_for_testing();
  ASSERT_TRUE(other_rail);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    const auto snapshot = GetSwiftUITabRailSnapshotForTesting(other_rail);
    return snapshot && snapshot->tab_identifiers.size() == 1 &&
           snapshot->active_tab_identifier.has_value();
  }));
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return registry.DiscoverProfiles().size() == 2; }));

  const CliResult discovered = RunDiscovery(user_data_directory);
  ASSERT_TRUE(discovered.launched);
  ASSERT_EQ(0, discovered.exit_code) << discovered.output;
  const base::DictValue discovery = ParseCliResult(discovered);
  const base::ListValue* profiles = discovery.FindList("profiles");
  ASSERT_TRUE(profiles);
  EXPECT_EQ(2u, profiles->size());

  std::vector<std::string> ambiguous = CliPrefix(user_data_directory);
  AppendArguments(&ambiguous, {"preferences", "get", "rail_density"});
  const CliResult ambiguous_result = RunCli(std::move(ambiguous));
  ASSERT_TRUE(ambiguous_result.launched);
  EXPECT_EQ(2, ambiguous_result.exit_code) << ambiguous_result.output;
  const base::DictValue ambiguous_response = ParseCliResult(ambiguous_result);
  EXPECT_EQ("error", FindStringOrEmpty(ambiguous_response, "status"));
  EXPECT_EQ("ambiguous_profile",
            FindStringOrEmpty(ambiguous_response, "error"));

  std::optional<RunningProfileControlDescriptor> other_running_profile;
  for (const RunningProfileControlDescriptor& candidate :
       registry.DiscoverProfiles()) {
    if (candidate.profile_identifier !=
        original_running_profile.profile_identifier) {
      other_running_profile = candidate;
      break;
    }
  }
  ASSERT_TRUE(other_running_profile);
  std::vector<std::string> exact = CliPrefix(user_data_directory);
  AppendArguments(
      &exact, {"--profile", other_running_profile->profile_identifier,
               "--instance", other_running_profile->profile_instance_identifier,
               "preferences", "get", "rail_density"});
  const CliResult exact_result = RunCli(std::move(exact));
  ASSERT_TRUE(exact_result.launched);
  ASSERT_EQ(0, exact_result.exit_code) << exact_result.output;
  const base::DictValue exact_response = ParseCliResult(exact_result);
  EXPECT_EQ("success", FindStringOrEmpty(exact_response, "status"));
  const base::DictValue* effective = exact_response.FindDict("effective");
  ASSERT_TRUE(effective);
  EXPECT_EQ("compact", FindStringOrEmpty(*effective, "value"));
  EXPECT_EQ(kCompactRailDensity,
            original_profile->GetPrefs()->GetString(kRailDensityPref));
  EXPECT_EQ(kCompactRailDensity,
            other_profile->GetPrefs()->GetString(kRailDensityPref));

  CloseBrowserSynchronously(other_browser);
  EXPECT_TRUE(base::test::RunUntil(
      [&]() { return registry.DiscoverProfiles().size() == 1; }));
}

}  // namespace
}  // namespace limitless
