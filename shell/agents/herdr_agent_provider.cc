// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/agents/herdr_agent_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/weak_ptr.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/values.h"

namespace limitless {
namespace {

constexpr base::TimeDelta kDefaultPollInterval = base::Seconds(2);
constexpr base::TimeDelta kDefaultRequestTimeout = base::Milliseconds(1500);
constexpr base::TimeDelta kKillReapTimeout = base::Seconds(1);
constexpr size_t kMaximumOutputBytes = 1024 * 1024;
constexpr size_t kMaximumAgentCount = 256;
constexpr size_t kMaximumIdentifierBytes = 128;
constexpr size_t kMaximumAgentBytes = 64;
constexpr size_t kMaximumTitleBytes = 512;
constexpr size_t kMaximumVersionBytes = 64;
constexpr std::string_view kHerdrSnapshotResponseId = "cli:api:snapshot";

HerdrAgentSnapshot SnapshotWithState(HerdrAgentSourceState state) {
  HerdrAgentSnapshot snapshot;
  snapshot.state = state;
  return snapshot;
}

bool IsSafeIdentifier(std::string_view value) {
  if (value.empty() || value.size() > kMaximumIdentifierBytes) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-' || character == ':' || character == '.';
  });
}

bool IsUnsafeLabelCharacter(char16_t character) {
  return character < 0x20 || (character >= 0x7f && character <= 0x9f) ||
         character == 0x061c || (character >= 0x200e && character <= 0x200f) ||
         (character >= 0x202a && character <= 0x202e) ||
         (character >= 0x2066 && character <= 0x2069);
}

std::optional<std::u16string> SanitizeLabel(std::string_view label,
                                            size_t maximum_bytes) {
  if (label.size() > maximum_bytes || !base::IsStringUTF8(label)) {
    return std::nullopt;
  }
  std::u16string sanitized = base::UTF8ToUTF16(label);
  std::erase_if(sanitized, IsUnsafeLabelCharacter);
  base::TrimWhitespace(sanitized, base::TRIM_ALL, &sanitized);
  return sanitized;
}

std::optional<HerdrAgentStatus> ParseStatus(std::string_view status) {
  if (status == "idle") {
    return HerdrAgentStatus::kIdle;
  }
  if (status == "working") {
    return HerdrAgentStatus::kWorking;
  }
  if (status == "blocked") {
    return HerdrAgentStatus::kBlocked;
  }
  if (status == "done") {
    return HerdrAgentStatus::kDone;
  }
  if (status == "unknown") {
    return HerdrAgentStatus::kUnknown;
  }
  return std::nullopt;
}

std::optional<std::u16string> ResolveAgentLabel(
    const base::DictValue& agent_dict,
    std::string_view pane_id) {
  for (std::string_view key : {"agent", "display_agent", "name"}) {
    const base::Value* value = agent_dict.Find(key);
    if (!value || value->is_none()) {
      continue;
    }
    if (!value->is_string()) {
      return std::nullopt;
    }
    std::optional<std::u16string> label =
        SanitizeLabel(value->GetString(), kMaximumAgentBytes);
    if (!label) {
      return std::nullopt;
    }
    if (!label->empty()) {
      return label;
    }
  }
  return base::UTF8ToUTF16(pane_id);
}

