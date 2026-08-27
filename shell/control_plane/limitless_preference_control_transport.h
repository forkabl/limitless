// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_H_

#include <atomic>
#include <memory>

#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "limitless_profile_preference_backend.h"

namespace limitless {

inline constexpr size_t kMaximumPreferenceTransportResponseBytes = 32 * 1024;
inline constexpr char kPreferenceControlRuntimeDirectory[] =
    "LimitlessControl";

enum class PreferenceControlTransportStartStatus {
  kSuccess,
  kAlreadyRunning,
  kPrivateProfile,
  kInvalidRuntimeDirectory,
  kPathTooLong,
  kBindFailed,
  kDescriptorWriteFailed,
  kThreadStartFailed,
};

// A local, one-request-per-connection transport. The discovery descriptor and
// Unix socket are available only to the current OS user. Requests still need
// the per-process capability carried by the descriptor, and peer credentials
// are checked before any payload is parsed.
class PreferenceControlTransport {
 public:
  PreferenceControlTransport(ProfilePreferenceControlBackend* backend,
                             base::FilePath user_data_directory);
  PreferenceControlTransport(const PreferenceControlTransport&) = delete;
  PreferenceControlTransport& operator=(const PreferenceControlTransport&) =
      delete;
  ~PreferenceControlTransport();

  PreferenceControlTransportStartStatus Start();
  void Stop();

  bool running() const { return running_; }
  const base::FilePath& runtime_directory() const { return runtime_directory_; }
  const base::FilePath& descriptor_path() const { return descriptor_path_; }
  const base::FilePath& socket_path() const { return socket_path_; }

 private:
  struct PendingResponse;

  bool PrepareRuntimeDirectory();
  bool WriteDescriptor();
  void AcceptLoop();
  void HandleRequestOnOrigin(std::string payload,
                             std::shared_ptr<PendingResponse> pending);

  const raw_ptr<ProfilePreferenceControlBackend> backend_;
  const base::FilePath runtime_directory_;
  const scoped_refptr<base::SingleThreadTaskRunner> origin_task_runner_;
  base::FilePath descriptor_path_;
  base::FilePath socket_path_;
  base::ScopedFD listen_fd_;
  base::Thread io_thread_{"LimitlessPreferenceControl"};
  std::atomic<bool> stopping_{false};
  bool running_ = false;
  base::WeakPtrFactory<PreferenceControlTransport> weak_factory_{this};
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_H_
