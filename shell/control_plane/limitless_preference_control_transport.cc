// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_preference_control_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/process/process_handle.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"

namespace limitless {
namespace {

constexpr base::TimeDelta kTransportIoTimeout = base::Seconds(2);
constexpr int kListenBacklog = 4;

bool ConfigureIoDeadline(int fd, base::TimeTicks deadline) {
  base::TimeDelta remaining = deadline - base::TimeTicks::Now();
  if (!remaining.is_positive()) {
    return false;
  }
  const int64_t microseconds = std::max<int64_t>(1, remaining.InMicroseconds());
  const timeval timeout = {
      .tv_sec = static_cast<time_t>(microseconds / 1000000),
      .tv_usec = static_cast<suseconds_t>(microseconds % 1000000),
  };
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
             0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
             0;
}

bool ReadExactly(int fd, base::span<uint8_t> output, base::TimeTicks deadline) {
  while (!output.empty()) {
    if (!ConfigureIoDeadline(fd, deadline)) {
      return false;
    }
    const ssize_t read_size = read(fd, output.data(), output.size());
    if (read_size == 0) {
      return false;
    }
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    output = output.subspan(static_cast<size_t>(read_size));
  }
  return true;
}

bool WriteExactly(int fd,
                  base::span<const uint8_t> input,
                  base::TimeTicks deadline) {
  while (!input.empty()) {
    if (!ConfigureIoDeadline(fd, deadline)) {
      return false;
    }
    const ssize_t write_size =
        send(fd, input.data(), input.size(), MSG_NOSIGNAL);
    if (write_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    input = input.subspan(static_cast<size_t>(write_size));
  }
  return true;
}

bool WriteFileExactly(int fd, std::string_view contents) {
  while (!contents.empty()) {
    const ssize_t write_size = write(fd, contents.data(), contents.size());
    if (write_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    contents.remove_prefix(static_cast<size_t>(write_size));
  }
  return true;
}

bool WritePrivateFileAtomically(const base::FilePath& path,
                                std::string_view contents) {
  const base::FilePath temporary = path.AddExtensionASCII("tmp");
  unlink(temporary.value().c_str());
  base::ScopedFD temporary_fd(open(
      temporary.value().c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
  if (!temporary_fd.is_valid() ||
      !WriteFileExactly(temporary_fd.get(), contents) ||
      fsync(temporary_fd.get()) != 0) {
    temporary_fd.reset();
    unlink(temporary.value().c_str());
    return false;
  }
  temporary_fd.reset();
  if (rename(temporary.value().c_str(), path.value().c_str()) != 0) {
    unlink(temporary.value().c_str());
    return false;
  }
  return true;
}

bool IsCurrentUserPeer(int fd) {
  uid_t peer_uid = 0;
  gid_t peer_gid = 0;
  return getpeereid(fd, &peer_uid, &peer_gid) == 0 && peer_uid == geteuid();
}

bool ConfigureNonBlockingCloseOnExec(int descriptor) {
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  const int status_flags = fcntl(descriptor, F_GETFL);
  return descriptor_flags >= 0 && status_flags >= 0 &&
         fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0 &&
         fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) == 0;
}

std::string SerializeParseError(PreferenceRequestParseStatus status) {
  base::DictValue result;
  result.Set("version", kPreferenceControlSchemaVersion);
  result.Set("status", "invalid_request");
  result.Set("error", PreferenceRequestParseStatusToString(status));
  std::string serialized;
  CHECK(base::JSONWriter::Write(result, &serialized));
  return serialized;
}

bool SendFramedResponse(int fd,
                        std::string_view response,
                        base::TimeTicks deadline) {
  if (response.size() > kMaximumPreferenceTransportResponseBytes) {
    return false;
  }
  const uint32_t frame_size = htonl(static_cast<uint32_t>(response.size()));
  return WriteExactly(fd, base::byte_span_from_ref(frame_size), deadline) &&
         WriteExactly(fd, base::as_byte_span(response), deadline);
}

}  // namespace

struct PreferenceControlTransport::PendingResponse {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<std::string> response;
  bool cancelled = false;
};

PreferenceControlTransport::PreferenceControlTransport(
    ProfilePreferenceControlBackend* backend,
    base::FilePath user_data_directory)
    : backend_(backend),
      runtime_directory_(
          user_data_directory.AppendASCII(kPreferenceControlRuntimeDirectory)),
      origin_task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {
  CHECK(backend_);
  CHECK(origin_task_runner_);
}

PreferenceControlTransport::~PreferenceControlTransport() {
  Stop();
}

PreferenceControlTransportStartStatus PreferenceControlTransport::Start() {
  if (running_) {
    return PreferenceControlTransportStartStatus::kAlreadyRunning;
  }
  if (backend_->is_off_the_record()) {
    return PreferenceControlTransportStartStatus::kPrivateProfile;
  }
  if (!PrepareRuntimeDirectory()) {
    return PreferenceControlTransportStartStatus::kInvalidRuntimeDirectory;
  }

  const std::string profile(backend_->profile_identifier());
  const std::string instance(backend_->profile_instance_identifier());
  const std::string endpoint_name =
      profile.substr(0, 8) + "-" + instance.substr(0, 8);
  socket_path_ = runtime_directory_.AppendASCII(endpoint_name + ".sock");
  descriptor_path_ =
      runtime_directory_.AppendASCII(profile + "." + instance + ".json");
  credential_path_ =
      runtime_directory_.AppendASCII(profile + "." + instance + ".token");
  sockaddr_un address = {};
  if (socket_path_.value().size() >= sizeof(address.sun_path)) {
    const std::string short_endpoint =
        "limitless-" + std::to_string(geteuid()) + "-" +
        std::to_string(base::GetCurrentProcId()) + "-" + endpoint_name +
        ".sock";
    socket_path_ = base::FilePath("/tmp").AppendASCII(short_endpoint);
    if (socket_path_.value().size() >= sizeof(address.sun_path)) {
      return PreferenceControlTransportStartStatus::kPathTooLong;
    }
  }

  base::ScopedFD listen_fd(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listen_fd.is_valid()) {
    return PreferenceControlTransportStartStatus::kBindFailed;
  }
  if (!ConfigureNonBlockingCloseOnExec(listen_fd.get())) {
    return PreferenceControlTransportStartStatus::kBindFailed;
  }

  address.sun_family = AF_UNIX;
  base::span(address.sun_path)
      .copy_prefix_from(base::span(socket_path_.value()));
  base::span(address.sun_path)[socket_path_.value().size()] = '\0';
  unlink(socket_path_.value().c_str());
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path_.value().size() + 1);
  if (bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&address),
           address_length) != 0 ||
      chmod(socket_path_.value().c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listen_fd.get(), kListenBacklog) != 0) {
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kBindFailed;
  }
  if (!WriteCredential()) {
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kCredentialWriteFailed;
  }
  if (!WriteDescriptor()) {
    unlink(credential_path_.value().c_str());
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kDescriptorWriteFailed;
  }
  int cancellation_descriptors[2] = {-1, -1};
  if (pipe(cancellation_descriptors) != 0) {
    unlink(descriptor_path_.value().c_str());
    unlink(credential_path_.value().c_str());
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kCancellationChannelFailed;
  }
  base::ScopedFD cancellation_reader(cancellation_descriptors[0]);
  cancellation_fd_.reset(cancellation_descriptors[1]);
  if (!ConfigureNonBlockingCloseOnExec(cancellation_reader.get()) ||
      !ConfigureNonBlockingCloseOnExec(cancellation_fd_.get())) {
    cancellation_fd_.reset();
    unlink(descriptor_path_.value().c_str());
    unlink(credential_path_.value().c_str());
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kCancellationChannelFailed;
  }
  if (!io_thread_.Start()) {
    cancellation_fd_.reset();
    unlink(descriptor_path_.value().c_str());
    unlink(credential_path_.value().c_str());
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kThreadStartFailed;
  }

