// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/function_ref.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "shell/agents/agent_presence_registry.h"
#include "shell/agents/herdr_agent_provider.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

constexpr char kReadySnapshot[] = R"json({
  "id":"cli:api:snapshot",
  "result":{
    "type":"session_snapshot",
    "snapshot":{
      "protocol":20,
      "version":"0.8.2",
      "workspaces":[
        {"workspace_id":"w1","label":"Alpha"},
        {"workspace_id":"w2","label":""}
      ],
      "agents":[
        {"pane_id":"w1:p1","workspace_id":"w1","agent":"co\u061C\u200E\u202Ed\u200Fex","agent_status":"idle","terminal_title_stripped":"Fi\u061C\u200E\u202Er\u200Fst","focused":false},
        {"pane_id":"w2:p2","workspace_id":"w2","agent":"claude","agent_status":"working","terminal_title_stripped":"","focused":true},
        {"pane_id":"w1:p3","workspace_id":"w1","agent":null,"display_agent":"gemini","agent_status":"blocked","terminal_title_stripped":null,"focused":false},
        {"pane_id":"w3:p4","workspace_id":"w3","name":"custom","agent_status":"done","focused":false},
        {"pane_id":"w3:p5","workspace_id":"w3","agent":null,"agent_status":"unknown","terminal_title_stripped":"   ","focused":false}
      ]
    }
  }
})json";

std::string SnapshotWithOneAgent(std::string_view status,
                                 bool focused = false) {
  return std::string(
             R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","workspaces":[{"workspace_id":"w1","label":"Workspace One"}],"agents":[{"pane_id":"w1:p1","workspace_id":"w1","agent":"codex","agent_status":")json") +
         std::string(status) +
         R"json(","terminal_title_stripped":"Agent One","focused":)json" +
         (focused ? "true" : "false") + "}]}}}";
}

void WriteFileOrDie(const base::FilePath& path, std::string_view contents) {
  CHECK(base::WriteFile(path, contents));
}

void WriteExecutableOrDie(const base::FilePath& path,
                          std::string_view contents) {
  WriteFileOrDie(path, contents);
  CHECK(base::SetPosixFilePermissions(path, 0700));
}

size_t CountLines(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return 0;
  }
  return static_cast<size_t>(std::ranges::count(contents, '\n'));
}

size_t FileByteCount(const base::FilePath& path) {
  const std::optional<int64_t> size = base::GetFileSize(path);
  return size && *size > 0 ? static_cast<size_t>(*size) : 0;
}

bool ProcessIsGone(pid_t process_id) {
  errno = 0;
  return HANDLE_EINTR(kill(process_id, 0)) == -1 && errno == ESRCH;
}

std::vector<pid_t> ReadProcessIds(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return {};
  }
  std::vector<pid_t> process_ids;
  for (const std::string& line : base::SplitString(
           contents, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    int process_id = -1;
    if (base::StringToInt(line, &process_id) && process_id > 0) {
      process_ids.push_back(static_cast<pid_t>(process_id));
    }
  }
  std::ranges::sort(process_ids);
  process_ids.erase(std::ranges::unique(process_ids).begin(),
                    process_ids.end());
  return process_ids;
}

pid_t ReadProcessId(const base::FilePath& path) {
  std::vector<pid_t> process_ids = ReadProcessIds(path);
  return process_ids.empty() ? -1 : process_ids.back();
}

bool ReleaseFifo(const base::FilePath& path) {
  const base::TimeTicks deadline = base::TimeTicks::Now() + base::Seconds(2);
  int descriptor = -1;
  do {
    descriptor = HANDLE_EINTR(
        open(path.value().c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC));
    if (descriptor >= 0) {
      break;
    }
    if (errno != ENXIO) {
      return false;
    }
    base::PlatformThread::Sleep(base::Milliseconds(5));
  } while (base::TimeTicks::Now() < deadline);
  if (descriptor < 0) {
    return false;
  }
  constexpr char kRelease[] = "release\n";
  const bool wrote_release =
      HANDLE_EINTR(write(descriptor, kRelease, sizeof(kRelease) - 1)) ==
      static_cast<ssize_t>(sizeof(kRelease) - 1);
  return HANDLE_EINTR(close(descriptor)) == 0 && wrote_release;
}

