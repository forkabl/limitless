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

#include <cstring>

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

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       DripFeedCannotExtendAbsoluteFrameDeadline) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  }
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess, transport.Start());

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
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       StopCancelsAcceptedPartialFrameAndCleansEndpoint) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  }
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess, transport.Start());
  const base::FilePath descriptor_path = transport.descriptor_path();
  const base::FilePath credential_path = transport.credential_path();
  const base::FilePath socket_path = transport.socket_path();

  base::ScopedFD connection = ConnectToSocket(socket_path);
  ASSERT_TRUE(connection.is_valid());
  const char partial_header = 0;
  ASSERT_EQ(static_cast<ssize_t>(sizeof(partial_header)),
            send(connection.get(), &partial_header, sizeof(partial_header),
                 MSG_NOSIGNAL));
  ASSERT_TRUE(base::test::RunUntil([&transport] {
    return transport.has_accepted_connection_for_testing();
  }));

  base::ElapsedTimer elapsed;
  transport.Stop();
  EXPECT_LT(elapsed.Elapsed(), base::Seconds(1));
  EXPECT_FALSE(base::PathExists(descriptor_path));
  EXPECT_FALSE(base::PathExists(credential_path));
  EXPECT_FALSE(base::PathExists(socket_path));
}

IN_PROC_BROWSER_TEST_F(LimitlessPreferenceControlTransportHostileBrowserTest,
                       StopCancelsIdleAcceptWithoutWaitingForIoDeadline) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir user_data_directory;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(user_data_directory.CreateUniqueTempDir());
  }
  ProfilePreferenceControlBackend backend(browser()->GetProfile());
  PreferenceControlTransport transport(&backend, user_data_directory.GetPath());
  ASSERT_EQ(PreferenceControlTransportStartStatus::kSuccess, transport.Start());

  base::ElapsedTimer elapsed;
  transport.Stop();
  EXPECT_LT(elapsed.Elapsed(), base::Seconds(1));
}

}  // namespace
}  // namespace limitless