HerdrAgentSnapshot ParseHerdrAgentSnapshot(std::string_view output) {
  if (output.empty() || output.size() > kMaximumOutputBytes) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  std::optional<base::Value> root =
      base::JSONReader::Read(output, base::JSON_PARSE_RFC);
  if (!root || !root->is_dict()) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  const base::DictValue& root_dict = root->GetDict();
  const std::string* response_id = root_dict.FindString("id");
  if (!response_id || *response_id != kHerdrSnapshotResponseId) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  if (const base::Value* error = root_dict.Find("error")) {
    if (!error->is_dict()) {
      return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
    }
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  const base::DictValue* result = root_dict.FindDict("result");
  const std::string* result_type =
      result ? result->FindString("type") : nullptr;
  if (!result || !result_type || *result_type != "session_snapshot") {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  const base::DictValue* snapshot_dict = result->FindDict("snapshot");
  if (!snapshot_dict) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  const std::optional<int> protocol = snapshot_dict->FindInt("protocol");
  if (!protocol) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  if (*protocol != kSupportedHerdrProtocol) {
    return SnapshotWithState(HerdrAgentSourceState::kIncompatibleProtocol);
  }
  const std::string* version = snapshot_dict->FindString("version");
  if (!version || version->empty() || version->size() > kMaximumVersionBytes ||
      !base::IsStringUTF8(*version)) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  const base::ListValue* agents = snapshot_dict->FindList("agents");
  if (!agents || agents->size() > kMaximumAgentCount) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }

  HerdrAgentSnapshot snapshot;
  snapshot.state = HerdrAgentSourceState::kReady;
  snapshot.version = *version;
  snapshot.protocol = *protocol;
  std::map<std::string, std::u16string> workspace_labels;
  const base::Value* workspaces_value = snapshot_dict->Find("workspaces");
  if (workspaces_value && !workspaces_value->is_list()) {
    return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
  }
  if (const base::ListValue* workspaces =
          workspaces_value ? &workspaces_value->GetList() : nullptr) {
    if (workspaces->size() > kMaximumAgentCount) {
      return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
    }
    for (const base::Value& value : *workspaces) {
      if (!value.is_dict()) {
        return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
      }
      const base::DictValue& workspace_dict = value.GetDict();
      const std::string* workspace_id =
          workspace_dict.FindString("workspace_id");
      const std::string* label = workspace_dict.FindString("label");
      if (!workspace_id || !IsSafeIdentifier(*workspace_id) || !label) {
        return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
      }
      std::optional<std::u16string> sanitized_label =
          SanitizeLabel(*label, kMaximumTitleBytes);
      if (!sanitized_label ||
          !workspace_labels.emplace(*workspace_id, std::move(*sanitized_label))
               .second) {
        return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
      }
    }
  }
  std::set<std::string> pane_ids;
  for (const base::Value& value : *agents) {
    if (!value.is_dict()) {
      return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
    }
    const base::DictValue& agent_dict = value.GetDict();
    const std::string* pane_id = agent_dict.FindString("pane_id");
    const std::string* workspace_id = agent_dict.FindString("workspace_id");
    const std::string* status = agent_dict.FindString("agent_status");
    const base::Value* terminal_title_value =
        agent_dict.Find("terminal_title_stripped");
    if (terminal_title_value && !terminal_title_value->is_none() &&
        !terminal_title_value->is_string()) {
      return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
    }
    const std::string* terminal_title =
        terminal_title_value && terminal_title_value->is_string()
            ? &terminal_title_value->GetString()
            : nullptr;
    const std::optional<bool> focused = agent_dict.FindBool("focused");
    if (!pane_id || !workspace_id || !status || !focused ||
        !IsSafeIdentifier(*pane_id) || !IsSafeIdentifier(*workspace_id) ||
        !pane_ids.insert(*pane_id).second) {
      return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
    }
    std::optional<HerdrAgentStatus> parsed_status = ParseStatus(*status);
    std::optional<std::u16string> sanitized_agent =
        ResolveAgentLabel(agent_dict, *pane_id);
    std::optional<std::u16string> sanitized_title =
        terminal_title ? SanitizeLabel(*terminal_title, kMaximumTitleBytes)
                       : std::optional<std::u16string>(std::u16string());
    if (!parsed_status || !sanitized_agent || sanitized_agent->empty() ||
        !sanitized_title) {
      return SnapshotWithState(HerdrAgentSourceState::kMalformedResponse);
    }
    if (sanitized_title->empty()) {
      *sanitized_title = *sanitized_agent + u" " + base::UTF8ToUTF16(*pane_id);
    }
    std::u16string workspace_label = base::UTF8ToUTF16(*workspace_id);
    if (auto workspace = workspace_labels.find(*workspace_id);
        workspace != workspace_labels.end() && !workspace->second.empty()) {
      workspace_label = workspace->second;
    }
    snapshot.agents.push_back(HerdrAgentPresence{
        .pane_id = *pane_id,
        .workspace_id = *workspace_id,
        .workspace_label = std::move(workspace_label),
        .agent = base::UTF16ToUTF8(*sanitized_agent),
        .status = *parsed_status,
        .terminal_title = std::move(*sanitized_title),
        .focused = *focused,
    });
  }
  return snapshot;
}

struct ExecutableResolution {
  base::FilePath path;
  HerdrAgentSourceState failure = HerdrAgentSourceState::kMissingExecutable;
};

ExecutableResolution ValidateExecutable(const base::FilePath& path) {
  if (path.empty() || !path.IsAbsolute()) {
    return {};
  }
  struct stat file_info;
  if (stat(path.value().c_str(), &file_info) != 0) {
    return {.failure = errno == EACCES
                           ? HerdrAgentSourceState::kPermissionDenied
                           : HerdrAgentSourceState::kMissingExecutable};
  }
  if (!S_ISREG(file_info.st_mode)) {
    return {.failure = HerdrAgentSourceState::kPermissionDenied};
  }
  if (access(path.value().c_str(), X_OK) != 0) {
    return {.failure = HerdrAgentSourceState::kPermissionDenied};
  }
  return {.path = path};
}

ExecutableResolution ResolveHerdrExecutable() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(kHerdrExecutableSwitch)) {
    return ValidateExecutable(
        command_line->GetSwitchValuePath(kHerdrExecutableSwitch));
  }

  std::vector<base::FilePath> candidates = {
      base::FilePath("/opt/homebrew/bin/herdr"),
      base::FilePath("/usr/local/bin/herdr"),
  };
  HerdrAgentSourceState failure = HerdrAgentSourceState::kMissingExecutable;
  for (const base::FilePath& candidate : candidates) {
    ExecutableResolution resolution = ValidateExecutable(candidate);
    if (!resolution.path.empty()) {
      return resolution;
    }
    if (resolution.failure == HerdrAgentSourceState::kPermissionDenied) {
      failure = resolution.failure;
    }
  }
  return {.failure = failure};
}