bool WaitForCondition(base::FunctionRef<bool()> condition,
                      base::TimeDelta timeout = base::Seconds(2)) {
  if (condition()) {
    return true;
  }
  base::RunLoop run_loop;
  base::RepeatingTimer poll_timer;
  poll_timer.Start(FROM_HERE, base::Milliseconds(5),
                   base::BindLambdaForTesting([&]() {
                     if (condition()) {
                       run_loop.Quit();
                     }
                   }));
  base::OneShotTimer timeout_timer;
  timeout_timer.Start(FROM_HERE, timeout, run_loop.QuitClosure());
  run_loop.Run();
  return condition();
}

class ScopedHelperProcessCleanup {
 public:
  explicit ScopedHelperProcessCleanup(
      base::FilePath process_id_path,
      std::optional<base::FilePath> group_member_id_path = std::nullopt)
      : process_id_path_(std::move(process_id_path)),
        group_member_id_path_(std::move(group_member_id_path)) {}

  ScopedHelperProcessCleanup(const ScopedHelperProcessCleanup&) = delete;
  ScopedHelperProcessCleanup& operator=(const ScopedHelperProcessCleanup&) =
      delete;

  ~ScopedHelperProcessCleanup() {
    const std::vector<pid_t> process_ids = ReadProcessIds(process_id_path_);
    const std::vector<pid_t> group_member_ids =
        group_member_id_path_ ? ReadProcessIds(*group_member_id_path_)
                              : std::vector<pid_t>();
    const bool has_live_group_member = std::ranges::any_of(
        group_member_ids,
        [](pid_t process_id) { return !ProcessIsGone(process_id); });
    for (pid_t process_id : process_ids) {
      if (!ProcessIsGone(process_id) || has_live_group_member) {
        HANDLE_EINTR(kill(-process_id, SIGKILL));
      }
    }
    const base::TimeTicks deadline = base::TimeTicks::Now() + base::Seconds(2);
    std::vector<pid_t> processes_to_wait_for = process_ids;
    processes_to_wait_for.insert(processes_to_wait_for.end(),
                                 group_member_ids.begin(),
                                 group_member_ids.end());
    for (pid_t process_id : processes_to_wait_for) {
      while (!ProcessIsGone(process_id) && base::TimeTicks::Now() < deadline) {
        base::PlatformThread::Sleep(base::Milliseconds(5));
      }
    }
  }

 private:
  const base::FilePath process_id_path_;
  const std::optional<base::FilePath> group_member_id_path_;
};

int CheckShellSyntax(const base::FilePath& path) {
  base::Process process =
      base::LaunchProcess({"/bin/sh", "-n", path.value()}, {});
  if (!process.IsValid()) {
    return -1;
  }
  int exit_code = -1;
  if (!process.WaitForExitWithTimeout(base::Seconds(5), &exit_code)) {
    base::EnsureProcessTerminated(std::move(process));
    return -1;
  }
  return exit_code;
}

