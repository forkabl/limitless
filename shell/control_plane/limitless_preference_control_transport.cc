// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_preference_control_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/process/process_handle.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"

namespace limitless {
namespace {

constexpr base::TimeDelta kTransportIoTimeout = base::Seconds(2);
constexpr int kListenBacklog = 4;

bool ReadExactly(int fd, void* destination, size_t length) {
  auto* output = static_cast<unsigned char*>(destination);
  while (length > 0) {
    const ssize_t read_size = read(fd, output, length);
    if (read_size == 0) {
      return false;
    }
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    output += read_size;
    length -= static_cast<size_t>(read_size);
  }
  return true;
}

bool WriteExactly(int fd, const void* source, size_t length) {
  const auto* input = static_cast<const unsigned char*>(source);
  while (length > 0) {
    const ssize_t write_size = send(fd, input, length, MSG_NOSIGNAL);
    if (write_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    input += write_size;
    length -= static_cast<size_t>(write_size);
  }
  return true;
}

void ConfigureIoDeadline(int fd) {
  const timeval timeout = {
      .tv_sec = static_cast<time_t>(kTransportIoTimeout.InSeconds()),
      .tv_usec = 0,
  };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool IsCurrentUserPeer(int fd) {
  uid_t peer_uid = 0;
  gid_t peer_gid = 0;
  return getpeereid(fd, &peer_uid, &peer_gid) == 0 && peer_uid == geteuid();
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

bool SendFramedResponse(int fd, std::string_view response) {
  if (response.size() > kMaximumPreferenceTransportResponseBytes) {
    return false;
  }
  const uint32_t frame_size = htonl(static_cast<uint32_t>(response.size()));
  return WriteExactly(fd, &frame_size, sizeof(frame_size)) &&
         WriteExactly(fd, response.data(), response.size());
}

}  // namespace

struct PreferenceControlTransport::PendingResponse {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<std::string> response;
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
  const std::string endpoint_name = profile.substr(0, 8) + "-" +
                                    instance.substr(0, 8);
  socket_path_ = runtime_directory_.AppendASCII(endpoint_name + ".sock");
  descriptor_path_ =
      runtime_directory_.AppendASCII(profile + "." + instance + ".json");
  if (socket_path_.value().size() >= sizeof(sockaddr_un::sun_path)) {
    return PreferenceControlTransportStartStatus::kPathTooLong;
  }

  listen_fd_.reset(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listen_fd_.is_valid()) {
    return PreferenceControlTransportStartStatus::kBindFailed;
  }
  fcntl(listen_fd_.get(), F_SETFD,
        fcntl(listen_fd_.get(), F_GETFD) | FD_CLOEXEC);

  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path_.value().c_str(),
              socket_path_.value().size() + 1);
  unlink(socket_path_.value().c_str());
  const socklen_t address_length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                             socket_path_.value().size() + 1);
  if (bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&address),
           address_length) != 0 ||
      chmod(socket_path_.value().c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listen_fd_.get(), kListenBacklog) != 0) {
    listen_fd_.reset();
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kBindFailed;
  }
  if (!WriteDescriptor()) {
    listen_fd_.reset();
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kDescriptorWriteFailed;
  }
  if (!io_thread_.Start()) {
    listen_fd_.reset();
    base::DeleteFile(descriptor_path_);
    unlink(socket_path_.value().c_str());
    return PreferenceControlTransportStartStatus::kThreadStartFailed;
  }

  stopping_.store(false, std::memory_order_release);
  running_ = true;
  io_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&PreferenceControlTransport::AcceptLoop,
                     base::Unretained(this)));
  return PreferenceControlTransportStartStatus::kSuccess;
}

void PreferenceControlTransport::Stop() {
  if (!running_) {
    return;
  }
  weak_factory_.InvalidateWeakPtrs();
  stopping_.store(true, std::memory_order_release);
  if (listen_fd_.is_valid()) {
    shutdown(listen_fd_.get(), SHUT_RDWR);
    listen_fd_.reset();
  }
  io_thread_.Stop();
  base::DeleteFile(descriptor_path_);
  base::DeleteFile(socket_path_);
  running_ = false;
}

bool PreferenceControlTransport::PrepareRuntimeDirectory() {
  if (!base::CreateDirectory(runtime_directory_)) {
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
  descriptor.Set("capability", backend_->capability());
  descriptor.Set("pid", base::GetCurrentProcId());
  std::string serialized;
  if (!base::JSONWriter::Write(descriptor, &serialized)) {
    return false;
  }
  const base::FilePath temporary = descriptor_path_.AddExtensionASCII("tmp");
  if (!base::WriteFile(temporary, serialized) ||
      chmod(temporary.value().c_str(), S_IRUSR | S_IWUSR) != 0 ||
      !base::ReplaceFile(temporary, descriptor_path_, nullptr)) {
    base::DeleteFile(temporary);
    return false;
  }
  return true;
}

void PreferenceControlTransport::AcceptLoop() {
  while (!stopping_.load(std::memory_order_acquire)) {
    base::ScopedFD connection(accept(listen_fd_.get(), nullptr, nullptr));
    if (!connection.is_valid()) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    ConfigureIoDeadline(connection.get());
    if (!IsCurrentUserPeer(connection.get())) {
      continue;
    }

    uint32_t network_size = 0;
    if (!ReadExactly(connection.get(), &network_size, sizeof(network_size))) {
      continue;
    }
    const size_t payload_size = ntohl(network_size);
    if (payload_size == 0 || payload_size > kMaximumPreferenceRequestBytes) {
      SendFramedResponse(
          connection.get(),
          SerializeParseError(PreferenceRequestParseStatus::kOversized));
      continue;
    }
    std::string payload(payload_size, '\0');
    if (!ReadExactly(connection.get(), payload.data(), payload.size())) {
      continue;
    }

    auto pending = std::make_shared<PendingResponse>();
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&PreferenceControlTransport::HandleRequestOnOrigin,
                       weak_factory_.GetWeakPtr(), std::move(payload), pending));
    std::unique_lock lock(pending->mutex);
    if (!pending->ready.wait_for(lock, kTransportIoTimeout.ToChrono(),
                                 [&pending] { return pending->response; })) {
      continue;
    }
    SendFramedResponse(connection.get(), *pending->response);
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
    response = SerializePreferenceControlResult(service.Execute(*parsed.request));
  }
  {
    std::lock_guard lock(pending->mutex);
    pending->response = std::move(response);
  }
  pending->ready.notify_one();
}

}  // namespace limitless
