#include "shell/control_plane/limitless_preference_control_transport_worker.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "shell/control_plane/limitless_preference_control_protocol.h"
#include "shell/control_plane/limitless_preference_control_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

constexpr base::TimeDelta kRunTestTimeout = base::Seconds(10);
constexpr base::TimeDelta kClientIoTimeout = base::Seconds(5);

enum class ReadResult {
  kComplete,
  kEof,
  kError,
};

struct ClientResult {
  bool connected = false;
  bool request_sent = false;
  bool response_received = false;
  bool eof = false;
  std::string response;
};

struct SlowDripResult {
  bool connected = false;
  bool header_sent = false;
  bool eof = false;
  size_t payload_bytes_sent = 0;
  base::TimeDelta elapsed;
};

bool ConfigureClientTimeouts(int fd) {
  const int64_t microseconds = kClientIoTimeout.InMicroseconds();
  const timeval timeout = {
      .tv_sec = static_cast<time_t>(microseconds / 1000000),
      .tv_usec = static_cast<suseconds_t>(microseconds % 1000000),
  };
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
             0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
             0;
}

base::ScopedFD ConnectOnce(const base::FilePath& socket_path) {
  base::ScopedFD connection(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!connection.is_valid() || !ConfigureClientTimeouts(connection.get())) {
    return {};
  }
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (socket_path.value().size() >= sizeof(address.sun_path)) {
    return {};
  }
  memcpy(address.sun_path, socket_path.value().c_str(),
         socket_path.value().size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.value().size() + 1);
  if (connect(connection.get(), reinterpret_cast<sockaddr*>(&address),
              address_length) != 0) {
    return {};
  }
  return connection;
}