TEST(HerdrAgentProviderTest, ParsesWholeSessionWithFriendlyWorkspaceLabels) {
  HerdrAgentSnapshot snapshot =
      ParseHerdrAgentSnapshotForTesting(kReadySnapshot);

  ASSERT_EQ(HerdrAgentSourceState::kReady, snapshot.state);
  EXPECT_EQ("0.8.2", snapshot.version);
  EXPECT_EQ(kSupportedHerdrProtocol, snapshot.protocol);
  ASSERT_EQ(5u, snapshot.agents.size());
  EXPECT_EQ(
      (std::vector<std::string>{"w1:p1", "w2:p2", "w1:p3", "w3:p4", "w3:p5"}),
      (std::vector<std::string>{
          snapshot.agents[0].pane_id, snapshot.agents[1].pane_id,
          snapshot.agents[2].pane_id, snapshot.agents[3].pane_id,
          snapshot.agents[4].pane_id}));
  EXPECT_EQ(u"Alpha", snapshot.agents[0].workspace_label);
  EXPECT_EQ(u"w2", snapshot.agents[1].workspace_label);
  EXPECT_EQ(u"w3", snapshot.agents[3].workspace_label);
  EXPECT_EQ("codex", snapshot.agents[0].agent);
  EXPECT_EQ("gemini", snapshot.agents[2].agent);
  EXPECT_EQ("custom", snapshot.agents[3].agent);
  EXPECT_EQ("w3:p5", snapshot.agents[4].agent);
  EXPECT_EQ(u"First", snapshot.agents[0].terminal_title);
  EXPECT_EQ(u"claude w2:p2", snapshot.agents[1].terminal_title);
  EXPECT_EQ(u"gemini w1:p3", snapshot.agents[2].terminal_title);
  EXPECT_EQ(u"custom w3:p4", snapshot.agents[3].terminal_title);
  EXPECT_EQ(u"w3:p5 w3:p5", snapshot.agents[4].terminal_title);
  EXPECT_EQ(
      (std::vector<bool>{false, true, false, false, false}),
      (std::vector<bool>{snapshot.agents[0].focused, snapshot.agents[1].focused,
                         snapshot.agents[2].focused, snapshot.agents[3].focused,
                         snapshot.agents[4].focused}));
  EXPECT_EQ(HerdrAgentStatus::kIdle, snapshot.agents[0].status);
  EXPECT_EQ(HerdrAgentStatus::kWorking, snapshot.agents[1].status);
  EXPECT_EQ(HerdrAgentStatus::kBlocked, snapshot.agents[2].status);
  EXPECT_EQ(HerdrAgentStatus::kDone, snapshot.agents[3].status);
  EXPECT_EQ(HerdrAgentStatus::kUnknown, snapshot.agents[4].status);
}

TEST(HerdrAgentProviderTest, RejectsInvalidAndIncompatibleSnapshots) {
  const std::vector<std::pair<std::string, HerdrAgentSourceState>> cases = {
      {"not json", HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","error":{"code":"unavailable"}})json",
       HerdrAgentSourceState::kUnavailable},
      {R"json({"id":"cli:api:snapshot","error":"unavailable","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"wrong","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"wrong","snapshot":{}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":21,"version":"0.8.2","agents":[]}}})json",
       HerdrAgentSourceState::kIncompatibleProtocol},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","workspaces":{},"agents":[]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[{"pane_id":"w1:p1","workspace_id":"w1","agent":"codex","agent_status":"idle","terminal_title_stripped":7,"focused":false}]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[{"pane_id":"w1:p1","workspace_id":"w1","agent":"codex","agent_status":"idle","focused":false},{"pane_id":"w1:p1","workspace_id":"w1","agent":"codex","agent_status":"idle","focused":false}]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[{"pane_id":"unsafe/id","workspace_id":"w1","agent":"codex","agent_status":"idle","focused":false}]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[{"pane_id":"w1:p1","workspace_id":"w1","agent":"codex","agent_status":"sleeping","focused":false}]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
      {R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[{"pane_id":"w1:p1","workspace_id":"w1","agent":"codex","agent_status":"idle"}]}}})json",
       HerdrAgentSourceState::kMalformedResponse},
  };

  for (const auto& [input, expected_state] : cases) {
    EXPECT_EQ(expected_state, ParseHerdrAgentSnapshotForTesting(input).state)
        << input;
  }

  std::string oversized(1024 * 1024 + 1, 'x');
  EXPECT_EQ(HerdrAgentSourceState::kMalformedResponse,
            ParseHerdrAgentSnapshotForTesting(oversized).state);

  std::string too_many =
      R"json({"id":"cli:api:snapshot","result":{"type":"session_snapshot","snapshot":{"protocol":20,"version":"0.8.2","agents":[)json";
  for (int index = 0; index < 257; ++index) {
    if (index) {
      too_many += ',';
    }
    too_many += "{\"pane_id\":\"p" + std::to_string(index) +
                "\",\"workspace_id\":\"w1\",\"agent\":\"codex\","
                "\"agent_status\":\"idle\",\"focused\":false}";
  }
  too_many += "]}}}";
  EXPECT_EQ(HerdrAgentSourceState::kMalformedResponse,
            ParseHerdrAgentSnapshotForTesting(too_many).state);
}

class HerdrAgentProviderProcessTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  std::unique_ptr<HerdrAgentProvider> CreateProvider(
      const base::FilePath& executable,
      base::TimeDelta timeout = base::Seconds(2)) {
    return CreateHerdrCliAgentProviderForTesting(executable, base::Hours(1),
                                                 timeout);
  }

  std::vector<HerdrAgentSnapshot> StartAndWaitForState(
      HerdrAgentProvider* provider,
      HerdrAgentSourceState state) {
    std::vector<HerdrAgentSnapshot> snapshots;
    provider->Start(
        base::BindLambdaForTesting([&](const HerdrAgentSnapshot& snapshot) {
          snapshots.push_back(snapshot);
        }));
    EXPECT_TRUE(WaitForCondition(
        [&]() { return !snapshots.empty() && snapshots.back().state == state; },
        base::Seconds(5)));
    return snapshots;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::ThreadPoolExecutionMode::ASYNC};
  base::ScopedTempDir temp_dir_;
};

TEST_F(HerdrAgentProviderProcessTest,
       ExecutesExactArgvAndCoalescesOverlappingRefreshes) {
  const base::FilePath response = temp_dir_.GetPath().Append("response.json");
  const base::FilePath invocations =
      temp_dir_.GetPath().Append("invocations.txt");
  const base::FilePath completions =
      temp_dir_.GetPath().Append("completions.txt");
  const base::FilePath launches = temp_dir_.GetPath().Append("launches.txt");
  const base::FilePath helper_pid = temp_dir_.GetPath().Append("helper.pid");
  const base::FilePath overlap = temp_dir_.GetPath().Append("overlap.txt");
  const base::FilePath lock = temp_dir_.GetPath().Append("active");
  const base::FilePath release = temp_dir_.GetPath().Append("release.fifo");
  const base::FilePath executable =
      temp_dir_.GetPath().Append("herdr fake; no-shell");
  WriteFileOrDie(response, SnapshotWithOneAgent("idle"));
  ASSERT_EQ(0, mkfifo(release.value().c_str(), 0600));
  WriteExecutableOrDie(executable,
                       "#!/bin/sh\n"
                       "printf 'x\\n' >> '" +
                           launches.value() +
                           "'\n"
                           "printf '%s\\n' \"$$\" >> '" +
                           helper_pid.value() +
                           "'\n"
                           "test \"$#\" -eq 2 || exit 90\n"
                           "test \"$1\" = api || exit 91\n"
                           "test \"$2\" = snapshot || exit 92\n"
                           "if ! /bin/mkdir '" +
                           lock.value() +
                           "'; then\n"
                           "  : > '" +
                           overlap.value() +
                           "'\n"
                           "  exit 93\n"
                           "fi\n"
                           "exec 3<> '" +
                           release.value() +
                           "' || exit 94\n"
                           "printf x >> '" +
                           invocations.value() +
                           "'\n"
                           "IFS= read -r release_value <&3 || exit 95\n"
                           "exec 3>&-\n"
                           "/bin/rmdir '" +
                           lock.value() +
                           "'\n"
                           "while IFS= read -r line || test -n \"$line\"; do\n"
                           "  printf '%s\\n' \"$line\"\n"
                           "done < '" +
                           response.value() +
                           "'\n"
                           "printf x >> '" +
                           completions.value() + "'\n");
  ASSERT_EQ(0, CheckShellSyntax(executable));

  std::vector<HerdrAgentSnapshot> snapshots;
  std::unique_ptr<HerdrAgentProvider> provider =
      CreateProvider(executable, base::Seconds(10));
  ScopedHelperProcessCleanup helper_cleanup(helper_pid);
  provider->Start(
      base::BindLambdaForTesting([&](const HerdrAgentSnapshot& snapshot) {
        snapshots.push_back(snapshot);
      }));
  const bool first_invocation_started = WaitForCondition([&]() {
    return FileByteCount(invocations) == 1 ||
           (snapshots.size() > 1 &&
            snapshots.back().state != HerdrAgentSourceState::kLoading);
  });
  ASSERT_TRUE(first_invocation_started)
      << "launch count: " << CountLines(launches) << ", provider state: "
      << HerdrAgentSourceStateName(snapshots.back().state);
  ASSERT_EQ(1u, FileByteCount(invocations))
      << "launch count: " << CountLines(launches) << ", provider state: "
      << HerdrAgentSourceStateName(snapshots.back().state);
  provider->RequestRefresh();
  provider->RequestRefresh();
  provider->RequestRefresh();
  EXPECT_EQ(1u, FileByteCount(invocations));
  EXPECT_FALSE(base::PathExists(overlap));

  ASSERT_TRUE(ReleaseFifo(release));
  ASSERT_TRUE(
      WaitForCondition([&]() { return FileByteCount(invocations) == 2; }));
  EXPECT_EQ(2u, FileByteCount(invocations));
  EXPECT_FALSE(base::PathExists(overlap));
  WriteFileOrDie(response, SnapshotWithOneAgent("working"));
  ASSERT_TRUE(ReleaseFifo(release));
  const bool working_snapshot_published = WaitForCondition([&]() {
    return !snapshots.empty() && snapshots.back().agents.size() == 1 &&
           snapshots.back().agents[0].status == HerdrAgentStatus::kWorking;
  });
  ASSERT_TRUE(working_snapshot_published)
      << "callbacks: " << snapshots.size()
      << ", invocations: " << FileByteCount(invocations)
      << ", completions: " << FileByteCount(completions)
      << ", last source state: "
      << (snapshots.empty() ? "none"
                            : HerdrAgentSourceStateName(snapshots.back().state))
      << ", last agent count: "
      << (snapshots.empty() ? 0 : snapshots.back().agents.size())
      << ", last agent status: "
      << (snapshots.empty() || snapshots.back().agents.empty()
              ? -1
              : static_cast<int>(snapshots.back().agents[0].status));
  const size_t callback_count = snapshots.size();
  const HerdrAgentSnapshot last_snapshot = snapshots.back();

  provider->RequestRefresh();
  ASSERT_TRUE(
      WaitForCondition([&]() { return FileByteCount(invocations) == 3; }));
  EXPECT_FALSE(base::PathExists(overlap));
  EXPECT_EQ(callback_count, snapshots.size());
  ASSERT_TRUE(ReleaseFifo(release));
  ASSERT_TRUE(
      WaitForCondition([&]() { return FileByteCount(completions) == 3; }));
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(overlap));
  EXPECT_EQ(3u, FileByteCount(invocations));
  EXPECT_EQ(callback_count, snapshots.size());
  EXPECT_EQ(last_snapshot, snapshots.back());
  ASSERT_GE(snapshots.size(), 2u);
  EXPECT_EQ(HerdrAgentSourceState::kLoading, snapshots.front().state);
  const pid_t last_helper = ReadProcessId(helper_pid);
  ASSERT_GT(last_helper, 0);
  EXPECT_TRUE(WaitForCondition([&]() { return ProcessIsGone(last_helper); }));
  provider->Stop();
}

