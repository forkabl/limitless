#include "limitless_preference_control_transport_worker.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "base/json/json_writer.h"
#include "base/process/process_handle.h"

namespace {

constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kPrivateDirectoryMode = S_IRWXU;

enum class PathCheck { kAbsent, kExists, kError };

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

bool WritePrivateFileNoReplaceAt(
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
      return false;
    }
  }
  if (!fd.is_valid()) {
    return false;
  }

  struct stat temporary_stat = {};
  if (fchmod(fd.get(), kPrivateFileMode) != 0 ||
      fstat(fd.get(), &temporary_stat) != 0 ||
      !S_ISREG(temporary_stat.st_mode) || temporary_stat.st_uid != geteuid() ||
      (temporary_stat.st_mode & 0777) != kPrivateFileMode) {
    UnlinkAt(directory_fd, temporary_basename);
    return false;
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
    return false;
  }

  const limitless::ArtifactIdentity temporary_identity = {
      temporary_stat.st_dev, temporary_stat.st_ino};
  if (renameatx_np(directory_fd, temporary_basename.c_str(), directory_fd,
                   basename.c_str(), RENAME_EXCL) != 0) {
    UnlinkAt(directory_fd, temporary_basename);
    return false;
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
    return false;
  }

  *published_identity = final_identity;
  *published_identity_fd = std::move(fd);
  return true;
}

}  // namespace

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
      if (written == 1 ||
          (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        break;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      writer_fd.reset();
      break;
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
    result.status = cancellation_->cancelled.load()
                        ? WorkerPrepareStatus::kCancelled
                        : WorkerPrepareStatus::kRuntimeDirectory;
    return result;
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
    result.status = WorkerPrepareStatus::kRuntimeDirectory;
    return result;
  }

  std::string socket_basename;
  std::string descriptor_basename;
  std::string credential_basename;
  if (!GetArtifactBasename(inputs_.runtime_directory, inputs_.socket_path,
                           &socket_basename) ||
      !GetArtifactBasename(inputs_.runtime_directory, inputs_.descriptor_path,
                           &descriptor_basename) ||
      !GetArtifactBasename(inputs_.runtime_directory, inputs_.credential_path,
                           &credential_basename)) {
    result.status = WorkerPrepareStatus::kRuntimeDirectory;
    return result;
  }
  listener_.reset(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener_.is_valid()) {
    result.status = WorkerPrepareStatus::kSocket;
    return result;
  }
  int flags = fcntl(listener_.get(), F_GETFD);
  int status = fcntl(listener_.get(), F_GETFL);
  if (flags < 0 || status < 0 ||
      fcntl(listener_.get(), F_SETFD, flags | FD_CLOEXEC) != 0 ||
      fcntl(listener_.get(), F_SETFL, status | O_NONBLOCK) != 0) {
    result.status = WorkerPrepareStatus::kSocket;
    return result;
  }
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (inputs_.socket_path.value().size() >= sizeof(address.sun_path)) {
    result.status = WorkerPrepareStatus::kSocket;
    return result;
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
    result.status = WorkerPrepareStatus::kCollision;
    return result;
  }
  if (socket_check == PathCheck::kError ||
      descriptor_check == PathCheck::kError ||
      credential_check == PathCheck::kError) {
    result.status = WorkerPrepareStatus::kSocket;
    return result;
  }
  if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
           offsetof(sockaddr_un, sun_path) +
               inputs_.socket_path.value().size() + 1) != 0) {
    result.status = WorkerPrepareStatus::kSocket;
    Cleanup();
    return result;
  }
  if (!ReadArtifactIdentityAt(runtime_directory_fd_.get(), socket_basename,
                              S_IFSOCK, &socket_identity_)) {
    result.status = WorkerPrepareStatus::kSocket;
    return result;
  }
  owns_socket_ = true;
  if (inputs_.force_post_bind_failure ||
      fchmodat(runtime_directory_fd_.get(), socket_basename.c_str(),
               kPrivateFileMode, 0) != 0 ||
      !ArtifactMatchesAt(runtime_directory_fd_.get(), socket_basename, S_IFSOCK,
                         socket_identity_, true) ||
      listen(listener_.get(), 4) != 0) {
    result.status = WorkerPrepareStatus::kSocket;
    Cleanup();
    return result;
  }
  // Establish cancellation before publishing any endpoint artifacts.
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    result.status = WorkerPrepareStatus::kCancellationChannel;
    Cleanup();
    return result;
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
    result.status = WorkerPrepareStatus::kCancellationChannel;
    Cleanup();
    return result;
  }
  cancellation_->InstallWriter(pipe_fds[1]);
  if (inputs_.before_credential_publication) {
    inputs_.before_credential_publication();
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire)) {
    result.status = WorkerPrepareStatus::kCancelled;
    Cleanup();
    return result;
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire) ||
      !WritePrivateFileNoReplaceAt(
          runtime_directory_fd_.get(), credential_basename, inputs_.capability,
          &credential_identity_, &credential_identity_fd_)) {
    result.status = WorkerPrepareStatus::kCredential;
    Cleanup();
    return result;
  }
  owns_credential_ = true;
  if (inputs_.before_descriptor_publication) {
    inputs_.before_descriptor_publication();
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire)) {
    result.status = WorkerPrepareStatus::kCancelled;
    Cleanup();
    return result;
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
    result.status = WorkerPrepareStatus::kDescriptor;
    Cleanup();
    return result;
  }
  if (cancellation_->cancelled.load(std::memory_order_acquire) ||
      !WritePrivateFileNoReplaceAt(
          runtime_directory_fd_.get(), descriptor_basename, descriptor,
          &descriptor_identity_, &descriptor_identity_fd_)) {
    result.status = WorkerPrepareStatus::kDescriptor;
    Cleanup();
    return result;
  }
  owns_descriptor_ = true;
  if (cancellation_->cancelled.load(std::memory_order_acquire)) {
    result.status = WorkerPrepareStatus::kCancelled;
    Cleanup();
    return result;
  }
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
  cancellation_->ClearWriter();
  bool removed_artifact = false;
  if (runtime_directory_fd_.is_valid()) {
    std::string socket_basename;
    std::string descriptor_basename;
    std::string credential_basename;
    if (owns_descriptor_ &&
        GetArtifactBasename(inputs_.runtime_directory, inputs_.descriptor_path,
                            &descriptor_basename)) {
      removed_artifact |= QuarantineOwnedArtifactAt(
          runtime_directory_fd_.get(), descriptor_basename, S_IFREG,
          descriptor_identity_, descriptor_identity_fd_.get());
    }
    if (owns_credential_ &&
        GetArtifactBasename(inputs_.runtime_directory, inputs_.credential_path,
                            &credential_basename)) {
      removed_artifact |= QuarantineOwnedArtifactAt(
          runtime_directory_fd_.get(), credential_basename, S_IFREG,
          credential_identity_, credential_identity_fd_.get());
    }
    if (owns_socket_ &&
        GetArtifactBasename(inputs_.runtime_directory, inputs_.socket_path,
                            &socket_basename)) {
      removed_artifact |= QuarantineOwnedArtifactAt(runtime_directory_fd_.get(),
                                                    socket_basename, S_IFSOCK,
                                                    socket_identity_, -1);
    }
    if (removed_artifact) {
      FsyncNoIntr(runtime_directory_fd_.get());
    }
  }

  listener_.reset();
  cancellation_reader_.reset();
  credential_identity_fd_.reset();
  descriptor_identity_fd_.reset();
  runtime_directory_fd_.reset();
  owns_socket_ = owns_descriptor_ = owns_credential_ = false;
  socket_identity_ = {};
  credential_identity_ = {};
  descriptor_identity_ = {};
  published_ = false;
}

}  // namespace limitless