bool WriteExactlyForClient(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t written = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (written > 0) {
      bytes.remove_prefix(static_cast<size_t>(written));
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool WriteFrameForClient(int fd, std::string_view payload) {
  const uint32_t network_size = htonl(static_cast<uint32_t>(payload.size()));
  return WriteExactlyForClient(
             fd, std::string_view(reinterpret_cast<const char*>(&network_size),
                                  sizeof(network_size))) &&
         WriteExactlyForClient(fd, payload);
}

ReadResult ReadExactlyForClient(int fd, char* output, size_t size) {
  while (size > 0) {
    const ssize_t read_size = read(fd, output, size);
    if (read_size > 0) {
      output += read_size;
      size -= static_cast<size_t>(read_size);
      continue;
    }
    if (read_size == 0) {
      return ReadResult::kEof;
    }
    if (errno == EINTR) {
      continue;
    }
    return ReadResult::kError;
  }
  return ReadResult::kComplete;
}

void ReadFramedResponse(int fd, ClientResult* result) {
  uint32_t network_size = 0;
  const ReadResult header_result = ReadExactlyForClient(
      fd, reinterpret_cast<char*>(&network_size), sizeof(network_size));
  if (header_result != ReadResult::kComplete) {
    result->eof = header_result == ReadResult::kEof;
    return;
  }
  const size_t response_size = ntohl(network_size);
  if (response_size > kMaximumPreferenceTransportResponseBytes) {
    return;
  }
  result->response.resize(response_size);
  const ReadResult payload_result = ReadExactlyForClient(
      fd, result->response.data(), result->response.size());
  result->response_received = payload_result == ReadResult::kComplete;
  result->eof = payload_result == ReadResult::kEof;
}

ClientResult ExchangeFrame(const base::FilePath& socket_path,
                           std::string payload) {
  ClientResult result;
  base::ScopedFD connection = ConnectOnce(socket_path);
  result.connected = connection.is_valid();
  if (!connection.is_valid()) {
    return result;
  }
  result.request_sent = WriteFrameForClient(connection.get(), payload);
  if (result.request_sent) {
    ReadFramedResponse(connection.get(), &result);
  }
  return result;
}

ClientResult SendOversizedHeader(const base::FilePath& socket_path) {
  ClientResult result;
  base::ScopedFD connection = ConnectOnce(socket_path);
  result.connected = connection.is_valid();
  if (!connection.is_valid()) {
    return result;
  }
  const uint32_t network_size =
      htonl(static_cast<uint32_t>(kMaximumPreferenceRequestBytes + 1));
  result.request_sent = WriteExactlyForClient(
      connection.get(),
      std::string_view(reinterpret_cast<const char*>(&network_size),
                       sizeof(network_size)));
  if (result.request_sent) {
    ReadFramedResponse(connection.get(), &result);
  }
  return result;
}

ClientResult SendTruncatedBody(const base::FilePath& socket_path,
                               bool await_cancellation) {
  ClientResult result;
  base::ScopedFD connection = ConnectOnce(socket_path);
  result.connected = connection.is_valid();
  if (!connection.is_valid()) {
    return result;
  }
  const uint32_t network_size = htonl(8u);
  result.request_sent =
      WriteExactlyForClient(
          connection.get(),
          std::string_view(reinterpret_cast<const char*>(&network_size),
                           sizeof(network_size))) &&
      WriteExactlyForClient(connection.get(), "ab");
  if (!result.request_sent) {
    return result;
  }
  if (!await_cancellation) {
    shutdown(connection.get(), SHUT_WR);
  }
  char byte = 0;
  const ReadResult read_result =
      ReadExactlyForClient(connection.get(), &byte, sizeof(byte));
  result.eof = read_result == ReadResult::kEof;
  return result;
}

ClientResult ConnectAndAwaitEof(const base::FilePath& socket_path) {
  ClientResult result;
  base::ScopedFD connection = ConnectOnce(socket_path);
  result.connected = connection.is_valid();
  if (!connection.is_valid()) {
    return result;
  }
  char byte = 0;
  result.eof = ReadExactlyForClient(connection.get(), &byte, sizeof(byte)) ==
               ReadResult::kEof;
  return result;
}

SlowDripResult DripPayloadPastDeadline(const base::FilePath& socket_path) {
  constexpr size_t kPayloadSize = 12;
  constexpr base::TimeDelta kDripInterval = base::Milliseconds(100);
  SlowDripResult result;
  base::ScopedFD connection = ConnectOnce(socket_path);
  result.connected = connection.is_valid();
  if (!connection.is_valid()) {
    return result;
  }
  const uint32_t network_size = htonl(static_cast<uint32_t>(kPayloadSize));
  result.header_sent = WriteExactlyForClient(
      connection.get(),
      std::string_view(reinterpret_cast<const char*>(&network_size),
                       sizeof(network_size)));
  if (!result.header_sent) {
    return result;
  }

  const base::TimeTicks started = base::TimeTicks::Now();
  for (size_t i = 0; i < kPayloadSize; ++i) {
    const char byte = 'x';
    ssize_t written;
    do {
      written = send(connection.get(), &byte, sizeof(byte), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != 1) {
      break;
    }
    ++result.payload_bytes_sent;
    base::PlatformThread::Sleep(kDripInterval);
    char response_byte = 0;
    const ssize_t observed = recv(connection.get(), &response_byte,
                                  sizeof(response_byte), MSG_DONTWAIT);
    if (observed == 0) {
      result.eof = true;
      break;
    }
    if (observed < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
        errno != EINTR) {
      break;
    }
  }
  result.elapsed = base::TimeTicks::Now() - started;
  if (!result.eof) {
    char response_byte = 0;
    result.eof =
        ReadExactlyForClient(connection.get(), &response_byte,
                             sizeof(response_byte)) == ReadResult::kEof;
  }
  return result;
}

void SignalEvent(base::WaitableEvent* event) {
  event->Signal();
}

void SignalAcceptedEvent(base::WaitableEvent* event, int fd) {
  event->Signal();
}

struct AcceptedFdEvidence {
  void Inspect(int fd) {
    const int descriptor_flags = fcntl(fd, F_GETFD);
    const int status_flags = fcntl(fd, F_GETFL);
    close_on_exec.store(
        descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
        std::memory_order_release);
    blocking.store(status_flags >= 0 && (status_flags & O_NONBLOCK) == 0,
                   std::memory_order_release);
    inspected.Signal();
  }

  std::atomic<bool> close_on_exec{false};
  std::atomic<bool> blocking{false};
  base::WaitableEvent inspected;
};

class RejectingSequencedTaskRunner : public base::SequencedTaskRunner {
 public:
  bool PostDelayedTask(const base::Location& from_here,
                       base::OnceClosure task,
                       base::TimeDelta delay) override {
    post_attempt_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  bool PostNonNestableDelayedTask(const base::Location& from_here,
                                  base::OnceClosure task,
                                  base::TimeDelta delay) override {
    return false;
  }

  bool RunsTasksInCurrentSequence() const override { return false; }

  std::atomic<int> post_attempt_count{0};

 private:
  ~RejectingSequencedTaskRunner() override = default;
};

class ProtocolBackend : public PreferenceControlBackend {
 public:
  std::string_view profile_identifier() const override {
    return "11111111-1111-4111-8111-111111111111";
  }

  std::string_view profile_instance_identifier() const override {
    return "22222222-2222-4222-8222-222222222222";
  }

  bool is_off_the_record() const override { return false; }

  bool Authenticate(std::string_view capability) const override {
    return capability == "correct-capability";
  }

  bool IsManaged(PreferenceKey key) const override { return false; }

  EffectivePreferenceValue GetEffectiveValue(PreferenceKey key) const override {
    return {.value = {.rail_density = RailDensity::kCompact},
            .source = PreferenceValueSource::kDefault};
  }

  bool SetUserValue(PreferenceKey key, const PreferenceValue& value) override {
    mutation_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  uint64_t AppendAuditRecord(const PreferenceAuditRecord& record) override {
    audit_count.fetch_add(1, std::memory_order_relaxed);
    if (record.status == PreferenceControlStatus::kSuccess) {
      successful_audit_count.fetch_add(1, std::memory_order_relaxed);
    }
    last_audit_status.store(record.status, std::memory_order_relaxed);
    return 17;
  }

  std::atomic<int> mutation_count{0};
  std::atomic<int> audit_count{0};
  std::atomic<int> successful_audit_count{0};
  std::atomic<PreferenceControlStatus> last_audit_status{
      PreferenceControlStatus::kWriteFailed};
};

PreferenceControlTransportWorker::Inputs Inputs(const base::FilePath& p);

class PreferenceControlTransportWorkerRunTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_directory_.CreateUniqueTempDir());
    ASSERT_TRUE(worker_thread_.Start());
    ASSERT_TRUE(origin_thread_.Start());
  }

  void TearDown() override {
    cancellation_->Cancel();
    if (run_started_ && !run_complete_.IsReady()) {
      EXPECT_TRUE(run_complete_.Wait());
    }
    origin_thread_.Stop();
    worker_thread_.Stop();
  }

  PreferenceControlTransportWorker::Inputs NewInputs() {
    return Inputs(temp_directory_.GetPath());
  }

  void StartWorker(
      PreferenceControlTransportWorker::Inputs inputs,
      PreferenceControlTransportWorker::OriginRequestHandler handler,
      scoped_refptr<base::SequencedTaskRunner> origin_task_runner = nullptr) {
    ASSERT_FALSE(run_started_);
    run_started_ = true;
    if (!origin_task_runner) {
      origin_task_runner = origin_thread_.task_runner();
    }
    auto prepare_callback = prepare_result_.GetSequenceBoundCallback();
    auto run_complete_callback = run_complete_.GetSequenceBoundCallback();
    ASSERT_TRUE(worker_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](PreferenceControlTransportWorker::Inputs worker_inputs,
               scoped_refptr<TransportCancellationState> cancellation,
               scoped_refptr<base::SequencedTaskRunner> origin_task_runner,
               PreferenceControlTransportWorker::OriginRequestHandler
                   request_handler,
               base::OnceCallback<void(
                   PreferenceControlTransportWorker::PrepareResult)> prepared,
               base::OnceClosure run_complete) {
              PreferenceControlTransportWorker worker(std::move(worker_inputs),
                                                      std::move(cancellation));
              auto result = worker.Prepare();
              const bool prepared_successfully = result.succeeded;
              std::move(prepared).Run(std::move(result));
              if (prepared_successfully) {
                worker.Run(std::move(origin_task_runner),
                           std::move(request_handler));
              }
              worker.Cleanup();
              std::move(run_complete).Run();
            },
            std::move(inputs), cancellation_, std::move(origin_task_runner),
            std::move(handler), std::move(prepare_callback),
            std::move(run_complete_callback))));
    ASSERT_TRUE(prepare_result_.Wait());
    ASSERT_TRUE(prepare_result_.Get().succeeded);
  }

  void CancelAndWaitForRun() {
    cancellation_->Cancel();
    ASSERT_TRUE(run_complete_.Wait());
  }

  template <typename Result, typename Function, typename... Arguments>
  Result RunClient(Function function, Arguments... arguments) {
    base::test::TestFuture<Result> result;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(function, std::move(arguments)...),
        result.GetCallback());
    EXPECT_TRUE(result.Wait());
    return result.Take();
  }

  bool TimedWait(base::WaitableEvent* event) {
    base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
    return event->TimedWait(kRunTestTimeout);
  }

  base::test::TaskEnvironment task_environment_;
  base::test::ScopedRunLoopTimeout run_loop_timeout_{FROM_HERE,
                                                     kRunTestTimeout};
  base::ScopedTempDir temp_directory_;
  base::Thread worker_thread_{"limitless-worker-run-test"};
  base::Thread origin_thread_{"limitless-worker-origin-test"};
  scoped_refptr<TransportCancellationState> cancellation_ =
      base::MakeRefCounted<TransportCancellationState>();
  base::test::TestFuture<PreferenceControlTransportWorker::PrepareResult>
      prepare_result_;
  base::test::TestFuture<void> run_complete_;
  bool run_started_ = false;
};

void CancelState(scoped_refptr<TransportCancellationState> state) {
  state->Cancel();
}
void WriteForeignDescriptor(base::FilePath path) {
  base::WriteFile(path, "foreign-descriptor");
}
void SwapRuntimeDirectory(base::FilePath current,
                          base::FilePath old_path,
                          bool* succeeded) {
  *succeeded = rename(current.value().c_str(), old_path.value().c_str()) == 0 &&
               base::CreateDirectory(current);
  if (!*succeeded) {
    return;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  std::string path = old_path.AppendASCII("s").value();
  memcpy(address.sun_path, path.c_str(), path.size() + 1);
  *succeeded =
      fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&address),
                      offsetof(sockaddr_un, sun_path) + path.size() + 1) == 0;
  if (fd >= 0) {
    close(fd);
  }
}
struct Evidence {
  PreferenceControlTransportWorker::PrepareResult result;
  bool socket = false;
  bool token = false;
  bool descriptor = false;
  std::string token_contents;
  std::string descriptor_contents;
  struct stat runtime_stat = {};
  struct stat socket_stat = {};
  struct stat token_stat = {};
  struct stat descriptor_stat = {};
  bool runtime_stat_ok = false;
  bool socket_stat_ok = false;
  bool token_stat_ok = false;
  bool descriptor_stat_ok = false;
  bool socket_removed = false;
  bool token_removed = false;
  bool descriptor_removed = false;
  bool residue_free = true;
};