TEST_F(HerdrAgentProviderProcessTest,
       TimesOutAndReapsLeaderAndProcessGroupChild) {
  const base::FilePath leader_pid = temp_dir_.GetPath().Append("leader.pid");
  const base::FilePath child_pid = temp_dir_.GetPath().Append("child.pid");
  const base::FilePath executable = temp_dir_.GetPath().Append("timeout-herdr");
  WriteExecutableOrDie(executable,
                       "#!/bin/sh\n"
                       "printf '%s\\n' \"$$\" > '" +
                           leader_pid.value() +
                           "'\n"
                           "/bin/sleep 30 &\n"
                           "child=$!\n"
                           "printf '%s\\n' \"$child\" > '" +
                           child_pid.value() +
                           "'\n"
                           "wait $child\n");

  std::unique_ptr<HerdrAgentProvider> provider =
      CreateProvider(executable, base::Seconds(1));
  std::vector<HerdrAgentSnapshot> snapshots =
      StartAndWaitForState(provider.get(), HerdrAgentSourceState::kTimedOut);
  const pid_t leader = ReadProcessId(leader_pid);
  const pid_t child = ReadProcessId(child_pid);
  ASSERT_GT(leader, 0);
  ASSERT_GT(child, 0);
  EXPECT_TRUE(WaitForCondition(
      [&]() { return ProcessIsGone(leader) && ProcessIsGone(child); }));
  int status = 0;
  errno = 0;
  EXPECT_EQ(-1, HANDLE_EINTR(waitpid(leader, &status, WNOHANG)));
  EXPECT_EQ(ECHILD, errno);
  provider->Stop();
}

