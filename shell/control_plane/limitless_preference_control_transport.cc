// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_preference_control_transport.h"

#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "limitless_preference_control_protocol.h"
#include "limitless_preference_control_service.h"

namespace limitless {

PreferenceControlTransport::PreferenceControlTransport(
    ProfilePreferenceControlBackend* backend,
    base::FilePath user_data_directory)
    : backend_(backend),
      runtime_directory_(
          user_data_directory.AppendASCII(kPreferenceControlRuntimeDirectory)),
      origin_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()),
      cleanup_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::WithBaseSyncPrimitives(),
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      worker_thread_(nullptr, base::OnTaskRunnerDeleter(cleanup_task_runner_)),
      cancellation_(base::MakeRefCounted<TransportCancellationState>()) {
  CHECK(backend_);
  CHECK(origin_task_runner_);
}

PreferenceControlTransport::~PreferenceControlTransport() {
  Stop();
  worker_.Reset();
  ReleaseWorkerThread();
}

void PreferenceControlTransport::ReleaseWorkerThread() {
  if (worker_thread_) {
    worker_thread_->DetachFromSequence();
    worker_thread_.reset();
  }
}

void PreferenceControlTransport::SetTestingHooksForTesting(TestingHooks hooks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK_EQ(state_, State::kStopped);
  testing_hooks_ = std::move(hooks);
}

void PreferenceControlTransport::StartAsync(StartCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(callback);
  if (state_ != State::kStopped) {
    const auto status =
        state_ == State::kStopping
            ? PreferenceControlTransportStartStatus::kCancelled
            : PreferenceControlTransportStartStatus::kAlreadyRunning;
    origin_task_runner_->PostTask(FROM_HERE,
                                  base::BindOnce(std::move(callback), status));
    return;
  }
  if (!starting_auto_restart_) {
    unexpected_exit_restart_budget_ = 1;
  }
  const uint64_t generation = ++generation_;
  if (backend_->is_off_the_record()) {
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback),
                       PreferenceControlTransportStartStatus::kPrivateProfile));
    return;
  }
  state_ = State::kStarting;
  cancellation_ = base::MakeRefCounted<TransportCancellationState>();
  pending_start_callback_ = std::move(callback);
  PreferenceControlTransportWorker::Inputs inputs;
  inputs.runtime_directory = runtime_directory_;
  const std::string profile(backend_->profile_identifier());
  const std::string instance(backend_->profile_instance_identifier());
  const std::string endpoint =
      profile.substr(0, 8) + "-" + instance.substr(0, 8);
  socket_path_ = runtime_directory_.AppendASCII(endpoint + ".sock");
  descriptor_path_ =
      runtime_directory_.AppendASCII(profile + "." + instance + ".json");
  credential_path_ =
      runtime_directory_.AppendASCII(profile + "." + instance + ".token");
  inputs.socket_path = socket_path_;
  inputs.descriptor_path = descriptor_path_;
  inputs.credential_path = credential_path_;
  inputs.profile_id = profile;
  inputs.instance_id = instance;
  inputs.capability = std::string(backend_->capability());
  inputs.accepted_fd_for_testing = testing_hooks_.accepted_fd;
  inputs.entering_accept_wait_for_testing = testing_hooks_.entering_accept_wait;
  inputs.should_exit_run_for_testing = testing_hooks_.should_exit_run;
  if (!worker_thread_) {
    worker_thread_.reset(
        new base::Thread("LimitlessPreferenceControlTransport"));
    const bool allowed = !testing_hooks_.allow_thread_start_for_testing ||
                         testing_hooks_.allow_thread_start_for_testing.Run();
    const bool started = allowed && worker_thread_->Start();
    if (!started) {
      ReleaseWorkerThread();
      state_ = State::kStopped;
      origin_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(
              std::move(pending_start_callback_),
              PreferenceControlTransportStartStatus::kThreadStartFailed));
      return;
    }
  }
  worker_ = base::SequenceBound<PreferenceControlTransportWorker>(
      worker_thread_->task_runner(), std::move(inputs), cancellation_);
  worker_.AsyncCall(&PreferenceControlTransportWorker::Prepare)
      .Then(base::BindOnce(&PreferenceControlTransport::OnPrepared,
                           weak_factory_.GetWeakPtr(), generation));
}

void PreferenceControlTransport::StartWhenStoppedAsync(StartCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(callback);
  if (state_ == State::kStopped) {
    StartAsync(std::move(callback));
    return;
  }
  if (state_ != State::kStopping) {
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            std::move(callback),
            state_ == State::kReady
                ? PreferenceControlTransportStartStatus::kAlreadyRunning
                : PreferenceControlTransportStartStatus::kCancelled));
    return;
  }
  if (!queued_start_callback_) {
    queued_start_callback_ = std::move(callback);
  } else {
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback),
                       PreferenceControlTransportStartStatus::kAlreadyRunning));
  }
}