enum class DrainResult {
  kOpen,
  kEof,
  kError,
  kOversized,
};

DrainResult DrainPipe(int descriptor, std::string* output) {
  std::array<char, 4096> buffer;
  while (true) {
    const ssize_t bytes_read =
        HANDLE_EINTR(read(descriptor, buffer.data(), buffer.size()));
    if (bytes_read > 0) {
      if (output->size() + static_cast<size_t>(bytes_read) >
          kMaximumOutputBytes) {
        return DrainResult::kOversized;
      }
      output->append(buffer.data(), static_cast<size_t>(bytes_read));
      continue;
    }
    if (bytes_read == 0) {
      return DrainResult::kEof;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return DrainResult::kOpen;
    }
    return DrainResult::kError;
  }
}

void KillAndReap(base::Process* process, pid_t process_group_id) {
  const int kill_result = HANDLE_EINTR(kill(-process_group_id, SIGKILL));
  if (kill_result != 0 && errno != ESRCH) {
    base::EnsureProcessTerminated(std::move(*process));
    return;
  }
  if (!process->WaitForExitWithTimeout(kKillReapTimeout, nullptr)) {
    base::EnsureProcessTerminated(std::move(*process));
  }
}

enum class LeaderExitResult {
  kExited,
  kTimedOut,
  kError,
};