TEST_F(HerdrAgentProviderProcessTest,
       ReapsResidualProcessGroupChildrenAfterLeaderExit) {
  const base::FilePath response = temp_dir_.GetPath().Append("response.json");
  WriteFileOrDie(response, SnapshotWithOneAgent("idle"));

  for (const auto& [name, exit_code, expected_state] :
       std::vector<std::tuple<std::string, int, HerdrAgentSourceState>>{
           {"normal", 0, HerdrAgentSourceState::kReady},
           {"nonzero", 7, HerdrAgentSourceState::kUnavailable},
       }) {
    const base::FilePath leader_pid =
        temp_dir_.GetPath().Append(name + "-leader.pid");
    const base::FilePath child_pid =
        temp_dir_.GetPath().Append(name + "-child.pid");
    const base::FilePath executable =
        temp_dir_.GetPath().Append(name + "-residual-herdr");
    WriteExecutableOrDie(
        executable,
        "#!/bin/sh\n"
        "printf '%s\\n' \"$$\" > '" +
            leader_pid.value() +
            "'\n"
            "/bin/sleep 30 </dev/null >/dev/null 2>&1 &\n"
            "child=$!\n"
            "printf '%s\\n' \"$child\" > '" +
            child_pid.value() +
            "'\n"
            "while IFS= read -r line || test -n \"$line\"; do\n"
            "  printf '%s\\n' \"$line\"\n"
            "done < '" +
            response.value() +
            "'\n"
            "exit " +
            std::to_string(exit_code) + "\n");

    ScopedHelperProcessCleanup residual_cleanup(leader_pid, child_pid);
    std::unique_ptr<HerdrAgentProvider> provider = CreateProvider(executable);
    StartAndWaitForState(provider.get(), expected_state);
    const pid_t leader = ReadProcessId(leader_pid);
    const pid_t child = ReadProcessId(child_pid);
    ASSERT_GT(leader, 0);
    ASSERT_GT(child, 0);
    EXPECT_TRUE(WaitForCondition(
        [&]() { return ProcessIsGone(leader) && ProcessIsGone(child); }));
    int status = 0;
    errno = 0;
    EXPECT_EQ(-1, HANDLE_EINTR(waitpid(leader, &status, WNOHANG)));
    EXPECT_EQ(ECHILD, errno);
    provider->Stop();
  }
}

TEST_F(HerdrAgentProviderProcessTest,
       RejectsOversizeAndNonzeroExitWithoutOrphans) {
  const base::FilePath oversized_pid =
      temp_dir_.GetPath().Append("oversized.pid");
  const base::FilePath oversized =
      temp_dir_.GetPath().Append("oversized-herdr");
  WriteExecutableOrDie(oversized,
                       "#!/bin/sh\n"
                       "/bin/echo $$ > '" +
                           oversized_pid.value() +
                           "'\n"
                           "/usr/bin/yes x | /usr/bin/head -c 1048577\n");
  std::unique_ptr<HerdrAgentProvider> oversized_provider =
      CreateProvider(oversized);
  StartAndWaitForState(oversized_provider.get(),
                       HerdrAgentSourceState::kMalformedResponse);
  const pid_t oversized_process = ReadProcessId(oversized_pid);
  ASSERT_GT(oversized_process, 0);
  EXPECT_TRUE(
      WaitForCondition([&]() { return ProcessIsGone(oversized_process); }));
  oversized_provider->Stop();

  const base::FilePath exit_pid = temp_dir_.GetPath().Append("exit.pid");
  const base::FilePath nonzero = temp_dir_.GetPath().Append("nonzero-herdr");
  WriteExecutableOrDie(nonzero, "#!/bin/sh\n/bin/echo $$ > '" +
                                    exit_pid.value() + "'\nexit 7\n");
  std::unique_ptr<HerdrAgentProvider> nonzero_provider =
      CreateProvider(nonzero);
  StartAndWaitForState(nonzero_provider.get(),
                       HerdrAgentSourceState::kUnavailable);
  const pid_t exited_process = ReadProcessId(exit_pid);
  ASSERT_GT(exited_process, 0);
  EXPECT_TRUE(
      WaitForCondition([&]() { return ProcessIsGone(exited_process); }));
  nonzero_provider->Stop();
}

