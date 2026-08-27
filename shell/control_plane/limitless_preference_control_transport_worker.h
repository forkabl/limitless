#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_WORKER_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_WORKER_H_

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/sequence_checker.h"

namespace limitless {

struct TransportCancellationState
    : public base::RefCountedThreadSafe<TransportCancellationState> {
  TransportCancellationState();
  void Cancel();
  void InstallWriter(int fd);
  void ClearWriter();
  std::atomic<bool> cancelled{false};
  base::Lock lock;
  base::ScopedFD writer_fd;
 private:
  friend class base::RefCountedThreadSafe<TransportCancellationState>;
  ~TransportCancellationState();
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
  };
  enum class WorkerPrepareStatus { kSuccess, kCancelled, kRuntimeDirectory,
                                   kCollision, kSocket, kCredential, kDescriptor,
                                   kCancellationChannel };
  struct PrepareResult {
    bool succeeded = false;
    WorkerPrepareStatus status = WorkerPrepareStatus::kRuntimeDirectory;
    base::FilePath socket_path;
    base::FilePath descriptor_path;
    base::FilePath credential_path;
  };

  PreferenceControlTransportWorker(Inputs inputs,
                                   scoped_refptr<TransportCancellationState> cancellation);
  ~PreferenceControlTransportWorker();
  PrepareResult Prepare();
  void Run();
  void Cleanup();

 private:
  SEQUENCE_CHECKER(sequence_checker_);
  Inputs inputs_;
  scoped_refptr<TransportCancellationState> cancellation_;
  base::ScopedFD listener_;
  base::ScopedFD cancellation_reader_;
  bool published_ = false;
  bool owns_socket_ = false;
  bool owns_credential_ = false;
  bool owns_descriptor_ = false;
};

}  // namespace limitless

#endif
