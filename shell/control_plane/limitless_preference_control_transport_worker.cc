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

void TransportCancellationState::Cancel() { cancelled.store(true, std::memory_order_release); }

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
       errno != EEXIST)) return result;
  struct stat st = {};
  if (lstat(inputs_.runtime_directory.value().c_str(), &st) != 0 ||
      !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      chmod(inputs_.runtime_directory.value().c_str(), S_IRWXU) != 0) return result;
  listener_.reset(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener_.is_valid()) return result;
  int flags = fcntl(listener_.get(), F_GETFD);
  int status = fcntl(listener_.get(), F_GETFL);
  if (flags < 0 || status < 0 || fcntl(listener_.get(), F_SETFD, flags | FD_CLOEXEC) != 0 ||
      fcntl(listener_.get(), F_SETFL, status | O_NONBLOCK) != 0) return result;
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (inputs_.socket_path.value().size() >= sizeof(address.sun_path)) return result;
  memcpy(address.sun_path, inputs_.socket_path.value().c_str(), inputs_.socket_path.value().size() + 1);
  unlink(inputs_.socket_path.value().c_str());
  if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
           offsetof(sockaddr_un, sun_path) + inputs_.socket_path.value().size() + 1) != 0 ||
      chmod(inputs_.socket_path.value().c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listener_.get(), 4) != 0) { Cleanup(); return result; }
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) { Cleanup(); return result; }
  cancellation_reader_.reset(pipe_fds[0]);
  flags = fcntl(pipe_fds[1], F_GETFD);
  if (flags < 0 || fcntl(pipe_fds[1], F_SETFD, flags | FD_CLOEXEC) != 0) {
    close(pipe_fds[1]); Cleanup(); return result;
  }
  close(pipe_fds[1]);
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
  unlink(inputs_.socket_path.value().c_str());
  unlink(inputs_.descriptor_path.value().c_str());
  unlink(inputs_.credential_path.value().c_str());
  published_ = false;
}

}  // namespace limitless