TEST_F(HerdrAgentProviderProcessTest,
       RejectsMissingNonExecutableAndNonRegularPaths) {
  const base::FilePath missing = temp_dir_.GetPath().Append("missing");
  const base::FilePath non_executable =
      temp_dir_.GetPath().Append("non-executable");
  WriteFileOrDie(non_executable, "#!/bin/sh\nexit 0\n");
  ASSERT_TRUE(base::SetPosixFilePermissions(non_executable, 0600));

  for (const auto& [path, expected_state] :
       std::vector<std::pair<base::FilePath, HerdrAgentSourceState>>{
           {missing, HerdrAgentSourceState::kMissingExecutable},
           {non_executable, HerdrAgentSourceState::kPermissionDenied},
           {temp_dir_.GetPath(), HerdrAgentSourceState::kPermissionDenied},
       }) {
    std::unique_ptr<HerdrAgentProvider> provider = CreateProvider(path);
    std::vector<HerdrAgentSnapshot> snapshots =
        StartAndWaitForState(provider.get(), expected_state);
    ASSERT_EQ(2u, snapshots.size());
    EXPECT_EQ(HerdrAgentSourceState::kLoading, snapshots[0].state);
    provider->Stop();
  }
}

TEST_F(HerdrAgentProviderProcessTest, StopRejectsLateRepliesAcrossRestart) {
  const base::FilePath response = temp_dir_.GetPath().Append("response.json");
  const base::FilePath invocations = temp_dir_.GetPath().Append("calls.txt");
  const base::FilePath helper_pid = temp_dir_.GetPath().Append("restart.pid");
  const base::FilePath hold = temp_dir_.GetPath().Append("hold");
  const base::FilePath executable = temp_dir_.GetPath().Append("restart-herdr");
  WriteFileOrDie(response, SnapshotWithOneAgent("idle"));
  WriteFileOrDie(hold, "hold");
  WriteExecutableOrDie(executable,
                       "#!/bin/sh\n"
                       "printf '%s\\n' \"$$\" >> '" +
                           helper_pid.value() +
                           "'\n"
                           "printf 'x\\n' >> '" +
                           invocations.value() +
                           "'\n"
                           "while test -f '" +
                           hold.value() +
                           "'; do /bin/sleep 0.01; done\n"
                           "while IFS= read -r line || test -n \"$line\"; do\n"
                           "  printf '%s\\n' \"$line\"\n"
                           "done < '" +
                           response.value() + "'\n");

  std::vector<HerdrAgentSnapshot> old_snapshots;
  std::vector<HerdrAgentSnapshot> new_snapshots;
  std::unique_ptr<HerdrAgentProvider> provider =
      CreateProvider(executable, base::Seconds(10));
  ScopedHelperProcessCleanup helper_cleanup(helper_pid);
  provider->Start(
      base::BindLambdaForTesting([&](const HerdrAgentSnapshot& snapshot) {
        old_snapshots.push_back(snapshot);
      }));
  ASSERT_TRUE(WaitForCondition([&]() { return CountLines(invocations) == 1; }));
  provider->Stop();
  provider->Start(
      base::BindLambdaForTesting([&](const HerdrAgentSnapshot& snapshot) {
        new_snapshots.push_back(snapshot);
      }));
  WriteFileOrDie(response, SnapshotWithOneAgent("working", true));
  ASSERT_TRUE(base::DeleteFile(hold));
  ASSERT_TRUE(WaitForCondition(
      [&]() {
        return CountLines(invocations) == 2 && !new_snapshots.empty() &&
               new_snapshots.back().state == HerdrAgentSourceState::kReady;
      },
      base::Seconds(5)));
  ASSERT_EQ(1u, old_snapshots.size());
  EXPECT_EQ(HerdrAgentSourceState::kLoading, old_snapshots[0].state);
  ASSERT_GE(new_snapshots.size(), 2u);
  ASSERT_EQ(1u, new_snapshots.back().agents.size());
  EXPECT_EQ(HerdrAgentStatus::kWorking, new_snapshots.back().agents[0].status);
  EXPECT_TRUE(new_snapshots.back().agents[0].focused);
  const pid_t last_helper = ReadProcessId(helper_pid);
  ASSERT_GT(last_helper, 0);
  EXPECT_TRUE(WaitForCondition([&]() { return ProcessIsGone(last_helper); }));
  provider->Stop();
}