struct FallbackCollisionEvidence {
  PreferenceControlTransportWorker::PrepareResult first_result;
  PreferenceControlTransportWorker::PrepareResult second_result;
  bool first_socket_survived = false;
  bool first_token_survived = false;
  bool first_descriptor_survived = false;
  bool second_token_absent = false;
  bool second_descriptor_absent = false;
  bool first_socket_removed = false;
  bool first_token_removed = false;
  bool first_descriptor_removed = false;
  bool parent_before_ok = false;
  bool parent_after_ok = false;
  struct stat parent_before = {};
  struct stat parent_after = {};
};

PreferenceControlTransportWorker::Inputs Inputs(const base::FilePath& p) {
  return {p.AppendASCII("r"),
          p.AppendASCII("r/d"),
          p.AppendASCII("r/t"),
          p.AppendASCII("r/s"),
          "p",
          "i",
          "cap"};
}

void ConfigureLongSocketFallback(
    PreferenceControlTransportWorker::Inputs* inputs,
    const base::FilePath& unique_root) {
  std::string unique_suffix = unique_root.BaseName().value();
  if (unique_suffix.size() > 20) {
    unique_suffix.erase(0, unique_suffix.size() - 20);
  }
  inputs->profile_id = "fallback-test";
  inputs->instance_id =
      std::to_string(base::GetCurrentProcId()) + "-" + std::move(unique_suffix);
  inputs->socket_path =
      inputs->runtime_directory.AppendASCII(std::string(200, 'x'));
}

base::FilePath FallbackSocketParent() {
  return base::FilePath(FILE_PATH_LITERAL("/tmp"))
      .AppendASCII("limitless-" + std::to_string(geteuid()));
}

base::FilePath ExpectedFallbackSocket(
    const PreferenceControlTransportWorker::Inputs& inputs) {
  return FallbackSocketParent().AppendASCII(
      "s-" + inputs.profile_id.substr(0, 12) + "-" + inputs.instance_id);
}

Evidence Journey(PreferenceControlTransportWorker::Inputs in,
                 scoped_refptr<TransportCancellationState> c) {
  Evidence e;
  {
    PreferenceControlTransportWorker w(std::move(in), c);
    e.result = w.Prepare();
    e.socket = base::PathExists(e.result.socket_path);
    e.token =
        base::ReadFileToString(e.result.credential_path, &e.token_contents);
    e.descriptor = base::ReadFileToString(e.result.descriptor_path,
                                          &e.descriptor_contents);
    e.socket_stat_ok =
        stat(e.result.socket_path.value().c_str(), &e.socket_stat) == 0;
    e.token_stat_ok =
        stat(e.result.credential_path.value().c_str(), &e.token_stat) == 0;
    e.descriptor_stat_ok =
        stat(e.result.descriptor_path.value().c_str(), &e.descriptor_stat) == 0;
    e.runtime_stat_ok = stat(e.result.socket_path.DirName().value().c_str(),
                             &e.runtime_stat) == 0;
  }
  e.socket_removed = !base::PathExists(e.result.socket_path);
  e.token_removed = !base::PathExists(e.result.credential_path);
  e.descriptor_removed = !base::PathExists(e.result.descriptor_path);
  base::FileEnumerator leftovers(e.result.socket_path.DirName(), false,
                                 base::FileEnumerator::FILES);
  for (base::FilePath path = leftovers.Next(); !path.empty();
       path = leftovers.Next()) {
    const std::string name = path.BaseName().value();
    const std::string pid = base::NumberToString(base::GetCurrentProcId());
    if (name.find(".limitless-publish-" + pid + "-") != std::string::npos ||
        name.find(".limitless-quarantine-" + pid + "-") != std::string::npos) {
      e.residue_free = false;
    }
  }
  return e;
}
Evidence RunWorker(PreferenceControlTransportWorker::Inputs in,
                   scoped_refptr<TransportCancellationState> c) {
  base::test::TestFuture<Evidence> f;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&Journey, std::move(in), c), f.GetCallback());
  return f.Take();
}

