#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_WORKER_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_WORKER_H_

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/sequence_checker.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace limitless {

struct ArtifactIdentity {
  dev_t dev = 0;
  ino_t ino = 0;
};

class PendingRequest : public base::RefCountedThreadSafe<PendingRequest> {
 public:
  explicit PendingRequest(base::TimeTicks deadline);
  bool TryBeginHandling();
  bool Complete(std::string response);
  void Cancel();
  bool Wait(std::string* response);

 private:
  enum class State {
    kWaiting,
    kClaimed,
    kCompleted,
    kCancelled,
  };

  friend class base::RefCountedThreadSafe<PendingRequest>;
  ~PendingRequest();

  base::Lock lock_;
  base::ConditionVariable cv_{&lock_};
  const base::TimeTicks deadline_;
  State state_ = State::kWaiting;
  std::optional<std::string> response_;
};

struct TransportCancellationState
    : public base::RefCountedThreadSafe<TransportCancellationState> {
  TransportCancellationState();
  void Cancel();
  void InstallWriter(int fd);
  void ClearWriter();
  uint64_t InstallAcceptedFd(int fd);
  bool InstallPendingRequest(uint64_t token,
                             scoped_refptr<PendingRequest> request);
  void ClearActive(uint64_t token);
  std::atomic<bool> cancelled{false};

 private:
  friend class base::RefCountedThreadSafe<TransportCancellationState>;
  ~TransportCancellationState();

  base::Lock lock_;
  base::ScopedFD writer_fd_;
  uint64_t next_registration_token_ = 0;
  uint64_t active_registration_token_ = 0;
  base::ScopedFD active_fd_;
  scoped_refptr<PendingRequest> active_request_;
};

class PreferenceControlTransportWorker {
 public:
  struct Inputs {
    base::FilePath runtime_directory;
    base::FilePath descriptor_path;
    base::FilePath credential_path;
    base::FilePath socket_path;
    std::string profile_id;
    std::string instance_id;
    std::string capability;
    bool force_post_bind_failure = false;
    base::RepeatingClosure before_socket_bind_for_testing;
    base::RepeatingClosure before_credential_publication_for_testing;
    base::RepeatingClosure before_descriptor_publication_for_testing;
  };
  enum class WorkerPrepareStatus {
    kSuccess,
    kCancelled,
    kRuntimeDirectory,
    kCollision,
    kSocket,
    kCredential,
    kDescriptor,
    kCancellationChannel
  };
  struct PrepareResult {
    bool succeeded = false;
    WorkerPrepareStatus status = WorkerPrepareStatus::kRuntimeDirectory;
    base::FilePath socket_path;
    base::FilePath descriptor_path;
    base::FilePath credential_path;
  };

  PreferenceControlTransportWorker(
      Inputs inputs,
      scoped_refptr<TransportCancellationState> cancellation);
  ~PreferenceControlTransportWorker();
  PrepareResult Prepare();
  void Run();
  void Cleanup();

 private:
  PrepareResult FailPrepare(WorkerPrepareStatus status);

  SEQUENCE_CHECKER(sequence_checker_);
  Inputs inputs_;
  scoped_refptr<TransportCancellationState> cancellation_;
  base::ScopedFD listener_;
  base::ScopedFD runtime_directory_fd_;
  base::ScopedFD bound_socket_directory_fd_;
  base::ScopedFD cancellation_reader_;
  base::ScopedFD credential_identity_fd_;
  base::ScopedFD descriptor_identity_fd_;
  bool published_ = false;
  bool owns_socket_ = false;
  bool owns_credential_ = false;
  bool owns_descriptor_ = false;
  ArtifactIdentity runtime_directory_identity_;
  ArtifactIdentity socket_identity_, credential_identity_, descriptor_identity_;
};

}  // namespace limitless

#endif