class FakeHerdrAgentProvider final : public HerdrAgentProvider {
 public:
  void Start(SnapshotCallback callback) override {
    ++start_count;
    callback_ = callback;
    stale_callback_ = std::move(callback);
  }

  void RequestRefresh() override { ++refresh_count; }

  void Stop() override {
    ++stop_count;
    callback_.Reset();
  }

  void Publish(const HerdrAgentSnapshot& snapshot) {
    if (callback_) {
      callback_.Run(snapshot);
    }
  }

  void PublishStale(const HerdrAgentSnapshot& snapshot) {
    stale_callback_.Run(snapshot);
  }

  int start_count = 0;
  int stop_count = 0;
  int refresh_count = 0;

 private:
  SnapshotCallback callback_;
  SnapshotCallback stale_callback_;
};

class RecordingRegistryObserver final : public AgentPresenceRegistryObserver {
 public:
  void OnHerdrAgentSnapshotChanged(
      const HerdrAgentSnapshot& snapshot) override {
    snapshots.push_back(snapshot);
  }

  std::vector<HerdrAgentSnapshot> snapshots;
};

TEST(AgentPresenceRegistryTest,
     CoalescesSnapshotsAndOwnsProviderByObserverLifetime) {
  auto provider = std::make_unique<FakeHerdrAgentProvider>();
  FakeHerdrAgentProvider* provider_ptr = provider.get();
  AgentPresenceRegistry registry(std::move(provider));
  RecordingRegistryObserver first;
  RecordingRegistryObserver second;

  registry.AddObserver(&first);
  EXPECT_EQ(1, provider_ptr->start_count);
  ASSERT_EQ(1u, first.snapshots.size());
  EXPECT_EQ(HerdrAgentSourceState::kLoading, first.snapshots.back().state);

  HerdrAgentSnapshot ready =
      ParseHerdrAgentSnapshotForTesting(SnapshotWithOneAgent("idle"));
  provider_ptr->Publish(ready);
  provider_ptr->Publish(ready);
  ASSERT_EQ(2u, first.snapshots.size());
  EXPECT_EQ(ready, first.snapshots.back());

  registry.AddObserver(&second);
  EXPECT_EQ(1, provider_ptr->start_count);
  ASSERT_EQ(1u, second.snapshots.size());
  EXPECT_EQ(ready, second.snapshots.back());
  registry.RemoveObserver(&first);
  EXPECT_EQ(0, provider_ptr->stop_count);
  registry.RequestRefreshForTesting();
  EXPECT_EQ(1, provider_ptr->refresh_count);

  registry.RemoveObserver(&second);
  EXPECT_EQ(1, provider_ptr->stop_count);
  provider_ptr->PublishStale(
      ParseHerdrAgentSnapshotForTesting(SnapshotWithOneAgent("working")));
  EXPECT_EQ(1u, second.snapshots.size());

  registry.AddObserver(&first);
  EXPECT_EQ(2, provider_ptr->start_count);
  EXPECT_EQ(HerdrAgentSourceState::kLoading, first.snapshots.back().state);
  registry.RemoveObserver(&first);
  EXPECT_EQ(2, provider_ptr->stop_count);
}

}  // namespace
}  // namespace limitless