FallbackCollisionEvidence RunFallbackCollision(
    PreferenceControlTransportWorker::Inputs first_inputs,
    PreferenceControlTransportWorker::Inputs second_inputs) {
  FallbackCollisionEvidence evidence;
  PreferenceControlTransportWorker first(
      first_inputs, base::MakeRefCounted<TransportCancellationState>());
  evidence.first_result = first.Prepare();
  if (!evidence.first_result.succeeded) {
    return evidence;
  }
  evidence.parent_before_ok =
      stat(evidence.first_result.socket_path.DirName().value().c_str(),
           &evidence.parent_before) == 0;
  {
    PreferenceControlTransportWorker second(
        second_inputs, base::MakeRefCounted<TransportCancellationState>());
    evidence.second_result = second.Prepare();
    second.Cleanup();
  }
  evidence.first_socket_survived =
      base::PathExists(evidence.first_result.socket_path);
  evidence.first_token_survived =
      base::PathExists(evidence.first_result.credential_path);
  evidence.first_descriptor_survived =
      base::PathExists(evidence.first_result.descriptor_path);
  evidence.second_token_absent =
      !base::PathExists(second_inputs.credential_path);
  evidence.second_descriptor_absent =
      !base::PathExists(second_inputs.descriptor_path);
  first.Cleanup();
  evidence.first_socket_removed =
      !base::PathExists(evidence.first_result.socket_path);
  evidence.first_token_removed =
      !base::PathExists(evidence.first_result.credential_path);
  evidence.first_descriptor_removed =
      !base::PathExists(evidence.first_result.descriptor_path);
  evidence.parent_after_ok =
      stat(evidence.first_result.socket_path.DirName().value().c_str(),
           &evidence.parent_after) == 0;
  return evidence;
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     SuccessfulPreparePublishesExactPrivateArtifactsAndCleanup) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto c = base::MakeRefCounted<TransportCancellationState>();
  auto in = Inputs(t.GetPath());
  Evidence e = RunWorker(in, c);
  EXPECT_TRUE(e.result.succeeded);
  EXPECT_TRUE(e.socket);
  EXPECT_TRUE(e.token);
  EXPECT_TRUE(e.descriptor);
  EXPECT_EQ(e.token_contents, "cap");
  ASSERT_TRUE(e.runtime_stat_ok);
  ASSERT_TRUE(e.socket_stat_ok);
  ASSERT_TRUE(e.token_stat_ok);
  ASSERT_TRUE(e.descriptor_stat_ok);
  auto parsed = base::JSONReader::Read(e.descriptor_contents, 0);
  ASSERT_TRUE(parsed.has_value());
  const base::DictValue* dict = parsed->GetIfDict();
  ASSERT_TRUE(dict);
  EXPECT_EQ(dict->FindInt("version"), 1);
  ASSERT_TRUE(dict->FindString("profile_id"));
  EXPECT_EQ(*dict->FindString("profile_id"), "p");
  ASSERT_TRUE(dict->FindString("instance_id"));
  EXPECT_EQ(*dict->FindString("instance_id"), "i");
  ASSERT_TRUE(dict->FindString("socket_path"));
  EXPECT_EQ(*dict->FindString("socket_path"), e.result.socket_path.value());
  ASSERT_TRUE(dict->FindString("credential_path"));
  EXPECT_EQ(*dict->FindString("credential_path"),
            e.result.credential_path.value());
  EXPECT_TRUE(S_ISSOCK(e.socket_stat.st_mode));
  EXPECT_EQ(e.socket_stat.st_uid, geteuid());
  EXPECT_EQ(e.token_stat.st_mode & 0777, 0600);
  EXPECT_EQ(e.descriptor_stat.st_mode & 0777, 0600);
  EXPECT_EQ(e.runtime_stat.st_mode & 0777, 0700);
  EXPECT_TRUE(S_ISDIR(e.runtime_stat.st_mode));
  EXPECT_EQ(e.runtime_stat.st_uid, geteuid());
  EXPECT_TRUE(S_ISREG(e.token_stat.st_mode));
  EXPECT_TRUE(S_ISREG(e.descriptor_stat.st_mode));
  EXPECT_EQ(e.token_stat.st_uid, geteuid());
  EXPECT_EQ(e.descriptor_stat.st_uid, geteuid());
  EXPECT_EQ(dict->FindInt("pid"), base::GetCurrentProcId());
  EXPECT_TRUE(e.socket_removed);
  EXPECT_TRUE(e.token_removed);
  EXPECT_TRUE(e.descriptor_removed);
  EXPECT_TRUE(e.residue_free);
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     CancelBeforePreparePublishesNothingAndReturnsCancelled) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto c = base::MakeRefCounted<TransportCancellationState>();
  c->Cancel();
  Evidence e = RunWorker(Inputs(t.GetPath()), c);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCancelled);
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     ExistingSocketCollisionIsPreserved) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::CreateDirectory(in.runtime_directory));
  ASSERT_TRUE(base::WriteFile(in.socket_path, "foreign"));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCollision);
  EXPECT_TRUE(base::PathExists(in.socket_path));
  std::string foreign;
  ASSERT_TRUE(base::ReadFileToString(in.socket_path, &foreign));
  EXPECT_EQ(foreign, "foreign");
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     NonEnoentPreflightErrorFailsClosed) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::WriteFile(t.GetPath().AppendASCII("r"), "file"));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_FALSE(e.result.succeeded);
  EXPECT_EQ(
      e.result.status,
      PreferenceControlTransportWorker::WorkerPrepareStatus::kRuntimeDirectory);
  EXPECT_FALSE(base::PathExists(in.socket_path));
  EXPECT_FALSE(base::PathExists(in.credential_path));
  EXPECT_FALSE(base::PathExists(in.descriptor_path));
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     DescriptorCollisionPreservesForeignFileAndPublishesNothing) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::CreateDirectory(in.runtime_directory));
  ASSERT_TRUE(base::WriteFile(in.descriptor_path, "foreign"));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_FALSE(e.result.succeeded);
  EXPECT_FALSE(base::PathExists(in.socket_path));
  EXPECT_FALSE(base::PathExists(in.credential_path));
  std::string foreign;
  ASSERT_TRUE(base::ReadFileToString(in.descriptor_path, &foreign));
  EXPECT_EQ(foreign, "foreign");
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     PostBindFailureRollsBackOwnedSocket) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  in.force_post_bind_failure = true;
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kSocket);
  EXPECT_FALSE(base::PathExists(in.socket_path));
  EXPECT_FALSE(base::PathExists(in.credential_path));
  EXPECT_FALSE(base::PathExists(in.descriptor_path));
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     CleanupIsIdempotentAndForeignFilesSurvive) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::CreateDirectory(in.runtime_directory));
  ASSERT_TRUE(base::WriteFile(in.descriptor_path, "foreign"));
  std::string foreign_before;
  ASSERT_TRUE(base::ReadFileToString(in.descriptor_path, &foreign_before));
  auto cleanup_journey = [](PreferenceControlTransportWorker::Inputs input,
                            scoped_refptr<TransportCancellationState> cancel) {
    PreferenceControlTransportWorker worker(std::move(input), cancel);
    auto result = worker.Prepare();
    worker.Cleanup();
    worker.Cleanup();
    return result.succeeded;
  };
  base::test::TestFuture<bool> done;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(cleanup_journey, in,
                     base::MakeRefCounted<TransportCancellationState>()),
      done.GetCallback());
  EXPECT_FALSE(done.Take());
  std::string foreign_after;
  ASSERT_TRUE(base::ReadFileToString(in.descriptor_path, &foreign_after));
  EXPECT_EQ(foreign_after, foreign_before);
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     PrepareCanBeRepeatedAfterCleanupWithoutTempArtifacts) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  base::test::TestFuture<bool> future;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(
          [](PreferenceControlTransportWorker::Inputs in) {
            auto cancellation =
                base::MakeRefCounted<TransportCancellationState>();
            {
              PreferenceControlTransportWorker worker(in, cancellation);
              if (!worker.Prepare().succeeded) {
                return false;
              }
              base::FileEnumerator entries(in.runtime_directory, false,
                                           base::FileEnumerator::FILES);
              for (base::FilePath path = entries.Next(); !path.empty();
                   path = entries.Next()) {
                if (path.BaseName().value().find(".limitless-publish-") !=
                    std::string::npos) {
                  return false;
                }
              }
              worker.Cleanup();
            }
            PreferenceControlTransportWorker second(in, cancellation);
            bool ok = second.Prepare().succeeded;
            second.Cleanup();
            return ok;
          },
          Inputs(t.GetPath())),
      future.GetCallback());
  EXPECT_TRUE(future.Take());
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     CleanupPreservesReplacedForeignArtifacts) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  base::test::TestFuture<bool> future;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(
          [](PreferenceControlTransportWorker::Inputs in) {
            auto cancellation =
                base::MakeRefCounted<TransportCancellationState>();
            PreferenceControlTransportWorker worker(in, cancellation);
            if (!worker.Prepare().succeeded) {
              return false;
            }
            bool unlinked = unlink(in.socket_path.value().c_str()) == 0 &&
                            unlink(in.credential_path.value().c_str()) == 0 &&
                            unlink(in.descriptor_path.value().c_str()) == 0;
            bool written =
                unlinked && base::WriteFile(in.socket_path, "socket-foreign") &&
                base::WriteFile(in.credential_path, "token-foreign") &&
                base::WriteFile(in.descriptor_path, "descriptor-foreign");
            worker.Cleanup();
            worker.Cleanup();
            std::string a, b, c;
            return written && base::ReadFileToString(in.socket_path, &a) &&
                   base::ReadFileToString(in.credential_path, &b) &&
                   base::ReadFileToString(in.descriptor_path, &c) &&
                   a == "socket-foreign" && b == "token-foreign" &&
                   c == "descriptor-foreign";
          },
          Inputs(t.GetPath())),
      future.GetCallback());
  EXPECT_TRUE(future.Take());
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     OverlongSocketUsesPrivateShortPathAndCleansOnlyOwnedArtifacts) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ConfigureLongSocketFallback(&in, t.GetPath());
  const base::FilePath expected_socket = ExpectedFallbackSocket(in);
  const base::FilePath expected_descriptor = in.descriptor_path;
  ASSERT_FALSE(base::PathExists(expected_socket));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  ASSERT_TRUE(e.result.succeeded);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kSuccess);
  EXPECT_EQ(e.result.socket_path, expected_socket);
  EXPECT_EQ(e.result.descriptor_path, expected_descriptor);
  EXPECT_LT(e.result.socket_path.value().size(), sizeof(sockaddr_un::sun_path));
  EXPECT_TRUE(e.socket);
  EXPECT_TRUE(e.token);
  EXPECT_TRUE(e.descriptor);
  EXPECT_EQ(e.token_contents, "cap");
  auto descriptor = base::JSONReader::Read(e.descriptor_contents, 0);
  ASSERT_TRUE(descriptor.has_value());
  const base::DictValue* descriptor_dict = descriptor->GetIfDict();
  ASSERT_TRUE(descriptor_dict);
  ASSERT_TRUE(descriptor_dict->FindString("socket_path"));
  EXPECT_EQ(*descriptor_dict->FindString("socket_path"),
            expected_socket.value());
  ASSERT_TRUE(e.runtime_stat_ok);
  EXPECT_TRUE(S_ISDIR(e.runtime_stat.st_mode));
  EXPECT_EQ(e.runtime_stat.st_uid, geteuid());
  EXPECT_EQ(e.runtime_stat.st_mode & 0777, 0700);
  ASSERT_TRUE(e.socket_stat_ok);
  EXPECT_TRUE(S_ISSOCK(e.socket_stat.st_mode));
  EXPECT_EQ(e.socket_stat.st_uid, geteuid());
  EXPECT_TRUE(e.socket_removed);
  EXPECT_TRUE(e.token_removed);
  EXPECT_TRUE(e.descriptor_removed);
  EXPECT_TRUE(e.residue_free);

  struct stat retained_parent = {};
  ASSERT_EQ(stat(FallbackSocketParent().value().c_str(), &retained_parent), 0);
  EXPECT_TRUE(S_ISDIR(retained_parent.st_mode));
  EXPECT_EQ(retained_parent.st_uid, geteuid());
  EXPECT_EQ(retained_parent.st_mode & 0777, 0700);
  EXPECT_EQ(retained_parent.st_dev, e.runtime_stat.st_dev);
  EXPECT_EQ(retained_parent.st_ino, e.runtime_stat.st_ino);
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     FallbackCollisionPreservesFirstWorkerAndSharedParent) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir first_root;
  base::ScopedTempDir second_root;
  ASSERT_TRUE(first_root.CreateUniqueTempDir());
  ASSERT_TRUE(second_root.CreateUniqueTempDir());
  auto first_inputs = Inputs(first_root.GetPath());
  auto second_inputs = Inputs(second_root.GetPath());
  ConfigureLongSocketFallback(&first_inputs, first_root.GetPath());
  ConfigureLongSocketFallback(&second_inputs, second_root.GetPath());
  second_inputs.profile_id = first_inputs.profile_id;
  second_inputs.instance_id = first_inputs.instance_id;
  const base::FilePath expected_socket = ExpectedFallbackSocket(first_inputs);
  ASSERT_EQ(ExpectedFallbackSocket(second_inputs), expected_socket);
  ASSERT_FALSE(base::PathExists(expected_socket));

  base::test::TestFuture<FallbackCollisionEvidence> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&RunFallbackCollision, std::move(first_inputs),
                     std::move(second_inputs)),
      result.GetCallback());
  FallbackCollisionEvidence evidence = result.Take();

  ASSERT_TRUE(evidence.first_result.succeeded);
  EXPECT_EQ(evidence.first_result.socket_path, expected_socket);
  EXPECT_FALSE(evidence.second_result.succeeded);
  EXPECT_EQ(evidence.second_result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCollision);
  EXPECT_EQ(evidence.second_result.socket_path, expected_socket);
  EXPECT_TRUE(evidence.first_socket_survived);
  EXPECT_TRUE(evidence.first_token_survived);
  EXPECT_TRUE(evidence.first_descriptor_survived);
  EXPECT_TRUE(evidence.second_token_absent);
  EXPECT_TRUE(evidence.second_descriptor_absent);
  EXPECT_TRUE(evidence.first_socket_removed);
  EXPECT_TRUE(evidence.first_token_removed);
  EXPECT_TRUE(evidence.first_descriptor_removed);
  ASSERT_TRUE(evidence.parent_before_ok);
  ASSERT_TRUE(evidence.parent_after_ok);
  EXPECT_TRUE(S_ISDIR(evidence.parent_after.st_mode));
  EXPECT_EQ(evidence.parent_after.st_uid, geteuid());
  EXPECT_EQ(evidence.parent_after.st_mode & 0777, 0700);
  EXPECT_EQ(evidence.parent_after.st_dev, evidence.parent_before.st_dev);
  EXPECT_EQ(evidence.parent_after.st_ino, evidence.parent_before.st_ino);
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     CredentialHookCancelsAndRollsBack) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto cancel = base::MakeRefCounted<TransportCancellationState>();
  auto in = Inputs(t.GetPath());
  in.before_credential_publication_for_testing =
      base::BindRepeating(&CancelState, cancel);
  Evidence e = RunWorker(in, cancel);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCancelled);
  EXPECT_FALSE(e.socket);
  EXPECT_FALSE(e.token);
  EXPECT_TRUE(e.residue_free);
  EXPECT_FALSE(e.descriptor);
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     DescriptorHookCollisionPreservesForeign) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  in.before_descriptor_publication_for_testing =
      base::BindRepeating(&WriteForeignDescriptor, in.descriptor_path);
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  std::string foreign;
  ASSERT_TRUE(base::ReadFileToString(in.descriptor_path, &foreign));
  EXPECT_EQ(foreign, "foreign-descriptor");
  EXPECT_FALSE(e.result.succeeded);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCollision);
  EXPECT_FALSE(e.socket);
  EXPECT_FALSE(e.token);
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     RuntimeDirectorySwapDuringBindLeavesNoSocket) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  auto old_path = t.GetPath().AppendASCII("r-old");
  bool hook_succeeded = false;
  in.before_socket_bind_for_testing = base::BindRepeating(
      &SwapRuntimeDirectory, in.runtime_directory, old_path, &hook_succeeded);
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_TRUE(hook_succeeded);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kSocket);
  EXPECT_FALSE(base::PathExists(in.socket_path));
  EXPECT_FALSE(base::PathExists(in.credential_path));
  EXPECT_FALSE(base::PathExists(in.descriptor_path));
  EXPECT_TRUE(base::DirectoryExists(old_path));
  struct stat foreign_socket = {};
  ASSERT_EQ(stat(old_path.AppendASCII("s").value().c_str(), &foreign_socket),
            0);
  EXPECT_TRUE(S_ISSOCK(foreign_socket.st_mode));
  EXPECT_FALSE(base::PathExists(old_path.AppendASCII("t")));
  EXPECT_FALSE(base::PathExists(old_path.AppendASCII("d")));
  base::FileEnumerator old_entries(old_path, false,
                                   base::FileEnumerator::FILES);
  for (base::FilePath path = old_entries.Next(); !path.empty();
       path = old_entries.Next()) {
    EXPECT_EQ(path.BaseName().value().find(".limitless-publish-"),
              std::string::npos);
    EXPECT_EQ(path.BaseName().value().find(".limitless-quarantine-"),
              std::string::npos);
  }
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     PendingRequestCompletionWakesWaiterWithExactResponse) {
  base::test::TaskEnvironment env;
  auto request = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                      base::Seconds(2));
  base::test::TestFuture<std::string> future;
  base::WaitableEvent started;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](scoped_refptr<PendingRequest> pending,
             base::WaitableEvent* started) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            started->Signal();
            std::string response;
            pending->Wait(&response);
            return response;
          },
          request, &started),
      future.GetCallback());
  started.Wait();
  EXPECT_TRUE(request->TryBeginHandling());
  EXPECT_TRUE(request->Complete("ok"));
  EXPECT_EQ(future.Take(), "ok");
  EXPECT_FALSE(request->Complete("late"));
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     PendingRequestDeadlineCancelsQueuedHandling) {
  base::test::TaskEnvironment env;
  auto p = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                base::Seconds(2));
  base::test::TestFuture<bool> f;
  base::WaitableEvent started;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](scoped_refptr<PendingRequest> r, base::WaitableEvent* started) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            started->Signal();
            std::string s;
            return r->Wait(&s);
          },
          p, &started),
      f.GetCallback());
  started.Wait();
  EXPECT_FALSE(f.Take());
  EXPECT_FALSE(p->TryBeginHandling());
  EXPECT_FALSE(p->Complete("late"));
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     CancellationWakesDirectlyInstalledPendingRequest) {
  base::test::TaskEnvironment env;
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  base::ScopedFD connection(fds[0]);
  base::ScopedFD peer(fds[1]);
  auto state = base::MakeRefCounted<TransportCancellationState>();
  auto pending = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                      base::Seconds(2));
  const uint64_t token = state->InstallAcceptedFd(connection.get());
  ASSERT_NE(token, 0u);
  ASSERT_TRUE(state->InstallPendingRequest(token, pending));

  base::test::TestFuture<bool> wait_result;
  base::WaitableEvent wait_started;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](scoped_refptr<PendingRequest> request,
             base::WaitableEvent* started) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            started->Signal();
            std::string response;
            return request->Wait(&response);
          },
          pending, &wait_started),
      wait_result.GetCallback());
  wait_started.Wait();

  state->Cancel();
  EXPECT_FALSE(wait_result.Take());
  EXPECT_FALSE(pending->TryBeginHandling());
  EXPECT_FALSE(pending->Complete("late"));
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     StaleRegistrationCannotClearNewerConnection) {
  base::test::TaskEnvironment env;
  int first[2], second[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, first), 0);
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, second), 0);
  base::ScopedFD first_connection(first[0]);
  base::ScopedFD first_peer(first[1]);
  base::ScopedFD second_connection(second[0]);
  base::ScopedFD second_peer(second[1]);
  auto state = base::MakeRefCounted<TransportCancellationState>();
  auto pending1 = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                       base::Seconds(2));
  auto pending2 = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                       base::Seconds(2));
  const uint64_t token1 = state->InstallAcceptedFd(first_connection.get());
  ASSERT_NE(token1, 0u);
  ASSERT_TRUE(state->InstallPendingRequest(token1, pending1));
  const uint64_t token2 = state->InstallAcceptedFd(second_connection.get());
  ASSERT_NE(token2, 0u);
  ASSERT_NE(token1, token2);

  base::test::TestFuture<bool> first_eof;
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
                                               base::BindOnce(
                                                   [](int fd) {
                                                     char byte;
                                                     return read(fd, &byte,
                                                                 1) == 0;
                                                   },
                                                   first_peer.get()),
                                               first_eof.GetCallback());
  EXPECT_TRUE(first_eof.Take());
  EXPECT_FALSE(pending1->TryBeginHandling());
  EXPECT_FALSE(pending1->Complete("superseded"));

  ASSERT_TRUE(state->InstallPendingRequest(token2, pending2));
  state->ClearActive(token1);

  base::test::TestFuture<bool> pending2_result;
  base::WaitableEvent pending2_started;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](scoped_refptr<PendingRequest> request,
             base::WaitableEvent* started) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            started->Signal();
            std::string response;
            return request->Wait(&response);
          },
          pending2, &pending2_started),
      pending2_result.GetCallback());
  pending2_started.Wait();

  base::test::TestFuture<bool> second_eof;
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
                                               base::BindOnce(
                                                   [](int fd) {
                                                     char byte;
                                                     return read(fd, &byte,
                                                                 1) == 0;
                                                   },
                                                   second_peer.get()),
                                               second_eof.GetCallback());

  state->Cancel();
  EXPECT_TRUE(second_eof.Take());
  EXPECT_FALSE(pending2_result.Take());
  EXPECT_FALSE(pending2->TryBeginHandling());
  EXPECT_FALSE(pending2->Complete("late"));
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     ExactClearCancelsPendingRequestAndStaleClearDoesNothing) {
  base::test::TaskEnvironment env;
  int superseded[2], active[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, superseded), 0);
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, active), 0);
  base::ScopedFD superseded_connection(superseded[0]);
  base::ScopedFD superseded_peer(superseded[1]);
  base::ScopedFD active_connection(active[0]);
  base::ScopedFD active_peer(active[1]);
  auto state = base::MakeRefCounted<TransportCancellationState>();
  const uint64_t stale_token =
      state->InstallAcceptedFd(superseded_connection.get());
  ASSERT_NE(stale_token, 0u);
  const uint64_t active_token =
      state->InstallAcceptedFd(active_connection.get());
  ASSERT_NE(active_token, 0u);
  ASSERT_NE(stale_token, active_token);

  auto pending = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                      base::Seconds(2));
  ASSERT_TRUE(state->InstallPendingRequest(active_token, pending));
  base::test::TestFuture<bool> wait_result;
  base::WaitableEvent wait_started;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](scoped_refptr<PendingRequest> request,
             base::WaitableEvent* started) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            started->Signal();
            std::string response;
            return request->Wait(&response);
          },
          pending, &wait_started),
      wait_result.GetCallback());
  wait_started.Wait();

  state->ClearActive(stale_token);
  const char sent = 'x';
  ASSERT_EQ(send(active_connection.get(), &sent, 1, MSG_NOSIGNAL), 1);
  char received = 0;
  ASSERT_EQ(read(active_peer.get(), &received, 1), 1);
  EXPECT_EQ(received, sent);

  base::test::TestFuture<bool> active_eof;
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
                                               base::BindOnce(
                                                   [](int fd) {
                                                     char byte;
                                                     return read(fd, &byte,
                                                                 1) == 0;
                                                   },
                                                   active_peer.get()),
                                               active_eof.GetCallback());
  state->ClearActive(active_token);
  EXPECT_TRUE(active_eof.Take());
  EXPECT_FALSE(wait_result.Take());
  EXPECT_FALSE(pending->TryBeginHandling());
  EXPECT_FALSE(pending->Complete("late"));
}