LeaderExitResult WaitForLeaderExitWithoutReaping(pid_t process_id,
                                                 base::TimeTicks deadline) {
  base::ScopedFD queue(HANDLE_EINTR(kqueue()));
  if (!queue.is_valid()) {
    return LeaderExitResult::kError;
  }

  struct kevent change = {};
  EV_SET(&change, process_id, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
  if (HANDLE_EINTR(kevent(queue.get(), &change, 1, nullptr, 0, nullptr)) < 0) {
    // This function exclusively owns the unreaped child. ESRCH therefore
    // means that the leader is already dead or in the macOS zombie-death
    // race, while its unreaped PID still pins the process-group identity.
    if (errno == ESRCH) {
      return LeaderExitResult::kExited;
    }
    return LeaderExitResult::kError;
  }

  while (true) {
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (!remaining.is_positive()) {
      return LeaderExitResult::kTimedOut;
    }

    struct kevent event = {};
    const struct timespec timeout = remaining.ToTimeSpec();
    const int result = kevent(queue.get(), nullptr, 0, &event, 1, &timeout);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return LeaderExitResult::kError;
    }
    if (result == 0) {
      return LeaderExitResult::kTimedOut;
    }
    if (event.ident != static_cast<uintptr_t>(process_id) ||
        !(event.fflags & NOTE_EXIT)) {
      return LeaderExitResult::kError;
    }
    return LeaderExitResult::kExited;
  }
}

bool TerminateResidualProcessGroupAndReapLeader(base::Process* process,
                                                pid_t process_group_id,
                                                int* exit_code) {
  // The leader has exited but remains unreaped, so its PID and process-group
  // ID cannot be reused while this signal is sent. The signal has no effect on
  // the zombie leader but terminates descendants that closed stdout before the
  // leader exited. macOS reports EPERM when the zombie leader is the only
  // remaining group member; a live same-user descendant makes this group
  // signal succeed and is covered by the residual-child regression.
  const int kill_result = HANDLE_EINTR(kill(-process_group_id, SIGKILL));
  if (kill_result != 0 && errno != ESRCH && errno != EPERM) {
    base::EnsureProcessTerminated(std::move(*process));
    return false;
  }
  if (!process->WaitForExitWithTimeout(kKillReapTimeout, exit_code)) {
    base::EnsureProcessTerminated(std::move(*process));
    return false;
  }
  return true;
}

