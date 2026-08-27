// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_H_
#define LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread.h"
#include "limitless_preference_control_transport_worker.h"
#include "limitless_profile_preference_backend.h"

namespace limitless {

// Runtime transport lifecycle for a single regular browser profile.
inline constexpr char kPreferenceControlRuntimeDirectory[] = "LimitlessControl";
enum class PreferenceControlTransportStartStatus {
  kSuccess,
  kAlreadyRunning,
  kPrivateProfile,
  kInvalidRuntimeDirectory,
  kPathTooLong,
  kBindFailed,
  kCredentialWriteFailed,
  kDescriptorWriteFailed,
  kCancellationChannelFailed,
  kThreadStartFailed,
  kCancelled
};
class PreferenceControlTransport {
 public:
  enum class State { kStopped, kStarting, kReady, kStopping };
  using StartCallback =
      base::OnceCallback<void(PreferenceControlTransportStartStatus)>;
  struct TestingHooks {
    base::RepeatingCallback<void(int)> accepted_fd;
    base::RepeatingClosure entering_accept_wait;
    base::RepeatingCallback<bool()> should_exit_run;
    base::RepeatingCallback<bool()> allow_thread_start_for_testing;
    base::RepeatingClosure entered_stopping_after_unexpected_run_exit;
  };
  PreferenceControlTransport(ProfilePreferenceControlBackend*, base::FilePath);
  PreferenceControlTransport(const PreferenceControlTransport&) = delete;
  PreferenceControlTransport& operator=(const PreferenceControlTransport&) =
      delete;
  ~PreferenceControlTransport();
  // Starts worker-owned endpoint preparation without blocking this sequence.
  // The callback is posted asynchronously exactly once.
  void StartAsync(StartCallback callback);
  // Queues one start until a stopping transport reaches Stopped.
  void StartWhenStoppedAsync(StartCallback callback);
  // Cancels pending work and asynchronously tears down the worker endpoint.
  void Stop();
  void SetTestingHooksForTesting(TestingHooks hooks);
  bool running() const { return state_ == State::kReady; }
  State state() const { return state_; }
  const base::FilePath& runtime_directory() const { return runtime_directory_; }
  const base::FilePath& descriptor_path() const { return descriptor_path_; }
  const base::FilePath& credential_path() const { return credential_path_; }
  const base::FilePath& socket_path() const { return socket_path_; }

 private:
  void OnPrepared(uint64_t, PreferenceControlTransportWorker::PrepareResult);
  void OnRunExited(uint64_t generation);
  void OnStopped(uint64_t);
  void HandleRequestOnOrigin(std::string,
                             base::TimeTicks,
                             scoped_refptr<PendingRequest>);
  const raw_ptr<ProfilePreferenceControlBackend> backend_;
  const base::FilePath runtime_directory_;
  const scoped_refptr<base::SequencedTaskRunner> origin_task_runner_;
  base::FilePath descriptor_path_, credential_path_, socket_path_;
  std::unique_ptr<base::Thread> worker_thread_;
  base::SequenceBound<PreferenceControlTransportWorker> worker_;
  scoped_refptr<TransportCancellationState> cancellation_;
  State state_ = State::kStopped;
  uint64_t generation_ = 0;
  StartCallback pending_start_callback_;
  StartCallback queued_start_callback_;
  TestingHooks testing_hooks_;
  bool restart_after_stop_ = false;
  int unexpected_exit_restart_budget_ = 1;
  bool starting_auto_restart_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<PreferenceControlTransport> weak_factory_{this};
  base::WeakPtrFactory<PreferenceControlTransport> cleanup_weak_factory_{this};
};
}  // namespace limitless
#endif  // LIMITLESS_SHELL_CONTROL_PLANE_LIMITLESS_PREFERENCE_CONTROL_TRANSPORT_H_
