#include "limitless_preference_control_transport_worker.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/process/process_handle.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "limitless_preference_control_protocol.h"
#include "limitless_preference_control_service.h"

namespace {

constexpr base::TimeDelta kTransportIoTimeout = base::Seconds(2);

bool ConfigureDeadline(int fd, base::TimeTicks deadline) {
  base::TimeDelta remaining = deadline - base::TimeTicks::Now();
  if (!remaining.is_positive()) {
    return false;
  }
  timeval timeout = {};
  const int64_t micros = std::max<int64_t>(1, remaining.InMicroseconds());
  timeout.tv_sec = micros / 1000000;
  timeout.tv_usec = micros % 1000000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
             0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
             0;
}

bool ReadExactly(int fd, base::span<uint8_t> output, base::TimeTicks deadline) {
  while (!output.empty()) {
    if (!ConfigureDeadline(fd, deadline)) {
      return false;
    }
    ssize_t n = read(fd, output.data(), output.size());
    if (n == 0) {
      return false;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    output = output.subspan(static_cast<size_t>(n));
  }
  return true;
}

bool WriteExactly(int fd,
                  base::span<const uint8_t> input,
                  base::TimeTicks deadline) {
  while (!input.empty()) {
    if (!ConfigureDeadline(fd, deadline)) {
      return false;
    }
    ssize_t n = send(fd, input.data(), input.size(), MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    input = input.subspan(static_cast<size_t>(n));
  }
  return true;
}

bool IsCurrentUserPeer(int fd) {
  uid_t uid = 0;
  gid_t gid = 0;
  return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
}

bool ConfigureAccepted(int fd) {
  int fd_flags = fcntl(fd, F_GETFD);
  int status_flags = fcntl(fd, F_GETFL);
  return fd_flags >= 0 && status_flags >= 0 &&
         fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0 &&
         fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) == 0;
}

bool SendFramedResponse(int fd,
                        std::string_view response,
                        base::TimeTicks deadline) {
  if (response.size() > limitless::kMaximumPreferenceTransportResponseBytes ||
      response.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint32_t frame_size = htonl(static_cast<uint32_t>(response.size()));
  return WriteExactly(fd, base::byte_span_from_ref(frame_size), deadline) &&
         WriteExactly(fd, base::as_byte_span(response), deadline);
}

constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kPrivateDirectoryMode = S_IRWXU;

enum class PathCheck { kAbsent, kExists, kError };

enum class PublishResult { kSuccess, kCollision, kFailure };

bool FsyncNoIntr(int fd) {
  for (;;) {
    if (fsync(fd) == 0) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

PathCheck CheckPathAt(int directory_fd, const std::string& basename) {
  struct stat st = {};
  if (fstatat(directory_fd, basename.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
    return PathCheck::kExists;
  }
  return errno == ENOENT ? PathCheck::kAbsent : PathCheck::kError;
}

bool GetArtifactBasename(const base::FilePath& runtime_directory,
                         const base::FilePath& artifact_path,
                         std::string* basename) {
  const base::FilePath artifact_basename = artifact_path.BaseName();
  if (artifact_path.DirName() != runtime_directory ||
      artifact_basename.empty() || artifact_basename.value() == "." ||
      artifact_basename.value() == "..") {
    return false;
  }
  *basename = artifact_basename.value();
  return true;
}

bool ReadArtifactIdentityAt(int directory_fd,
                            const std::string& basename,
                            mode_t expected_type,
                            limitless::ArtifactIdentity* identity) {
  struct stat st = {};
  if (fstatat(directory_fd, basename.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      (st.st_mode & S_IFMT) != expected_type || st.st_uid != geteuid()) {
    return false;
  }
  identity->dev = st.st_dev;
  identity->ino = st.st_ino;
  return true;
}

bool StatMatchesIdentity(const struct stat& st,
                         mode_t expected_type,
                         const limitless::ArtifactIdentity& identity) {
  return (st.st_mode & S_IFMT) == expected_type && st.st_uid == geteuid() &&
         st.st_dev == identity.dev && st.st_ino == identity.ino;
}

bool ArtifactMatchesAt(int directory_fd,
                       const std::string& basename,
                       mode_t expected_type,
                       const limitless::ArtifactIdentity& identity,
                       bool require_private_mode) {
  struct stat st = {};
  return fstatat(directory_fd, basename.c_str(), &st, AT_SYMLINK_NOFOLLOW) ==
             0 &&
         StatMatchesIdentity(st, expected_type, identity) &&
         (!require_private_mode || (st.st_mode & 0777) == kPrivateFileMode);
}

bool DescriptorMatchesIdentity(
    int fd,
    mode_t expected_type,
    const limitless::ArtifactIdentity& expected_identity) {
  struct stat st = {};
  return fd >= 0 && fstat(fd, &st) == 0 &&
         StatMatchesIdentity(st, expected_type, expected_identity);
}

bool UnlinkAt(int directory_fd, const std::string& basename) {
  for (;;) {
    if (unlinkat(directory_fd, basename.c_str(), 0) == 0 || errno == ENOENT) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

bool QuarantineOwnedArtifactAt(
    int directory_fd,
    const std::string& basename,
    mode_t expected_type,
    const limitless::ArtifactIdentity& expected_identity,
    int identity_fd) {
  if ((expected_type == S_IFREG && identity_fd < 0) ||
      (identity_fd >= 0 &&
       !DescriptorMatchesIdentity(identity_fd, expected_type,
                                  expected_identity))) {
    return false;
  }

  static std::atomic<uint64_t> quarantine_counter{0};
  std::string quarantine_basename;
  bool quarantined = false;
  for (size_t attempt = 0; attempt < 128; ++attempt) {
    quarantine_basename = ".limitless-quarantine-" + std::to_string(getpid()) +
                          "-" +
                          std::to_string(quarantine_counter.fetch_add(
                              1, std::memory_order_relaxed));
    if (renameatx_np(directory_fd, basename.c_str(), directory_fd,
                     quarantine_basename.c_str(), RENAME_EXCL) == 0) {
      quarantined = true;
      break;
    }
    if (errno != EEXIST) {
      return false;
    }
  }
  if (!quarantined) {
    return false;
  }

  struct stat quarantine_stat = {};
  const bool owns_quarantined_artifact =
      fstatat(directory_fd, quarantine_basename.c_str(), &quarantine_stat,
              AT_SYMLINK_NOFOLLOW) == 0 &&
      StatMatchesIdentity(quarantine_stat, expected_type, expected_identity);
  if (owns_quarantined_artifact) {
    UnlinkAt(directory_fd, quarantine_basename);
    return true;
  }

  // A replacement is not ours to delete. Restore it only if the public name
  // is still vacant. If another writer won that name, retain the quarantine
  // artifact so no foreign inode is destroyed.
  renameatx_np(directory_fd, quarantine_basename.c_str(), directory_fd,
               basename.c_str(), RENAME_EXCL);
  return true;
}

PublishResult WritePrivateFileNoReplaceAt(
    int directory_fd,
    const std::string& basename,
    std::string_view value,
    limitless::ArtifactIdentity* published_identity,
    base::ScopedFD* published_identity_fd) {
  static std::atomic<uint64_t> temporary_counter{0};
  std::string temporary_basename;
  base::ScopedFD fd;
  for (size_t attempt = 0; attempt < 128; ++attempt) {
    temporary_basename = ".limitless-publish-" + std::to_string(getpid()) +
                         "-" +
                         std::to_string(temporary_counter.fetch_add(
                             1, std::memory_order_relaxed));
    fd.reset(openat(directory_fd, temporary_basename.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    kPrivateFileMode));
    if (fd.is_valid()) {
      break;
    }
    if (errno != EEXIST) {
      return PublishResult::kFailure;
    }
  }
  if (!fd.is_valid()) {
    return PublishResult::kFailure;
  }

  struct stat temporary_stat = {};
  if (fchmod(fd.get(), kPrivateFileMode) != 0 ||
      fstat(fd.get(), &temporary_stat) != 0 ||
      !S_ISREG(temporary_stat.st_mode) || temporary_stat.st_uid != geteuid() ||
      (temporary_stat.st_mode & 0777) != kPrivateFileMode) {
    UnlinkAt(directory_fd, temporary_basename);
    return PublishResult::kFailure;
  }

  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t written =
        write(fd.get(), value.data() + offset, value.size() - offset);
    if (written > 0) {
      offset += static_cast<size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  if (offset != value.size() || !FsyncNoIntr(fd.get())) {
    UnlinkAt(directory_fd, temporary_basename);
    return PublishResult::kFailure;
  }

  const limitless::ArtifactIdentity temporary_identity = {
      temporary_stat.st_dev, temporary_stat.st_ino};
  if (renameatx_np(directory_fd, temporary_basename.c_str(), directory_fd,
                   basename.c_str(), RENAME_EXCL) != 0) {
    const int rename_error = errno;
    UnlinkAt(directory_fd, temporary_basename);
    return rename_error == EEXIST ? PublishResult::kCollision
                                  : PublishResult::kFailure;
  }

  limitless::ArtifactIdentity final_identity;
  const bool final_is_valid =
      ReadArtifactIdentityAt(directory_fd, basename, S_IFREG,
                             &final_identity) &&
      final_identity.dev == temporary_identity.dev &&
      final_identity.ino == temporary_identity.ino &&
      ArtifactMatchesAt(directory_fd, basename, S_IFREG, temporary_identity,
                        true);
  if (!final_is_valid || !FsyncNoIntr(directory_fd)) {
    QuarantineOwnedArtifactAt(directory_fd, basename, S_IFREG,
                              temporary_identity, fd.get());
    UnlinkAt(directory_fd, temporary_basename);
    FsyncNoIntr(directory_fd);
    return PublishResult::kFailure;
  }

  *published_identity = final_identity;
  *published_identity_fd = std::move(fd);
  return PublishResult::kSuccess;
}

void ShutdownNoIntr(int fd) {
  while (shutdown(fd, SHUT_RDWR) != 0 && errno == EINTR) {
  }
}

void WakeCancellationPipeNoIntr(int fd) {
  const char byte = 0;
  for (;;) {
    const ssize_t written = write(fd, &byte, 1);
    if (written == 1 ||
        (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
      return;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

}  // namespace

namespace limitless {

PendingRequest::PendingRequest(base::TimeTicks deadline)
    : deadline_(deadline) {}

PendingRequest::~PendingRequest() = default;

bool PendingRequest::TryBeginHandling() {
  base::AutoLock lock(lock_);
  if (state_ != State::kWaiting) {
    return false;
  }
  if (base::TimeTicks::Now() >= deadline_) {
    state_ = State::kCancelled;
    cv_.Broadcast();
    return false;
  }
  state_ = State::kClaimed;
  return true;
}

bool PendingRequest::Complete(std::string response) {
  base::AutoLock lock(lock_);
  if (state_ != State::kClaimed) {
    return false;
  }
  if (base::TimeTicks::Now() >= deadline_) {
    state_ = State::kCancelled;
    cv_.Broadcast();
    return false;
  }
  response_ = std::move(response);
  state_ = State::kCompleted;
  cv_.Broadcast();
  return true;
}

void PendingRequest::Cancel() {
  base::AutoLock lock(lock_);
  if (state_ == State::kCompleted || state_ == State::kCancelled) {
    return;
  }
  state_ = State::kCancelled;
  cv_.Broadcast();
}

bool PendingRequest::Wait(std::string* response) {
  base::AutoLock lock(lock_);
  while (state_ == State::kWaiting || state_ == State::kClaimed) {
    const base::TimeDelta remaining = deadline_ - base::TimeTicks::Now();
    if (!remaining.is_positive()) {
      state_ = State::kCancelled;
      cv_.Broadcast();
      return false;
    }
    cv_.TimedWait(remaining);
  }
  if (state_ != State::kCompleted) {
    return false;
  }
  *response = *response_;
  return true;
}

TransportCancellationState::TransportCancellationState() = default;
TransportCancellationState::~TransportCancellationState() = default;

void TransportCancellationState::Cancel() {
  cancelled.store(true, std::memory_order_release);
  scoped_refptr<PendingRequest> request;
  {
    base::AutoLock guard(lock_);
    if (active_fd_.is_valid()) {
      ShutdownNoIntr(active_fd_.get());
    }
    request = active_request_;
    if (writer_fd_.is_valid()) {
      WakeCancellationPipeNoIntr(writer_fd_.get());
    }
  }
  if (request) {
    request->Cancel();
  }
}

void TransportCancellationState::InstallWriter(int fd) {
  base::AutoLock guard(lock_);
  writer_fd_.reset(fd);
  if (cancelled.load(std::memory_order_acquire)) {
    WakeCancellationPipeNoIntr(writer_fd_.get());
  }
}

void TransportCancellationState::ClearWriter() {
  base::AutoLock guard(lock_);
  writer_fd_.reset();
}

uint64_t TransportCancellationState::InstallAcceptedFd(int fd) {
  if (fd < 0) {
    return 0;
  }
  const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (duplicate < 0) {
    return 0;
  }
  base::ScopedFD owned_fd(duplicate);
  scoped_refptr<PendingRequest> superseded_request;
  base::ScopedFD superseded_fd;
  uint64_t token = 0;
  {
    base::AutoLock guard(lock_);
    if (next_registration_token_ == std::numeric_limits<uint64_t>::max()) {
      return 0;
    }
    superseded_fd = std::move(active_fd_);
    superseded_request = std::move(active_request_);
    token = ++next_registration_token_;
    active_registration_token_ = token;
    active_fd_ = std::move(owned_fd);
    if (cancelled.load(std::memory_order_acquire)) {
      ShutdownNoIntr(active_fd_.get());
    }
  }
  if (superseded_fd.is_valid()) {
    ShutdownNoIntr(superseded_fd.get());
  }
  if (superseded_request) {
    superseded_request->Cancel();
  }
  return token;
}

bool TransportCancellationState::InstallPendingRequest(
    uint64_t token,
    scoped_refptr<PendingRequest> request) {
  bool installed = false;
  {
    base::AutoLock guard(lock_);
    if (!cancelled.load(std::memory_order_acquire) && request && token != 0 &&
        token == active_registration_token_ && active_fd_.is_valid() &&
        !active_request_) {
      active_request_ = request;
      installed = true;
    }
  }
  if (!installed && request) {
    request->Cancel();
  }
  return installed;
}

void TransportCancellationState::ClearActive(uint64_t token) {
  scoped_refptr<PendingRequest> request;
  base::ScopedFD old_fd;
  {
    base::AutoLock guard(lock_);
    if (token == 0 || token != active_registration_token_) {
      return;
    }
    active_registration_token_ = 0;
    old_fd = std::move(active_fd_);
    request = std::move(active_request_);
  }
  if (old_fd.is_valid()) {
    ShutdownNoIntr(old_fd.get());
  }
  if (request) {
    request->Cancel();
  }
}

PreferenceControlTransportWorker::PreferenceControlTransportWorker(
    Inputs inputs,
    scoped_refptr<TransportCancellationState> cancellation)
    : inputs_(std::move(inputs)), cancellation_(std::move(cancellation)) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

PreferenceControlTransportWorker::~PreferenceControlTransportWorker() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Cleanup();
}

PreferenceControlTransportWorker::PrepareResult
PreferenceControlTransportWorker::FailPrepare(WorkerPrepareStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Cleanup();
  PrepareResult result;
  result.status = status;
  result.socket_path = inputs_.socket_path;
  result.descriptor_path = inputs_.descriptor_path;
  result.credential_path = inputs_.credential_path;
  return result;
}

PreferenceControlTransportWorker::PrepareResult
PreferenceControlTransportWorker::Prepare() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrepareResult result;
  result.socket_path = inputs_.socket_path;
  result.descriptor_path = inputs_.descriptor_path;
  result.credential_path = inputs_.credential_path;
  if (cancellation_->cancelled.load(std::memory_order_acquire) ||
      (mkdir(inputs_.runtime_directory.value().c_str(),
             kPrivateDirectoryMode) != 0 &&
       errno != EEXIST)) {
    return FailPrepare(cancellation_->cancelled.load()
                           ? WorkerPrepareStatus::kCancelled
                           : WorkerPrepareStatus::kRuntimeDirectory);
  }
  runtime_directory_fd_.reset(
      open(inputs_.runtime_directory.value().c_str(),
           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat runtime_check = {};
  if (!runtime_directory_fd_.is_valid() ||
      fstat(runtime_directory_fd_.get(), &runtime_check) != 0 ||
      !S_ISDIR(runtime_check.st_mode) || runtime_check.st_uid != geteuid() ||
      fchmod(runtime_directory_fd_.get(), kPrivateDirectoryMode) != 0 ||
      fstat(runtime_directory_fd_.get(), &runtime_check) != 0 ||
      !S_ISDIR(runtime_check.st_mode) || runtime_check.st_uid != geteuid() ||
      (runtime_check.st_mode & 0777) != kPrivateDirectoryMode) {
    return FailPrepare(WorkerPrepareStatus::kRuntimeDirectory);
  }
  runtime_directory_identity_ = {runtime_check.st_dev, runtime_check.st_ino};

  std::string socket_basename;
  std::string descriptor_basename;
  std::string credential_basename;
  if (!GetArtifactBasename(inputs_.runtime_directory, inputs_.socket_path,
                           &socket_basename) ||
      !GetArtifactBasename(inputs_.runtime_directory, inputs_.descriptor_path,
                           &descriptor_basename) ||
      !GetArtifactBasename(inputs_.runtime_directory, inputs_.credential_path,
                           &credential_basename)) {
    return FailPrepare(WorkerPrepareStatus::kRuntimeDirectory);
  }
  listener_.reset(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener_.is_valid()) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  int flags = fcntl(listener_.get(), F_GETFD);
  int status = fcntl(listener_.get(), F_GETFL);
  if (flags < 0 || status < 0 ||
      fcntl(listener_.get(), F_SETFD, flags | FD_CLOEXEC) != 0 ||
      fcntl(listener_.get(), F_SETFL, status | O_NONBLOCK) != 0) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (inputs_.socket_path.value().size() >= sizeof(address.sun_path)) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  memcpy(address.sun_path, inputs_.socket_path.value().c_str(),
         inputs_.socket_path.value().size() + 1);
  const PathCheck socket_check =
      CheckPathAt(runtime_directory_fd_.get(), socket_basename);
  const PathCheck descriptor_check =
      CheckPathAt(runtime_directory_fd_.get(), descriptor_basename);
  const PathCheck credential_check =
      CheckPathAt(runtime_directory_fd_.get(), credential_basename);
  if (socket_check == PathCheck::kExists ||
      descriptor_check == PathCheck::kExists ||
      credential_check == PathCheck::kExists) {
    return FailPrepare(WorkerPrepareStatus::kCollision);
  }
  if (socket_check == PathCheck::kError ||
      descriptor_check == PathCheck::kError ||
      credential_check == PathCheck::kError) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  if (inputs_.before_socket_bind_for_testing) {
    inputs_.before_socket_bind_for_testing.Run();
  }
  base::ScopedFD pre_bind_runtime_directory_fd(
      open(inputs_.runtime_directory.value().c_str(),
           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat pre_bind_runtime_check = {};
  if (!pre_bind_runtime_directory_fd.is_valid() ||
      fstat(pre_bind_runtime_directory_fd.get(), &pre_bind_runtime_check) !=
          0 ||
      !StatMatchesIdentity(pre_bind_runtime_check, S_IFDIR,
                           runtime_directory_identity_) ||
      (pre_bind_runtime_check.st_mode & 0777) != kPrivateDirectoryMode) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
           offsetof(sockaddr_un, sun_path) +
               inputs_.socket_path.value().size() + 1) != 0) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }

  if (ReadArtifactIdentityAt(runtime_directory_fd_.get(), socket_basename,
                             S_IFSOCK, &socket_identity_)) {
    owns_socket_ = true;
    bound_socket_directory_fd_.reset(
        fcntl(runtime_directory_fd_.get(), F_DUPFD_CLOEXEC, 0));
    if (!bound_socket_directory_fd_.is_valid()) {
      return FailPrepare(WorkerPrepareStatus::kSocket);
    }
  } else {
    base::ScopedFD current_runtime_directory_fd(
        open(inputs_.runtime_directory.value().c_str(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!current_runtime_directory_fd.is_valid() ||
        !ReadArtifactIdentityAt(current_runtime_directory_fd.get(),
                                socket_basename, S_IFSOCK, &socket_identity_)) {
      return FailPrepare(WorkerPrepareStatus::kSocket);
    }
    owns_socket_ = true;
    bound_socket_directory_fd_ = std::move(current_runtime_directory_fd);
  }

  base::ScopedFD current_runtime_directory_fd(
      open(inputs_.runtime_directory.value().c_str(),
           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat current_runtime_check = {};
  if (!current_runtime_directory_fd.is_valid() ||
      fstat(current_runtime_directory_fd.get(), &current_runtime_check) != 0 ||
      !StatMatchesIdentity(current_runtime_check, S_IFDIR,
                           runtime_directory_identity_) ||
      (current_runtime_check.st_mode & 0777) != kPrivateDirectoryMode) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  if (inputs_.force_post_bind_failure ||
      fchmodat(bound_socket_directory_fd_.get(), socket_basename.c_str(),
               kPrivateFileMode, 0) != 0 ||
      !ArtifactMatchesAt(bound_socket_directory_fd_.get(), socket_basename,
                         S_IFSOCK, socket_identity_, true) ||
      listen(listener_.get(), 4) != 0) {
    return FailPrepare(WorkerPrepareStatus::kSocket);
  }
  // Establish cancellation before publishing any endpoint artifacts.
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    return FailPrepare(WorkerPrepareStatus::kCancellationChannel);
  }
  cancellation_reader_.reset(pipe_fds[0]);
  flags = fcntl(pipe_fds[1], F_GETFD);
  status = fcntl(pipe_fds[1], F_GETFL);
  int reader_flags = fcntl(cancellation_reader_.get(), F_GETFD);
  int reader_status = fcntl(cancellation_reader_.get(), F_GETFL);
  if (flags < 0 || status < 0 || reader_flags < 0 || reader_status < 0 ||
      fcntl(pipe_fds[1], F_SETFD, flags | FD_CLOEXEC) != 0 ||
      fcntl(pipe_fds[1], F_SETFL, status | O_NONBLOCK) != 0 ||
      fcntl(cancellation_reader_.get(), F_SETFD, reader_flags | FD_CLOEXEC) !=
          0 ||
      fcntl(cancellation_reader_.get(), F_SETFL, reader_status | O_NONBLOCK) !=
          0) {
    close(pipe_fds[1]);
    return FailPrepare(WorkerPrepareStatus::kCancellationChannel);
  }
  cancellation_->InstallWriter(pipe_fds[1]);
  if (inputs_.before_credential_publication_for_testing) {
    inputs_.before_credential_publication_for_testing.Run();
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire)) {
    return FailPrepare(WorkerPrepareStatus::kCancelled);
  }
  const PublishResult credential_publish_result = WritePrivateFileNoReplaceAt(
      runtime_directory_fd_.get(), credential_basename, inputs_.capability,
      &credential_identity_, &credential_identity_fd_);
  if (credential_publish_result != PublishResult::kSuccess) {
    return FailPrepare(credential_publish_result == PublishResult::kCollision
                           ? WorkerPrepareStatus::kCollision
                           : WorkerPrepareStatus::kCredential);
  }
  owns_credential_ = true;
  if (inputs_.before_descriptor_publication_for_testing) {
    inputs_.before_descriptor_publication_for_testing.Run();
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire)) {
    return FailPrepare(WorkerPrepareStatus::kCancelled);
  }
  base::DictValue descriptor_value;
  descriptor_value.Set("version", 1);
  descriptor_value.Set("profile_id", inputs_.profile_id);
  descriptor_value.Set("instance_id", inputs_.instance_id);
  descriptor_value.Set("socket_path", inputs_.socket_path.value());
  descriptor_value.Set("credential_path", inputs_.credential_path.value());
  descriptor_value.Set("pid", base::GetCurrentProcId());
  std::string descriptor;
  if (!base::JSONWriter::Write(descriptor_value, &descriptor)) {
    return FailPrepare(WorkerPrepareStatus::kDescriptor);
  }
  const PublishResult descriptor_publish_result = WritePrivateFileNoReplaceAt(
      runtime_directory_fd_.get(), descriptor_basename, descriptor,
      &descriptor_identity_, &descriptor_identity_fd_);
  if (descriptor_publish_result != PublishResult::kSuccess) {
    return FailPrepare(descriptor_publish_result == PublishResult::kCollision
                           ? WorkerPrepareStatus::kCollision
                           : WorkerPrepareStatus::kDescriptor);
  }
  owns_descriptor_ = true;
  if (cancellation_->cancelled.load(std::memory_order_acquire)) {
    return FailPrepare(WorkerPrepareStatus::kCancelled);
  }
  published_ = true;
  result.status = WorkerPrepareStatus::kSuccess;
  result.succeeded = true;
  return result;
}

void PreferenceControlTransportWorker::DispatchToOrigin(
    OriginRequestHandler handler,
    std::string payload,
    base::TimeTicks deadline,
    scoped_refptr<PendingRequest> pending) {
  if (!pending->TryBeginHandling()) {
    return;
  }
  handler.Run(std::move(payload), deadline, std::move(pending));
}

void PreferenceControlTransportWorker::Run(
    scoped_refptr<base::SequencedTaskRunner> origin_task_runner,
    OriginRequestHandler handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::ScopedClosureRunner cleanup(base::BindOnce(
      &PreferenceControlTransportWorker::Cleanup, base::Unretained(this)));
  if (!published_ || !listener_.is_valid() ||
      !cancellation_reader_.is_valid() || !origin_task_runner ||
      origin_task_runner->RunsTasksInCurrentSequence() || handler.is_null()) {
    return;
  }
  while (!cancellation_->cancelled.load(std::memory_order_acquire)) {
    if (inputs_.entering_accept_wait_for_testing) {
      inputs_.entering_accept_wait_for_testing.Run();
    }
    pollfd fds[] = {{listener_.get(), POLLIN, 0},
                    {cancellation_reader_.get(), POLLIN, 0}};
    int ready = poll(fds, 2, -1);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0 || fds[1].revents != 0) {
      break;
    }
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      break;
    }
    if (!(fds[0].revents & POLLIN)) {
      continue;
    }
    base::ScopedFD connection(accept(listener_.get(), nullptr, nullptr));
    if (!connection.is_valid()) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;
    }
    if (!ConfigureAccepted(connection.get())) {
      continue;
    }
    if (inputs_.accepted_fd_for_testing) {
      inputs_.accepted_fd_for_testing.Run(connection.get());
    }
    uint64_t token = cancellation_->InstallAcceptedFd(connection.get());
    if (!token || cancellation_->cancelled.load(std::memory_order_acquire)) {
      if (token) {
        cancellation_->ClearActive(token);
      }
      continue;
    }
    base::ScopedClosureRunner clear_token(base::BindOnce(
        &TransportCancellationState::ClearActive, cancellation_, token));
    if (!IsCurrentUserPeer(connection.get())) {
      continue;
    }
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + (inputs_.io_timeout_for_testing.is_zero()
                                      ? kTransportIoTimeout
                                      : inputs_.io_timeout_for_testing);
    uint32_t network_size = 0;
    if (!ReadExactly(connection.get(), base::byte_span_from_ref(network_size),
                     deadline)) {
      continue;
    }
    const size_t size = ntohl(network_size);
    if (size == 0 || size > kMaximumPreferenceRequestBytes) {
      const std::string error = SerializePreferenceRequestParseError(
          PreferenceRequestParseStatus::kOversized);
      SendFramedResponse(connection.get(), error, deadline);
      continue;
    }
    std::string payload(size, '\0');
    if (!ReadExactly(connection.get(), base::as_writable_byte_span(payload),
                     deadline)) {
      continue;
    }
    auto pending = base::MakeRefCounted<PendingRequest>(deadline);
    if (!cancellation_->InstallPendingRequest(token, pending) ||
        cancellation_->cancelled.load(std::memory_order_acquire)) {
      pending->Cancel();
      continue;
    }
    if (!origin_task_runner->PostTask(
            FROM_HERE,
            base::BindOnce(&PreferenceControlTransportWorker::DispatchToOrigin,
                           handler, std::move(payload), deadline, pending))) {
      pending->Cancel();
      continue;
    }
    std::string response;
    if (!pending->Wait(&response) ||
        cancellation_->cancelled.load(std::memory_order_acquire) ||
        response.size() > kMaximumPreferenceTransportResponseBytes) {
      continue;
    }
    SendFramedResponse(connection.get(), response, deadline);
  }
}

void PreferenceControlTransportWorker::Cleanup() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  cancellation_->ClearWriter();
  bool removed_runtime_artifact = false;
  if (runtime_directory_fd_.is_valid()) {
    std::string descriptor_basename;
    std::string credential_basename;
    if (owns_descriptor_ &&
        GetArtifactBasename(inputs_.runtime_directory, inputs_.descriptor_path,
                            &descriptor_basename)) {
      removed_runtime_artifact |= QuarantineOwnedArtifactAt(
          runtime_directory_fd_.get(), descriptor_basename, S_IFREG,
          descriptor_identity_, descriptor_identity_fd_.get());
    }
    if (owns_credential_ &&
        GetArtifactBasename(inputs_.runtime_directory, inputs_.credential_path,
                            &credential_basename)) {
      removed_runtime_artifact |= QuarantineOwnedArtifactAt(
          runtime_directory_fd_.get(), credential_basename, S_IFREG,
          credential_identity_, credential_identity_fd_.get());
    }
    if (removed_runtime_artifact) {
      FsyncNoIntr(runtime_directory_fd_.get());
    }
  }
  const int socket_directory_fd = bound_socket_directory_fd_.is_valid()
                                      ? bound_socket_directory_fd_.get()
                                      : runtime_directory_fd_.get();
  if (owns_socket_ && socket_directory_fd >= 0) {
    std::string socket_basename;
    if (GetArtifactBasename(inputs_.runtime_directory, inputs_.socket_path,
                            &socket_basename) &&
        QuarantineOwnedArtifactAt(socket_directory_fd, socket_basename,
                                  S_IFSOCK, socket_identity_, -1)) {
      FsyncNoIntr(socket_directory_fd);
    }
  }

  listener_.reset();
  cancellation_reader_.reset();
  credential_identity_fd_.reset();
  descriptor_identity_fd_.reset();
  bound_socket_directory_fd_.reset();
  runtime_directory_fd_.reset();
  owns_socket_ = owns_descriptor_ = owns_credential_ = false;
  socket_identity_ = {};
  credential_identity_ = {};
  descriptor_identity_ = {};
  runtime_directory_identity_ = {};
  published_ = false;
}

}  // namespace limitless