bool SetCloseOnExec(int descriptor) {
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  return descriptor_flags >= 0 &&
         fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

HerdrAgentSnapshot RunHerdrSnapshotCommand(base::FilePath executable,
                                           base::TimeDelta request_timeout) {
  ExecutableResolution resolution = ValidateExecutable(executable);
  if (resolution.path.empty()) {
    return SnapshotWithState(resolution.failure);
  }

  int pipe_descriptors[2];
  if (pipe(pipe_descriptors) != 0) {
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  base::ScopedFD read_descriptor(pipe_descriptors[0]);
  base::ScopedFD write_descriptor(pipe_descriptors[1]);
  if (!SetCloseOnExec(read_descriptor.get()) ||
      !SetCloseOnExec(write_descriptor.get())) {
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  const int current_flags = fcntl(read_descriptor.get(), F_GETFL);
  if (current_flags < 0 ||
      fcntl(read_descriptor.get(), F_SETFL, current_flags | O_NONBLOCK) < 0) {
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  base::ScopedFD null_descriptor(
      HANDLE_EINTR(open("/dev/null", O_WRONLY | O_CLOEXEC)));
  if (!null_descriptor.is_valid()) {
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }

  base::LaunchOptions options;
  options.new_process_group = true;
  options.fds_to_remap.emplace_back(write_descriptor.get(), STDOUT_FILENO);
  options.fds_to_remap.emplace_back(null_descriptor.get(), STDERR_FILENO);
  const std::vector<std::string> argv = {
      executable.value(),
      "api",
      "snapshot",
  };
  base::Process process = base::LaunchProcess(argv, options);
  if (!process.IsValid()) {
    return SnapshotWithState(errno == EACCES
                                 ? HerdrAgentSourceState::kPermissionDenied
                                 : HerdrAgentSourceState::kUnavailable);
  }
  const pid_t process_group_id = process.Pid();
  write_descriptor.reset();
  null_descriptor.reset();

  const base::TimeTicks deadline = base::TimeTicks::Now() + request_timeout;
  bool pipe_eof = false;
  int exit_code = 0;
  std::string output;
  while (base::TimeTicks::Now() < deadline && !pipe_eof) {
    DrainResult drain_result = DrainPipe(read_descriptor.get(), &output);
    if (drain_result == DrainResult::kError ||
        drain_result == DrainResult::kOversized) {
      KillAndReap(&process, process_group_id);
      return SnapshotWithState(drain_result == DrainResult::kOversized
                                   ? HerdrAgentSourceState::kMalformedResponse
                                   : HerdrAgentSourceState::kUnavailable);
    }
    pipe_eof = drain_result == DrainResult::kEof;
    if (pipe_eof) {
      break;
    }

    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    const int wait_milliseconds = static_cast<int>(
        std::clamp<int64_t>(remaining.InMillisecondsRoundedUp(), 1, 50));
    pollfd descriptor = {
        .fd = read_descriptor.get(),
        .events = POLLIN | POLLHUP,
        .revents = 0,
    };
    const int poll_result =
        HANDLE_EINTR(poll(&descriptor, 1, wait_milliseconds));
    if (poll_result < 0) {
      KillAndReap(&process, process_group_id);
      return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
    }
  }

  if (!pipe_eof) {
    KillAndReap(&process, process_group_id);
    return SnapshotWithState(HerdrAgentSourceState::kTimedOut);
  }
  const LeaderExitResult leader_exit =
      WaitForLeaderExitWithoutReaping(process.Pid(), deadline);
  if (leader_exit == LeaderExitResult::kTimedOut) {
    KillAndReap(&process, process_group_id);
    return SnapshotWithState(HerdrAgentSourceState::kTimedOut);
  }
  if (leader_exit == LeaderExitResult::kError) {
    KillAndReap(&process, process_group_id);
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  if (!TerminateResidualProcessGroupAndReapLeader(&process, process_group_id,
                                                  &exit_code)) {
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  if (exit_code != 0) {
    return SnapshotWithState(HerdrAgentSourceState::kUnavailable);
  }
  return ParseHerdrAgentSnapshot(output);
}

class HerdrCliAgentProvider final : public HerdrAgentProvider {
 public:
  HerdrCliAgentProvider(ExecutableResolution executable,
                        base::TimeDelta poll_interval,
                        base::TimeDelta request_timeout)
      : executable_(std::move(executable)),
        poll_interval_(poll_interval),
        request_timeout_(request_timeout),
        worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::WithBaseSyncPrimitives(),
             base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

  HerdrCliAgentProvider(const HerdrCliAgentProvider&) = delete;
  HerdrCliAgentProvider& operator=(const HerdrCliAgentProvider&) = delete;
  ~HerdrCliAgentProvider() override { Stop(); }

  void Start(SnapshotCallback callback) override {
    Stop();
    callback_ = std::move(callback);
    ++generation_;
    last_snapshot_.reset();
    Publish(SnapshotWithState(HerdrAgentSourceState::kLoading));
    if (executable_.path.empty()) {
      Publish(SnapshotWithState(executable_.failure));
      return;
    }
    timer_.Start(FROM_HERE, poll_interval_, this,
                 &HerdrCliAgentProvider::RequestRefresh);
    RequestRefresh();
  }

  void RequestRefresh() override {
    if (!callback_ || executable_.path.empty()) {
      return;
    }
    if (request_in_flight_) {
      refresh_pending_ = true;
      return;
    }
    request_in_flight_ = true;
    const uint64_t generation = generation_;
    const bool posted = worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&RunHerdrSnapshotCommand, executable_.path,
                       request_timeout_),
        base::BindOnce(&HerdrCliAgentProvider::OnRefreshCompleted,
                       weak_ptr_factory_.GetWeakPtr(), generation));
    if (!posted) {
      request_in_flight_ = false;
      Publish(SnapshotWithState(HerdrAgentSourceState::kUnavailable));
    }
  }

  void Stop() override {
    timer_.Stop();
    ++generation_;
    weak_ptr_factory_.InvalidateWeakPtrs();
    request_in_flight_ = false;
    refresh_pending_ = false;
    last_snapshot_.reset();
    callback_.Reset();
  }

 private:
  void OnRefreshCompleted(uint64_t generation, HerdrAgentSnapshot snapshot) {
    if (!callback_ || generation != generation_) {
      return;
    }
    request_in_flight_ = false;
    const bool refresh_pending = std::exchange(refresh_pending_, false);
    Publish(std::move(snapshot));
    if (refresh_pending) {
      RequestRefresh();
    }
  }

  void Publish(HerdrAgentSnapshot snapshot) {
    if (!callback_ || (last_snapshot_ && *last_snapshot_ == snapshot)) {
      return;
    }
    last_snapshot_ = std::move(snapshot);
    callback_.Run(*last_snapshot_);
  }

  ExecutableResolution executable_;
  const base::TimeDelta poll_interval_;
  const base::TimeDelta request_timeout_;
  const scoped_refptr<base::SequencedTaskRunner> worker_;
  SnapshotCallback callback_;
  base::RepeatingTimer timer_;
  std::optional<HerdrAgentSnapshot> last_snapshot_;
  uint64_t generation_ = 0;
  bool request_in_flight_ = false;
  bool refresh_pending_ = false;
  base::WeakPtrFactory<HerdrCliAgentProvider> weak_ptr_factory_{this};
};

}  // namespace

std::string_view HerdrAgentStatusName(HerdrAgentStatus status) {
  switch (status) {
    case HerdrAgentStatus::kIdle:
      return "idle";
    case HerdrAgentStatus::kWorking:
      return "working";
    case HerdrAgentStatus::kBlocked:
      return "blocked";
    case HerdrAgentStatus::kDone:
      return "done";
    case HerdrAgentStatus::kUnknown:
      return "unknown";
  }
}

std::string_view HerdrAgentSourceStateName(HerdrAgentSourceState state) {
  switch (state) {
    case HerdrAgentSourceState::kLoading:
      return "loading";
    case HerdrAgentSourceState::kReady:
      return "ready";
    case HerdrAgentSourceState::kMissingExecutable:
      return "missingExecutable";
    case HerdrAgentSourceState::kPermissionDenied:
      return "permissionDenied";
    case HerdrAgentSourceState::kUnavailable:
      return "unavailable";
    case HerdrAgentSourceState::kTimedOut:
      return "timedOut";
    case HerdrAgentSourceState::kMalformedResponse:
      return "malformedResponse";
    case HerdrAgentSourceState::kIncompatibleProtocol:
      return "incompatibleProtocol";
    case HerdrAgentSourceState::kPrivateProfileDisabled:
      return "privateProfileDisabled";
  }
}

std::unique_ptr<HerdrAgentProvider> CreateHerdrCliAgentProvider() {
  return std::make_unique<HerdrCliAgentProvider>(
      ResolveHerdrExecutable(), kDefaultPollInterval, kDefaultRequestTimeout);
}

std::unique_ptr<HerdrAgentProvider> CreateHerdrCliAgentProviderForTesting(
    const base::FilePath& executable,
    base::TimeDelta poll_interval,
    base::TimeDelta request_timeout) {
  return std::make_unique<HerdrCliAgentProvider>(
      ValidateExecutable(executable), poll_interval, request_timeout);
}

HerdrAgentSnapshot ParseHerdrAgentSnapshotForTesting(std::string_view output) {
  return ParseHerdrAgentSnapshot(output);
}

}  // namespace limitless