  stopping_.store(false, std::memory_order_release);
  running_ = true;
  io_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&PreferenceControlTransport::AcceptLoop,
                     base::Unretained(this), weak_factory_.GetWeakPtr(),
                     std::move(listen_fd), std::move(cancellation_reader)));
  return PreferenceControlTransportStartStatus::kSuccess;
}

void PreferenceControlTransport::Stop() {
  if (!running_) {
    return;
  }
  weak_factory_.InvalidateWeakPtrs();
  stopping_.store(true, std::memory_order_release);
  if (cancellation_fd_.is_valid()) {
    const char cancellation = 0;
    const ssize_t write_result =
        write(cancellation_fd_.get(), &cancellation, sizeof(cancellation));
    static_cast<void>(write_result);
  }
  {
    std::lock_guard lock(accepted_connection_mutex_);
    if (accepted_connection_descriptor_ >= 0) {
      shutdown(accepted_connection_descriptor_, SHUT_RDWR);
    }
  }
  std::shared_ptr<PendingResponse> pending;
  {
    std::lock_guard lock(pending_response_mutex_);
    pending = pending_response_.lock();
  }
  if (pending) {
    {
      std::lock_guard pending_lock(pending->mutex);
      pending->cancelled = true;
    }
    pending->ready.notify_one();
  }
  io_thread_.Stop();
  cancellation_fd_.reset();
  unlink(descriptor_path_.value().c_str());
  unlink(credential_path_.value().c_str());
  unlink(socket_path_.value().c_str());
  running_ = false;
}

bool PreferenceControlTransport::has_accepted_connection_for_testing() {
  std::lock_guard lock(accepted_connection_mutex_);
  return accepted_connection_descriptor_ >= 0;
}

