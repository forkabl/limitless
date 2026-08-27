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

namespace {
bool WriteAtomic(const base::FilePath& path, std::string_view value) {
  base::FilePath tmp = path.AddExtensionASCII("tmp");
  int fd = open(tmp.value().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  size_t offset = 0;
  while (offset < value.size()) {
    ssize_t n = write(fd, value.data() + offset, value.size() - offset);
    if (n > 0) offset += static_cast<size_t>(n);
    else if (n < 0 && errno == EINTR) continue;
    else break;
  }
  bool ok = offset == value.size() && fsync(fd) == 0;
  close(fd);
  if (!ok || rename(tmp.value().c_str(), path.value().c_str()) != 0) { unlink(tmp.value().c_str()); return false; }
  return true;
}
}

namespace limitless {

TransportCancellationState::TransportCancellationState() = default;
TransportCancellationState::~TransportCancellationState() = default;

void TransportCancellationState::Cancel() {
  cancelled.store(true, std::memory_order_release);
  base::AutoLock guard(lock);
  if (writer_fd.is_valid()) {
    const char byte = 0;
    for (;;) {
      ssize_t written = write(writer_fd.get(), &byte, 1);
      if (written == 1 || (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) break;
      if (written < 0 && errno == EINTR) continue;
      writer_fd.reset(); break;
    }
  }
}

void TransportCancellationState::InstallWriter(int fd) {
  base::AutoLock guard(lock);
  writer_fd.reset(fd);
  if (cancelled.load(std::memory_order_acquire)) {
    const char byte = 0;
    write(writer_fd.get(), &byte, 1);
  }
}

void TransportCancellationState::ClearWriter() {
  base::AutoLock guard(lock);
  writer_fd.reset();
}

PreferenceControlTransportWorker::PreferenceControlTransportWorker(
    Inputs inputs, scoped_refptr<TransportCancellationState> cancellation)
    : inputs_(std::move(inputs)), cancellation_(std::move(cancellation)) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

PreferenceControlTransportWorker::~PreferenceControlTransportWorker() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Cleanup();
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
      errno != EEXIST)) { result.status = cancellation_->cancelled.load() ? WorkerPrepareStatus::kCancelled : WorkerPrepareStatus::kRuntimeDirectory; return result; }
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
  struct stat socket_stat = {};
  if (lstat(inputs_.socket_path.value().c_str(), &socket_stat) == 0) {
    result.status = WorkerPrepareStatus::kCollision;
    return result;
  }
  if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
           offsetof(sockaddr_un, sun_path) + inputs_.socket_path.value().size() + 1) != 0 ||
      chmod(inputs_.socket_path.value().c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listener_.get(), 4) != 0) { Cleanup(); return result; }
  owns_socket_ = true;
  // Establish cancellation before publishing any endpoint artifacts.
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) { result.status = WorkerPrepareStatus::kCancellationChannel; Cleanup(); return result; }
  cancellation_reader_.reset(pipe_fds[0]);
  flags = fcntl(pipe_fds[1], F_GETFD); status = fcntl(pipe_fds[1], F_GETFL);
  int reader_flags = fcntl(cancellation_reader_.get(), F_GETFD);
  int reader_status = fcntl(cancellation_reader_.get(), F_GETFL);
  if (flags < 0 || status < 0 || reader_flags < 0 || reader_status < 0 ||
      fcntl(pipe_fds[1], F_SETFD, flags | FD_CLOEXEC) != 0 ||
      fcntl(pipe_fds[1], F_SETFL, status | O_NONBLOCK) != 0 ||
      fcntl(cancellation_reader_.get(), F_SETFD, reader_flags | FD_CLOEXEC) != 0 ||
      fcntl(cancellation_reader_.get(), F_SETFL, reader_status | O_NONBLOCK) != 0) {
    close(pipe_fds[1]); result.status = WorkerPrepareStatus::kCancellationChannel; Cleanup(); return result;
  }
  cancellation_->InstallWriter(pipe_fds[1]);
  if (cancellation_->cancelled.load(std::memory_order_acquire) ||
      !WriteAtomic(inputs_.credential_path, inputs_.capability)) { result.status = WorkerPrepareStatus::kCredential; Cleanup(); return result; }
  owns_credential_ = true;
  base::DictValue descriptor_value;
  descriptor_value.Set("version", 1);
  descriptor_value.Set("profile_id", inputs_.profile_id);
  descriptor_value.Set("instance_id", inputs_.instance_id);
  descriptor_value.Set("socket_path", inputs_.socket_path.value());
  descriptor_value.Set("credential_path", inputs_.credential_path.value());
  descriptor_value.Set("pid", base::GetCurrentProcId());
  std::string descriptor;
  if (!base::JSONWriter::Write(descriptor_value, &descriptor)) {
    result.status = WorkerPrepareStatus::kDescriptor; Cleanup(); return result;
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire) ||
      !WriteAtomic(inputs_.descriptor_path, descriptor)) { result.status = WorkerPrepareStatus::kDescriptor; Cleanup(); return result; }
  owns_descriptor_ = true;
  if (cancellation_->cancelled.load(std::memory_order_acquire)) { result.status = WorkerPrepareStatus::kCancelled; Cleanup(); return result; }
  published_ = true;
  result.status = WorkerPrepareStatus::kSuccess;
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
