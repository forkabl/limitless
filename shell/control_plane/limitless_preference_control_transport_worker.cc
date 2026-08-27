#include "limitless_preference_control_transport_worker.h"

#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/json/json_writer.h"
#include "base/process/process_handle.h"

namespace limitless {

TransportCancellationState::TransportCancellationState() = default;
TransportCancellationState::~TransportCancellationState() = default;

void TransportCancellationState::Cancel() {
  cancelled.store(true, std::memory_order_release);
  base::AutoLock guard(lock);
  if (writer_fd >= 0) {
    const char byte = 0;
    if (write(writer_fd, &byte, 1) < 0 && errno != EINTR && errno != EAGAIN) {
      close(writer_fd);
      writer_fd = -1;
    }
  }
}

void TransportCancellationState::InstallWriter(int fd) {
  base::AutoLock guard(lock);
  if (writer_fd >= 0) close(writer_fd);
  writer_fd = fd;
  if (cancelled.load(std::memory_order_acquire)) {
    const char byte = 0;
    write(writer_fd, &byte, 1);
  }
}

void TransportCancellationState::ClearWriter() {
  base::AutoLock guard(lock);
  if (writer_fd >= 0) close(writer_fd);
  writer_fd = -1;
}

PreferenceControlTransportWorker::PreferenceControlTransportWorker(
    Inputs inputs, scoped_refptr<TransportCancellationState> cancellation)
    : inputs_(std::move(inputs)), cancellation_(std::move(cancellation)) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

PreferenceControlTransportWorker::~PreferenceControlTransportWorker() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

PreferenceControlTransportWorker::PrepareResult
PreferenceControlTransportWorker::Prepare() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrepareResult result;
  result.socket_path = inputs_.socket_path;
  result.descriptor_path = inputs_.descriptor_path;
  result.credential_path = inputs_.credential_path;
  if (cancellation_->cancelled.load(std::memory_order_acquire) ||
      (mkdir(inputs_.runtime_directory.value().c_str(), S_IRWXU) != 0 &&
      errno != EEXIST)) { result.status = WorkerPrepareStatus::kRuntimeDirectory; return result; }
  struct stat st = {};
  if (lstat(inputs_.runtime_directory.value().c_str(), &st) != 0 ||
      !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      chmod(inputs_.runtime_directory.value().c_str(), S_IRWXU) != 0) { result.status = WorkerPrepareStatus::kRuntimeDirectory; return result; }
  listener_.reset(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener_.is_valid()) { result.status = WorkerPrepareStatus::kSocket; return result; }
  int flags = fcntl(listener_.get(), F_GETFD);
  int status = fcntl(listener_.get(), F_GETFL);
  if (flags < 0 || status < 0 || fcntl(listener_.get(), F_SETFD, flags | FD_CLOEXEC) != 0 ||
      fcntl(listener_.get(), F_SETFL, status | O_NONBLOCK) != 0) return result;
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (inputs_.socket_path.value().size() >= sizeof(address.sun_path)) return result;
  memcpy(address.sun_path, inputs_.socket_path.value().c_str(), inputs_.socket_path.value().size() + 1);
  unlink(inputs_.socket_path.value().c_str());
  if (access(inputs_.socket_path.value().c_str(), F_OK) == 0) { result.status = WorkerPrepareStatus::kCollision; return result; }
  if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
           offsetof(sockaddr_un, sun_path) + inputs_.socket_path.value().size() + 1) != 0 ||
      chmod(inputs_.socket_path.value().c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listener_.get(), 4) != 0) { Cleanup(); return result; }
  owns_socket_ = true;
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) { Cleanup(); return result; }
  cancellation_reader_.reset(pipe_fds[0]);
  flags = fcntl(pipe_fds[1], F_GETFD);
  if (flags < 0 || fcntl(pipe_fds[1], F_SETFD, flags | FD_CLOEXEC) != 0) {
    close(pipe_fds[1]); Cleanup(); return result;
  }
  cancellation_->InstallWriter(pipe_fds[1]);
  if (cancellation_->cancelled.load(std::memory_order_acquire)) { Cleanup(); return result; }
  published_ = true;
  result.succeeded = true;
  return result;
}

void PreferenceControlTransportWorker::Run() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void PreferenceControlTransportWorker::Cleanup() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  listener_.reset();
  cancellation_reader_.reset();
  cancellation_->ClearWriter();
  if (owns_socket_) unlink(inputs_.socket_path.value().c_str());
  if (owns_descriptor_) unlink(inputs_.descriptor_path.value().c_str());
  if (owns_credential_) unlink(inputs_.credential_path.value().c_str());
  owns_socket_ = owns_descriptor_ = owns_credential_ = false;
  published_ = false;
}

}  // namespace limitless