bool PreferenceControlTransport::PrepareRuntimeDirectory() {
  if (mkdir(runtime_directory_.value().c_str(), S_IRWXU) != 0 &&
      errno != EEXIST) {
    return false;
  }
  struct stat metadata = {};
  if (lstat(runtime_directory_.value().c_str(), &metadata) != 0 ||
      !S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid()) {
    return false;
  }
  return chmod(runtime_directory_.value().c_str(), S_IRWXU) == 0;
}

bool PreferenceControlTransport::WriteDescriptor() {
  base::DictValue descriptor;
  descriptor.Set("version", kPreferenceControlSchemaVersion);
  descriptor.Set("profile_id", backend_->profile_identifier());
  descriptor.Set("instance_id", backend_->profile_instance_identifier());
  descriptor.Set("socket_path", socket_path_.value());
  descriptor.Set("credential_path", credential_path_.value());
  descriptor.Set("pid", base::GetCurrentProcId());
  std::string serialized;
  if (!base::JSONWriter::Write(descriptor, &serialized)) {
    return false;
  }
  return WritePrivateFileAtomically(descriptor_path_, serialized);
}

bool PreferenceControlTransport::WriteCredential() {
  return WritePrivateFileAtomically(credential_path_, backend_->capability());
}

void PreferenceControlTransport::AcceptLoop(
    base::WeakPtr<PreferenceControlTransport> origin_weak_ptr,
    base::ScopedFD listen_fd,
    base::ScopedFD cancellation_fd) {
  while (!stopping_.load(std::memory_order_acquire)) {
    pollfd descriptors[] = {
        {.fd = listen_fd.get(), .events = POLLIN, .revents = 0},
        {.fd = cancellation_fd.get(), .events = POLLIN, .revents = 0},
    };
    const int ready = poll(descriptors, std::size(descriptors), -1);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0 || descriptors[1].revents != 0) {
      break;
    }
    if (!(descriptors[0].revents & POLLIN)) {
      break;
    }
    base::ScopedFD connection(accept(listen_fd.get(), nullptr, nullptr));
    if (!connection.is_valid()) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;
    }
    {
      std::lock_guard lock(accepted_connection_mutex_);
      accepted_connection_descriptor_ = connection.get();
    }
    base::ScopedClosureRunner clear_accepted(
        base::BindOnce(&PreferenceControlTransport::ClearAcceptedConnection,
                       base::Unretained(this), connection.get()));
    if (!IsCurrentUserPeer(connection.get())) {
      continue;
    }
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + kTransportIoTimeout;

    uint32_t network_size = 0;
    if (!ReadExactly(connection.get(), base::byte_span_from_ref(network_size),
                     deadline)) {
      continue;
    }
    const size_t payload_size = ntohl(network_size);
    if (payload_size == 0 || payload_size > kMaximumPreferenceRequestBytes) {
      SendFramedResponse(
          connection.get(),
          SerializeParseError(PreferenceRequestParseStatus::kOversized),
          deadline);
      continue;
    }
    std::string payload(payload_size, '\0');
    if (!ReadExactly(connection.get(), base::as_writable_byte_span(payload),
                     deadline)) {
      continue;
    }

    auto pending = std::make_shared<PendingResponse>();
    {
      std::lock_guard lock(pending_response_mutex_);
      pending_response_ = pending;
    }
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&PreferenceControlTransport::HandleRequestOnOrigin,
                       origin_weak_ptr, std::move(payload), pending));
    std::unique_lock lock(pending->mutex);
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (!remaining.is_positive() ||
        !pending->ready.wait_for(
            lock, std::chrono::microseconds(remaining.InMicroseconds()),
            [&pending] {
              return pending->response.has_value() || pending->cancelled;
            }) ||
        pending->cancelled) {
      lock.unlock();
      std::lock_guard pending_lock(pending_response_mutex_);
      pending_response_.reset();
      continue;
    }
    SendFramedResponse(connection.get(), *pending->response, deadline);
    lock.unlock();
    {
      std::lock_guard pending_lock(pending_response_mutex_);
      pending_response_.reset();
    }
  }
}

void PreferenceControlTransport::ClearAcceptedConnection(int descriptor) {
  std::lock_guard lock(accepted_connection_mutex_);
  if (accepted_connection_descriptor_ == descriptor) {
    accepted_connection_descriptor_ = -1;
  }
}

void PreferenceControlTransport::HandleRequestOnOrigin(
    std::string payload,
    std::shared_ptr<PendingResponse> pending) {
  std::string response;
  PreferenceRequestParseResult parsed = ParsePreferenceRequest(payload);
  if (!parsed.succeeded()) {
    response = SerializeParseError(parsed.status);
  } else {
    PreferenceControlService service(backend_);
    response =
        SerializePreferenceControlResult(service.Execute(*parsed.request));
  }
  {
    std::lock_guard lock(pending->mutex);
    pending->response = std::move(response);
  }
  pending->ready.notify_one();
}

}  // namespace limitless