TEST(LimitlessPreferenceControlTransportWorkerTest,
     InvalidAcceptedFdDoesNotReplaceValidRegistration) {
  base::test::TaskEnvironment env;
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  base::ScopedFD connection(fds[0]);
  base::ScopedFD peer(fds[1]);
  auto state = base::MakeRefCounted<TransportCancellationState>();
  auto pending = base::MakeRefCounted<PendingRequest>(base::TimeTicks::Now() +
                                                      base::Seconds(2));
  const uint64_t token = state->InstallAcceptedFd(connection.get());
  ASSERT_NE(token, 0u);
  ASSERT_TRUE(state->InstallPendingRequest(token, pending));
  EXPECT_EQ(state->InstallAcceptedFd(-1), 0u);

  base::test::TestFuture<bool> wait_result;
  base::WaitableEvent wait_started;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(
          [](scoped_refptr<PendingRequest> request,
             base::WaitableEvent* started) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            started->Signal();
            std::string response;
            return request->Wait(&response);
          },
          pending, &wait_started),
      wait_result.GetCallback());
  wait_started.Wait();

  base::test::TestFuture<bool> peer_eof;
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
                                               base::BindOnce(
                                                   [](int fd) {
                                                     char byte;
                                                     return read(fd, &byte,
                                                                 1) == 0;
                                                   },
                                                   peer.get()),
                                               peer_eof.GetCallback());
  state->Cancel();
  EXPECT_TRUE(peer_eof.Take());
  EXPECT_FALSE(wait_result.Take());
  EXPECT_FALSE(pending->TryBeginHandling());
  EXPECT_FALSE(pending->Complete("late"));
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       SameUidFramedRequestRoundTripsThroughOriginSequence) {
  constexpr std::string_view kRequest = "request-body";
  constexpr std::string_view kResponse = "response-body";
  base::test::TestFuture<bool, bool> handler_result;
  auto handler_callback = handler_result.GetSequenceBoundRepeatingCallback();
  auto origin_task_runner = origin_thread_.task_runner();
  StartWorker(NewInputs(),
              base::BindRepeating(
                  [](scoped_refptr<base::SequencedTaskRunner> expected_origin,
                     base::RepeatingCallback<void(bool, bool)> report,
                     std::string payload, base::TimeTicks deadline,
                     scoped_refptr<PendingRequest> pending) {
                    const bool on_origin =
                        expected_origin->RunsTasksInCurrentSequence();
                    const bool completed =
                        payload == "request-body" &&
                        pending->Complete(std::string("response-body"));
                    report.Run(on_origin, completed);
                  },
                  std::move(origin_task_runner), std::move(handler_callback)));

  const ClientResult client = RunClient<ClientResult>(
      &ExchangeFrame, prepare_result_.Get().socket_path, std::string(kRequest));
  ASSERT_TRUE(handler_result.Wait());
  EXPECT_TRUE(handler_result.Get<0>());
  EXPECT_TRUE(handler_result.Get<1>());
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.request_sent);
  EXPECT_TRUE(client.response_received);
  EXPECT_EQ(client.response, kResponse);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       OversizedHeaderReturnsExactErrorWithoutOriginDispatch) {
  std::atomic<int> dispatch_count{0};
  StartWorker(NewInputs(), base::BindRepeating(
                               [](std::atomic<int>* count, std::string payload,
                                  base::TimeTicks deadline,
                                  scoped_refptr<PendingRequest> pending) {
                                 count->fetch_add(1, std::memory_order_relaxed);
                               },
                               &dispatch_count));

  const ClientResult client = RunClient<ClientResult>(
      &SendOversizedHeader, prepare_result_.Get().socket_path);
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.request_sent);
  ASSERT_TRUE(client.response_received);
  EXPECT_EQ(client.response, SerializePreferenceRequestParseError(
                                 PreferenceRequestParseStatus::kOversized));
  EXPECT_EQ(dispatch_count.load(std::memory_order_relaxed), 0);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       TruncatedBodyClosesConnectionWithoutOriginDispatch) {
  std::atomic<int> dispatch_count{0};
  StartWorker(NewInputs(), base::BindRepeating(
                               [](std::atomic<int>* count, std::string payload,
                                  base::TimeTicks deadline,
                                  scoped_refptr<PendingRequest> pending) {
                                 count->fetch_add(1, std::memory_order_relaxed);
                               },
                               &dispatch_count));

  const ClientResult client = RunClient<ClientResult>(
      &SendTruncatedBody, prepare_result_.Get().socket_path, false);
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.request_sent);
  EXPECT_TRUE(client.eof);
  EXPECT_EQ(dispatch_count.load(std::memory_order_relaxed), 0);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       SlowDripCannotExtendAbsoluteInputDeadline) {
  constexpr size_t kCompletePayloadSize = 12;
  std::atomic<int> dispatch_count{0};
  auto inputs = NewInputs();
  inputs.io_timeout_for_testing = base::Milliseconds(200);
  StartWorker(
      std::move(inputs),
      base::BindRepeating(
          [](std::atomic<int>* count, std::string payload,
             base::TimeTicks deadline, scoped_refptr<PendingRequest> pending) {
            count->fetch_add(1, std::memory_order_relaxed);
          },
          &dispatch_count));

  const SlowDripResult client = RunClient<SlowDripResult>(
      &DripPayloadPastDeadline, prepare_result_.Get().socket_path);
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.header_sent);
  EXPECT_TRUE(client.eof);
  EXPECT_LT(client.payload_bytes_sent, kCompletePayloadSize);
  EXPECT_LT(client.elapsed, base::Milliseconds(900));
  EXPECT_EQ(dispatch_count.load(std::memory_order_relaxed), 0);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       CancellationWakesIdlePollAndRemovesOwnedArtifacts) {
  base::WaitableEvent entering_accept_wait;
  auto inputs = NewInputs();
  const base::FilePath socket_path = inputs.socket_path;
  const base::FilePath credential_path = inputs.credential_path;
  const base::FilePath descriptor_path = inputs.descriptor_path;
  inputs.entering_accept_wait_for_testing =
      base::BindRepeating(&SignalEvent, &entering_accept_wait);
  StartWorker(std::move(inputs), base::DoNothing());
  ASSERT_TRUE(TimedWait(&entering_accept_wait));

  const base::TimeTicks cancel_started = base::TimeTicks::Now();
  CancelAndWaitForRun();
  EXPECT_LT(base::TimeTicks::Now() - cancel_started, base::Seconds(3));
  EXPECT_FALSE(base::PathExists(socket_path));
  EXPECT_FALSE(base::PathExists(credential_path));
  EXPECT_FALSE(base::PathExists(descriptor_path));
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       CancellationShutsDownAcceptedPartialFrame) {
  base::WaitableEvent accepted;
  auto inputs = NewInputs();
  inputs.accepted_fd_for_testing =
      base::BindRepeating(&SignalAcceptedEvent, &accepted);
  StartWorker(std::move(inputs), base::DoNothing());

  base::test::TestFuture<ClientResult> client_result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&SendTruncatedBody, prepare_result_.Get().socket_path,
                     true),
      client_result.GetCallback());
  ASSERT_TRUE(TimedWait(&accepted));
  cancellation_->Cancel();
  ASSERT_TRUE(client_result.Wait());
  const ClientResult client = client_result.Take();
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.request_sent);
  EXPECT_TRUE(client.eof);
  ASSERT_TRUE(run_complete_.Wait());
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       AcceptedDescriptorIsBlockingAndCloseOnExec) {
  AcceptedFdEvidence evidence;
  auto inputs = NewInputs();
  inputs.accepted_fd_for_testing = base::BindRepeating(
      &AcceptedFdEvidence::Inspect, base::Unretained(&evidence));
  StartWorker(std::move(inputs), base::DoNothing());

  base::test::TestFuture<ClientResult> client_result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ConnectAndAwaitEof, prepare_result_.Get().socket_path),
      client_result.GetCallback());
  ASSERT_TRUE(TimedWait(&evidence.inspected));
  EXPECT_TRUE(evidence.close_on_exec.load(std::memory_order_acquire));
  EXPECT_TRUE(evidence.blocking.load(std::memory_order_acquire));
  cancellation_->Cancel();
  ASSERT_TRUE(client_result.Wait());
  EXPECT_TRUE(client_result.Get().connected);
  EXPECT_TRUE(client_result.Get().eof);
  ASSERT_TRUE(run_complete_.Wait());
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       QueuedOriginHandlerCannotClaimAfterAbsoluteDeadline) {
  base::WaitableEvent origin_blocked;
  base::WaitableEvent release_origin;
  ASSERT_TRUE(origin_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WaitableEvent* entered, base::WaitableEvent* release) {
            base::ScopedAllowBaseSyncPrimitivesForTesting allow_sync;
            entered->Signal();
            EXPECT_TRUE(release->TimedWait(kRunTestTimeout));
          },
          &origin_blocked, &release_origin)));
  ASSERT_TRUE(TimedWait(&origin_blocked));
  base::ScopedClosureRunner release_guard(
      base::BindOnce(&SignalEvent, &release_origin));

  std::atomic<int> dispatch_count{0};
  std::atomic<int> mutation_count{0};
  auto inputs = NewInputs();
  inputs.io_timeout_for_testing = base::Milliseconds(150);
  StartWorker(std::move(inputs),
              base::BindRepeating(
                  [](std::atomic<int>* dispatches, std::atomic<int>* mutations,
                     std::string payload, base::TimeTicks deadline,
                     scoped_refptr<PendingRequest> pending) {
                    dispatches->fetch_add(1, std::memory_order_relaxed);
                    mutations->fetch_add(1, std::memory_order_relaxed);
                    pending->Complete("too-late");
                  },
                  &dispatch_count, &mutation_count));

  const ClientResult client = RunClient<ClientResult>(
      &ExchangeFrame, prepare_result_.Get().socket_path, std::string("queued"));
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.request_sent);
  EXPECT_FALSE(client.response_received);
  EXPECT_TRUE(client.eof);
  release_guard.RunAndReset();
  base::test::TestFuture<void> origin_drained;
  ASSERT_TRUE(origin_thread_.task_runner()->PostTask(
      FROM_HERE, origin_drained.GetSequenceBoundCallback()));
  ASSERT_TRUE(origin_drained.Wait());
  EXPECT_EQ(dispatch_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(mutation_count.load(std::memory_order_relaxed), 0);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       WrongCapabilityReturnsUnauthenticatedWithoutMutation) {
  constexpr std::string_view kRequest =
      R"json({"version":1,"request_id":"33333333-3333-4333-8333-333333333333","operation":"write","target":{"profile_id":"11111111-1111-4111-8111-111111111111","instance_id":"22222222-2222-4222-8222-222222222222"},"actor":{"type":"agent","id":"worker-run-test"},"authentication":{"scheme":"capability","credential":"wrong-capability"},"key":"rail_density","value":"comfortable"})json";
  ProtocolBackend backend;
  base::test::TestFuture<bool, bool> handler_result;
  auto handler_callback = handler_result.GetSequenceBoundRepeatingCallback();
  StartWorker(
      NewInputs(),
      base::BindRepeating(
          [](ProtocolBackend* backend,
             base::RepeatingCallback<void(bool, bool)> report,
             std::string payload, base::TimeTicks deadline,
             scoped_refptr<PendingRequest> pending) {
            PreferenceRequestParseResult parsed =
                ParsePreferenceRequest(payload);
            if (!parsed.succeeded()) {
              report.Run(false,
                         pending->Complete(SerializePreferenceRequestParseError(
                             parsed.status)));
              return;
            }
            PreferenceControlService service(backend);
            const bool completed =
                pending->Complete(SerializePreferenceControlResult(
                    service.Execute(*parsed.request)));
            report.Run(true, completed);
          },
          &backend, std::move(handler_callback)));

  const ClientResult client = RunClient<ClientResult>(
      &ExchangeFrame, prepare_result_.Get().socket_path, std::string(kRequest));
  ASSERT_TRUE(handler_result.Wait());
  EXPECT_TRUE(handler_result.Get<0>());
  EXPECT_TRUE(handler_result.Get<1>());
  ASSERT_TRUE(client.response_received);
  std::optional<base::Value> response =
      base::JSONReader::Read(client.response, base::JSON_PARSE_RFC);
  ASSERT_TRUE(response.has_value());
  const base::DictValue* response_dict = response->GetIfDict();
  ASSERT_TRUE(response_dict);
  ASSERT_TRUE(response_dict->FindString("status"));
  EXPECT_EQ(*response_dict->FindString("status"), "unauthenticated");
  ASSERT_TRUE(response_dict->FindString("request_id"));
  EXPECT_EQ(*response_dict->FindString("request_id"),
            "33333333-3333-4333-8333-333333333333");
  EXPECT_EQ(backend.mutation_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(backend.audit_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(backend.successful_audit_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(backend.last_audit_status.load(std::memory_order_relaxed),
            PreferenceControlStatus::kUnauthenticated);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       CancellationReleasesClaimedPendingAndRejectsLateCompletion) {
  base::test::TestFuture<scoped_refptr<PendingRequest>> handler_result;
  auto handler_callback = handler_result.GetSequenceBoundRepeatingCallback();
  StartWorker(
      NewInputs(),
      base::BindRepeating(
          [](base::RepeatingCallback<void(scoped_refptr<PendingRequest>)>
                 report,
             std::string payload, base::TimeTicks deadline,
             scoped_refptr<PendingRequest> pending) { report.Run(pending); },
          std::move(handler_callback)));

  base::test::TestFuture<ClientResult> client_result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ExchangeFrame, prepare_result_.Get().socket_path,
                     std::string("claim-and-wait")),
      client_result.GetCallback());
  ASSERT_TRUE(handler_result.Wait());
  scoped_refptr<PendingRequest> pending = handler_result.Get<0>();
  cancellation_->Cancel();
  ASSERT_TRUE(client_result.Wait());
  EXPECT_TRUE(client_result.Get().connected);
  EXPECT_TRUE(client_result.Get().request_sent);
  EXPECT_TRUE(client_result.Get().eof);
  EXPECT_FALSE(pending->Complete("late"));
  ASSERT_TRUE(run_complete_.Wait());
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       OversizedHandlerResponseIsDropped) {
  base::test::TestFuture<bool> handler_result;
  auto handler_callback = handler_result.GetSequenceBoundRepeatingCallback();
  StartWorker(
      NewInputs(),
      base::BindRepeating(
          [](base::RepeatingCallback<void(bool)> report, std::string payload,
             base::TimeTicks deadline, scoped_refptr<PendingRequest> pending) {
            const bool completed = pending->Complete(
                std::string(kMaximumPreferenceTransportResponseBytes + 1, 'x'));
            report.Run(completed);
          },
          std::move(handler_callback)));

  const ClientResult client =
      RunClient<ClientResult>(&ExchangeFrame, prepare_result_.Get().socket_path,
                              std::string("large-response"));
  ASSERT_TRUE(handler_result.Wait());
  EXPECT_TRUE(handler_result.Get<0>());
  EXPECT_TRUE(client.connected);
  EXPECT_TRUE(client.request_sent);
  EXPECT_FALSE(client.response_received);
  EXPECT_TRUE(client.eof);
  CancelAndWaitForRun();
}

TEST_F(PreferenceControlTransportWorkerRunTest,
       RejectedOriginPostsDropEachRequestAndRemainCancellable) {
  std::atomic<int> dispatch_count{0};
  auto rejecting_origin = base::MakeRefCounted<RejectingSequencedTaskRunner>();
  auto inputs = NewInputs();
  inputs.io_timeout_for_testing = base::Seconds(4);
  const base::FilePath socket_path = inputs.socket_path;
  const base::FilePath credential_path = inputs.credential_path;
  const base::FilePath descriptor_path = inputs.descriptor_path;
  StartWorker(
      std::move(inputs),
      base::BindRepeating(
          [](std::atomic<int>* count, std::string payload,
             base::TimeTicks deadline, scoped_refptr<PendingRequest> pending) {
            count->fetch_add(1, std::memory_order_relaxed);
            pending->Complete("unexpected");
          },
          &dispatch_count),
      rejecting_origin);

  const base::TimeTicks first_started = base::TimeTicks::Now();
  const ClientResult first = RunClient<ClientResult>(
      &ExchangeFrame, socket_path, std::string("first-complete-request"));
  const base::TimeDelta first_elapsed = base::TimeTicks::Now() - first_started;
  const base::TimeTicks second_started = base::TimeTicks::Now();
  const ClientResult second = RunClient<ClientResult>(
      &ExchangeFrame, socket_path, std::string("second-complete-request"));
  const base::TimeDelta second_elapsed =
      base::TimeTicks::Now() - second_started;
  EXPECT_TRUE(first.connected);
  EXPECT_TRUE(first.request_sent);
  EXPECT_TRUE(first.eof);
  EXPECT_FALSE(first.response_received);
  EXPECT_TRUE(second.connected);
  EXPECT_TRUE(second.request_sent);
  EXPECT_TRUE(second.eof);
  EXPECT_FALSE(second.response_received);
  EXPECT_EQ(dispatch_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(
      rejecting_origin->post_attempt_count.load(std::memory_order_relaxed), 2);
  EXPECT_LT(first_elapsed, base::Seconds(2));
  EXPECT_LT(second_elapsed, base::Seconds(2));

  CancelAndWaitForRun();
  EXPECT_FALSE(base::PathExists(socket_path));
  EXPECT_FALSE(base::PathExists(credential_path));
  EXPECT_FALSE(base::PathExists(descriptor_path));
}

}  // namespace
}  // namespace limitless