void PreferenceControlTransport::OnPrepared(
    uint64_t generation,
    PreferenceControlTransportWorker::PrepareResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || state_ != State::kStarting) {
    cancellation_->Cancel();
    const uint64_t cleanup_generation = generation_;
    worker_.AsyncCall(&PreferenceControlTransportWorker::Cleanup)
        .Then(base::BindOnce(&PreferenceControlTransport::OnStopped,
                             cleanup_weak_factory_.GetWeakPtr(),
                             cleanup_generation));
    if (pending_start_callback_) {
      origin_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(pending_start_callback_),
                         PreferenceControlTransportStartStatus::kCancelled));
    }
    return;
  }
  if (!result.succeeded) {
    state_ = State::kStopping;
    const uint64_t cleanup_generation = generation_;
    worker_.AsyncCall(&PreferenceControlTransportWorker::Cleanup)
        .Then(base::BindOnce(&PreferenceControlTransport::OnStopped,
                             cleanup_weak_factory_.GetWeakPtr(),
                             cleanup_generation));
    PreferenceControlTransportStartStatus status =
        PreferenceControlTransportStartStatus::kBindFailed;
    switch (result.status) {
      case PreferenceControlTransportWorker::WorkerPrepareStatus::
          kRuntimeDirectory:
        status =
            PreferenceControlTransportStartStatus::kInvalidRuntimeDirectory;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kCredential:
        status = PreferenceControlTransportStartStatus::kCredentialWriteFailed;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kDescriptor:
        status = PreferenceControlTransportStartStatus::kDescriptorWriteFailed;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::
          kCancellationChannel:
        status =
            PreferenceControlTransportStartStatus::kCancellationChannelFailed;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kCancelled:
        status = PreferenceControlTransportStartStatus::kCancelled;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kSocket:
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kCollision:
        status = PreferenceControlTransportStartStatus::kBindFailed;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kPathTooLong:
        status = PreferenceControlTransportStartStatus::kPathTooLong;
        break;
      case PreferenceControlTransportWorker::WorkerPrepareStatus::kSuccess:
        NOTREACHED();
      default:
        break;
    }
    if (pending_start_callback_) {
      origin_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(pending_start_callback_), status));
    }
    return;
  }
  socket_path_ = result.socket_path;
  descriptor_path_ = result.descriptor_path;
  credential_path_ = result.credential_path;
  state_ = State::kReady;
  worker_.AsyncCall(&PreferenceControlTransportWorker::Run)
      .WithArgs(origin_task_runner_,
                base::BindRepeating(
                    &PreferenceControlTransport::HandleRequestOnOrigin,
                    weak_factory_.GetWeakPtr()))
      .Then(base::BindOnce(&PreferenceControlTransport::OnRunExited,
                           weak_factory_.GetWeakPtr(), generation));
  if (pending_start_callback_) {
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(pending_start_callback_),
                       PreferenceControlTransportStartStatus::kSuccess));
  }
}

void PreferenceControlTransport::OnRunExited(uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || state_ != State::kReady) {
    return;
  }
  ++generation_;
  if (unexpected_exit_restart_budget_ > 0) {
    --unexpected_exit_restart_budget_;
    restart_after_stop_ = true;
  }
  state_ = State::kStopping;
  if (testing_hooks_.entered_stopping_after_unexpected_run_exit) {
    testing_hooks_.entered_stopping_after_unexpected_run_exit.Run();
  }
  cancellation_->Cancel();
  const uint64_t cleanup_generation = generation_;
  worker_.AsyncCall(&PreferenceControlTransportWorker::Cleanup)
      .Then(base::BindOnce(&PreferenceControlTransport::OnStopped,
                           cleanup_weak_factory_.GetWeakPtr(),
                           cleanup_generation));
}

void PreferenceControlTransport::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kStopped || state_ == State::kStopping) {
    restart_after_stop_ = false;
    if (state_ == State::kStopping && queued_start_callback_) {
      origin_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(queued_start_callback_),
                         PreferenceControlTransportStartStatus::kCancelled));
    }
    return;
  }
  restart_after_stop_ = false;
  ++generation_;
  weak_factory_.InvalidateWeakPtrs();
  state_ = State::kStopping;
  cancellation_->Cancel();
  if (pending_start_callback_) {
    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(pending_start_callback_),
                       PreferenceControlTransportStartStatus::kCancelled));
  }
  const uint64_t generation = generation_;
  worker_.AsyncCall(&PreferenceControlTransportWorker::Cleanup)
      .Then(base::BindOnce(&PreferenceControlTransport::OnStopped,
                           cleanup_weak_factory_.GetWeakPtr(), generation));
}

void PreferenceControlTransport::OnStopped(uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || state_ != State::kStopping) {
    return;
  }
  const bool restart = restart_after_stop_;
  restart_after_stop_ = false;
  worker_.Reset();
  state_ = State::kStopped;
  if (restart || queued_start_callback_) {
    starting_auto_restart_ = restart;
    StartAsync(
        queued_start_callback_
            ? std::move(queued_start_callback_)
            : base::BindOnce([](PreferenceControlTransportStartStatus) {}));
    starting_auto_restart_ = false;
  }
}

void PreferenceControlTransport::HandleRequestOnOrigin(
    std::string payload,
    base::TimeTicks deadline,
    scoped_refptr<PendingRequest> pending) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  (void)deadline;
  std::string response;
  PreferenceRequestParseResult parsed = ParsePreferenceRequest(payload);
  if (!parsed.succeeded()) {
    response = SerializePreferenceRequestParseError(parsed.status);
  } else {
    PreferenceControlService service(backend_);
    response =
        SerializePreferenceControlResult(service.Execute(*parsed.request));
  }
  pending->Complete(std::move(response));
}

}  // namespace limitless
