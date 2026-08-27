// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_preference_control_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/platform_thread.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/timer/elapsed_timer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless {
namespace {

base::ScopedFD ConnectToSocket(const base::FilePath& socket_path) {
  base::ScopedFD connection(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!connection.is_valid()) {
    return {};
  }
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (socket_path.value().size() >= sizeof(address.sun_path)) {
    return {};
  }
  std::memcpy(address.sun_path, socket_path.value().c_str(),
              socket_path.value().size() + 1);
  const socklen_t length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.value().size() + 1);
  if (connect(connection.get(), reinterpret_cast<sockaddr*>(&address),
              length) != 0) {
    return {};
  }
  return connection;
}

struct DripFeedResult {
  bool server_closed = false;
  base::TimeDelta elapsed;
};

DripFeedResult DripFeedFrame(const base::FilePath& socket_path) {
  base::ScopedFD connection = ConnectToSocket(socket_path);
  if (!connection.is_valid()) {
    return {};
  }
  constexpr uint32_t kPayloadSize = 20;
  const uint32_t network_size = htonl(kPayloadSize);
  if (send(connection.get(), &network_size, sizeof(network_size),
           MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(network_size))) {
    return {};
  }

  base::ElapsedTimer elapsed;
  for (size_t index = 0; index < kPayloadSize; ++index) {
    const char byte = 'x';
    if (send(connection.get(), &byte, sizeof(byte), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(sizeof(byte))) {
      return {.server_closed = true, .elapsed = elapsed.Elapsed()};
    }
    base::PlatformThread::Sleep(base::Milliseconds(300));
  }
  return {.server_closed = false, .elapsed = elapsed.Elapsed()};
}

class LimitlessPreferenceControlTransportHostileBrowserTest
    : public InProcessBrowserTest {};

void QueueRecoveryAndStop(
    PreferenceControlTransport* transport,
    base::test::TestFuture<PreferenceControlTransportStartStatus>* recovery) {
  transport->StartWhenStoppedAsync(recovery->GetCallback());
  transport->Stop();
}

PreferenceControlTransportStartStatus StartTransport(
    PreferenceControlTransport* transport) {
  base::test::TestFuture<PreferenceControlTransportStartStatus> result;
  transport->StartAsync(result.GetCallback());
  return result.Take();
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       DripFeedCannotExtendAbsoluteFrameDeadline) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));

  base::test::TestFuture<DripFeedResult> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&DripFeedFrame, transport.socket_path()),
      result.GetCallback());
  const DripFeedResult observed = result.Take();
  EXPECT_TRUE(observed.server_closed);
  EXPECT_GE(observed.elapsed, base::Seconds(1));
  EXPECT_LT(observed.elapsed, base::Seconds(4));
  transport.Stop();
  EXPECT_EQ(PreferenceControlTransport::State::kStopping, transport.state());
  ASSERT_TRUE(base::test::RunUntil([&transport] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       StopCancelsAcceptedPartialFrameAndCleansEndpoint) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  base::test::TestFuture<int> accepted_fd;
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  PreferenceControlTransport::TestingHooks testing_hooks;
  testing_hooks.accepted_fd = accepted_fd.GetSequenceBoundRepeatingCallback();
  transport.SetTestingHooksForTesting(std::move(testing_hooks));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));
  const base::FilePath descriptor_path = transport.descriptor_path();
  const base::FilePath credential_path = transport.credential_path();
  const base::FilePath socket_path = transport.socket_path();

  base::ScopedFD connection = ConnectToSocket(socket_path);
  ASSERT_TRUE(connection.is_valid());
  const char partial_header = 0;
  ASSERT_EQ(static_cast<ssize_t>(sizeof(partial_header)),
            send(connection.get(), &partial_header, sizeof(partial_header),
                 MSG_NOSIGNAL));
  ASSERT_TRUE(accepted_fd.Wait());
  EXPECT_GE(accepted_fd.Take(), 0);

  base::ElapsedTimer elapsed;
  transport.Stop();
  const base::TimeDelta stop_elapsed = elapsed.Elapsed();
  EXPECT_EQ(PreferenceControlTransport::State::kStopping, transport.state());
  EXPECT_LT(stop_elapsed, base::Seconds(1));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return transport.state() == PreferenceControlTransport::State::kStopped &&
           !base::PathExists(descriptor_path) &&
           !base::PathExists(credential_path) && !base::PathExists(socket_path);
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       StopCancelsIdleAcceptWithoutWaitingForIoDeadline) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));

  base::ElapsedTimer elapsed;
  transport.Stop();
  const base::TimeDelta stop_elapsed = elapsed.Elapsed();
  EXPECT_EQ(PreferenceControlTransport::State::kStopping, transport.state());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
  EXPECT_LT(stop_elapsed, base::Seconds(1));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       UnexpectedRunExitUsesOneRestartBudget) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  PreferenceControlTransport::TestingHooks hooks;
  auto attempts = std::make_shared<std::atomic<int>>(0);
  hooks.should_exit_run = base::BindRepeating(
      [](std::shared_ptr<std::atomic<int>> attempts) {
        return attempts->fetch_add(1) == 0;
      },
      attempts);
  transport.SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));

  ASSERT_TRUE(base::test::RunUntil([&transport, &attempts] {
    return attempts->load() == 2 &&
           transport.state() == PreferenceControlTransport::State::kReady;
  }));
  EXPECT_EQ(attempts->load(), 2);
  transport.Stop();
  ASSERT_TRUE(base::test::RunUntil([&transport] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       StopWhileStoppingCancelsQueuedRecovery) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  PreferenceControlTransport::TestingHooks hooks;
  auto attempts = std::make_shared<std::atomic<int>>(0);
  hooks.should_exit_run = base::BindRepeating(
      [](std::shared_ptr<std::atomic<int>> attempts) {
        attempts->fetch_add(1);
        return true;
      },
      attempts);
  base::test::TestFuture<PreferenceControlTransportStartStatus> recovery;
  hooks.entered_stopping_after_unexpected_run_exit =
      base::BindRepeating(&QueueRecoveryAndStop, &transport, &recovery);
  transport.SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));
  ASSERT_TRUE(base::test::RunUntil([&transport] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
  EXPECT_EQ(PreferenceControlTransportStartStatus::kCancelled, recovery.Take());
  EXPECT_EQ(attempts->load(), 1);
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       ConsecutiveUnexpectedExitsExhaustRestartBudget) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, dir.GetPath());
  auto attempts = std::make_shared<std::atomic<int>>(0);
  PreferenceControlTransport::TestingHooks hooks;
  hooks.should_exit_run = base::BindRepeating(
      [](std::shared_ptr<std::atomic<int>> attempts) {
        return attempts->fetch_add(1) < 2;
      },
      attempts);
  transport.SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(StartTransport(&transport),
            PreferenceControlTransportStartStatus::kSuccess);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return attempts->load() == 2 &&
           transport.state() == PreferenceControlTransport::State::kStopped;
  }));
  EXPECT_FALSE(base::PathExists(transport.socket_path()));
  EXPECT_FALSE(base::PathExists(transport.credential_path()));
  EXPECT_FALSE(base::PathExists(transport.descriptor_path()));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       QueuedRecoveryRearmsRestartBudget) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, dir.GetPath());
  auto attempts = std::make_shared<std::atomic<int>>(0);
  PreferenceControlTransport::TestingHooks hooks;
  hooks.should_exit_run = base::BindRepeating(
      [](std::shared_ptr<std::atomic<int>> attempts) {
        const int current = attempts->fetch_add(1) + 1;
        return current == 1 || current == 3;
      },
      attempts);
  transport.SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(StartTransport(&transport),
            PreferenceControlTransportStartStatus::kSuccess);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return attempts->load() >= 2 &&
           transport.state() == PreferenceControlTransport::State::kReady;
  }));
  transport.Stop();
  ASSERT_EQ(PreferenceControlTransport::State::kStopping, transport.state());
  base::test::TestFuture<PreferenceControlTransportStartStatus> recovery;
  transport.StartWhenStoppedAsync(recovery.GetCallback());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess, recovery.Take());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return attempts->load() == 4 &&
           transport.state() == PreferenceControlTransport::State::kReady;
  }));
  transport.Stop();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       ThreadStartFailureIsReportedExactlyOnce) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, dir.GetPath());
  PreferenceControlTransport::TestingHooks hooks;
  hooks.allow_thread_start_for_testing =
      base::BindRepeating([] { return false; });
  transport.SetTestingHooksForTesting(std::move(hooks));
  base::test::TestFuture<PreferenceControlTransportStartStatus> result;
  transport.StartAsync(result.GetCallback());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kThreadStartFailed,
            result.Take());
  EXPECT_EQ(PreferenceControlTransport::State::kStopped, transport.state());
  EXPECT_FALSE(base::PathExists(transport.runtime_directory()));
  EXPECT_FALSE(base::PathExists(transport.socket_path()));
  EXPECT_FALSE(base::PathExists(transport.credential_path()));
  EXPECT_FALSE(base::PathExists(transport.descriptor_path()));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       DedicatedWorkerThreadIsReusedAcrossRestart) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  struct ThreadState {
    std::mutex mutex;
    std::vector<base::PlatformThreadId> ids;
  };
  auto state = std::make_shared<ThreadState>();
  PreferenceControlTransport transport(&backend, dir.GetPath());
  PreferenceControlTransport::TestingHooks hooks;
  hooks.should_exit_run = base::BindRepeating(
      [](std::shared_ptr<ThreadState> state) {
        std::lock_guard lock(state->mutex);
        state->ids.push_back(base::PlatformThread::CurrentId());
        return false;
      },
      state);
  transport.SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));
  ASSERT_TRUE(base::test::RunUntil([&] {
    std::lock_guard lock(state->mutex);
    return state->ids.size() == 1;
  }));
  transport.Stop();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(&transport));
  ASSERT_TRUE(base::test::RunUntil([&] {
    std::lock_guard lock(state->mutex);
    return state->ids.size() == 2;
  }));
  transport.Stop();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return transport.state() == PreferenceControlTransport::State::kStopped;
  }));
  std::lock_guard lock(state->mutex);
  EXPECT_EQ(state->ids.at(0), state->ids.at(1));
  EXPECT_NE(state->ids.at(0), base::PlatformThread::CurrentId());
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       DestroyWhileIdleAcceptCleansEndpoint) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  auto transport =
      std::make_unique<PreferenceControlTransport>(&backend, dir.GetPath());
  base::test::TestFuture<void> entered_accept;
  PreferenceControlTransport::TestingHooks hooks;
  hooks.entering_accept_wait =
      entered_accept.GetSequenceBoundRepeatingCallback();
  transport->SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(transport.get()));
  ASSERT_TRUE(entered_accept.Wait());
  const auto socket = transport->socket_path();
  const auto token = transport->credential_path();
  const auto descriptor = transport->descriptor_path();
  base::ElapsedTimer elapsed;
  transport.reset();
  EXPECT_LT(elapsed.Elapsed(), base::Seconds(1));
  EXPECT_FALSE(base::PathExists(socket));
  EXPECT_FALSE(base::PathExists(token));
  EXPECT_FALSE(base::PathExists(descriptor));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       DestroyWithPartialFrameCleansEndpoint) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  auto transport =
      std::make_unique<PreferenceControlTransport>(&backend, dir.GetPath());
  base::test::TestFuture<int> accepted;
  PreferenceControlTransport::TestingHooks hooks;
  hooks.accepted_fd = accepted.GetSequenceBoundRepeatingCallback();
  transport->SetTestingHooksForTesting(std::move(hooks));
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess,
            StartTransport(transport.get()));
  const auto socket = transport->socket_path();
  const auto token = transport->credential_path();
  const auto descriptor = transport->descriptor_path();
  base::ScopedFD connection = ConnectToSocket(socket);
  ASSERT_TRUE(connection.is_valid());
  const char byte = 0;
  ASSERT_EQ(1, send(connection.get(), &byte, 1, MSG_NOSIGNAL));
  ASSERT_TRUE(accepted.Wait());
  base::ElapsedTimer elapsed;
  transport.reset();
  EXPECT_LT(elapsed.Elapsed(), base::Seconds(1));
  EXPECT_FALSE(base::PathExists(socket));
  EXPECT_FALSE(base::PathExists(token));
  EXPECT_FALSE(base::PathExists(descriptor));
}

}  // namespace
}  // namespace limitless
