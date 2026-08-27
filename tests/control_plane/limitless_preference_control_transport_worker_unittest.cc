#include "shell/control_plane/limitless_preference_control_transport_worker.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "testing/gtest/include/gtest/gtest.h"
#include <sys/stat.h>

namespace limitless {
namespace {
struct Evidence {
  PreferenceControlTransportWorker::PrepareResult result;
  bool socket = false;
  bool token = false;
  bool descriptor = false;
  std::string token_contents;
  std::string descriptor_contents;
  struct stat runtime_stat = {};
  struct stat socket_stat = {};
  struct stat token_stat = {};
  struct stat descriptor_stat = {};
  bool socket_removed = false;
  bool token_removed = false;
  bool descriptor_removed = false;
};
PreferenceControlTransportWorker::Inputs Inputs(const base::FilePath& p) {
  return {p.AppendASCII("r"),
          p.AppendASCII("r/d"),
          p.AppendASCII("r/t"),
          p.AppendASCII("r/s"),
          "p",
          "i",
          "cap"};
}
Evidence Journey(PreferenceControlTransportWorker::Inputs in,
                 scoped_refptr<TransportCancellationState> c) {
  Evidence e;
  { PreferenceControlTransportWorker w(std::move(in), c);
    e.result = w.Prepare();
    e.socket = base::PathExists(e.result.socket_path);
    e.token = base::ReadFileToString(e.result.credential_path, &e.token_contents);
    e.descriptor = base::ReadFileToString(e.result.descriptor_path, &e.descriptor_contents);
    stat(e.result.socket_path.value().c_str(), &e.socket_stat);
    stat(e.result.credential_path.value().c_str(), &e.token_stat);
    stat(e.result.descriptor_path.value().c_str(), &e.descriptor_stat);
    stat(e.result.socket_path.DirName().value().c_str(), &e.runtime_stat);
  }
  e.socket_removed = !base::PathExists(e.result.socket_path);
  e.token_removed = !base::PathExists(e.result.credential_path);
  e.descriptor_removed = !base::PathExists(e.result.descriptor_path);
  return e;
}
Evidence RunWorker(PreferenceControlTransportWorker::Inputs in,
                   scoped_refptr<TransportCancellationState> c) {
  base::test::TestFuture<Evidence> f;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&Journey, std::move(in), c), f.GetCallback());
  return f.Take();
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     SuccessfulPreparePublishesExactPrivateArtifactsAndCleanup) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto c = base::MakeRefCounted<TransportCancellationState>();
  auto in = Inputs(t.GetPath());
  Evidence e = RunWorker(in, c);
  EXPECT_TRUE(e.result.succeeded);
  EXPECT_TRUE(e.socket);
  EXPECT_TRUE(e.token);
  EXPECT_TRUE(e.descriptor);
  EXPECT_EQ(e.token_contents, "cap");
  auto parsed = base::JSONReader::Read(e.descriptor_contents, 0);
  ASSERT_TRUE(parsed.has_value());
  const base::DictValue* dict = parsed->GetIfDict();
  ASSERT_TRUE(dict);
  EXPECT_EQ(dict->FindInt("version"), 1);
  ASSERT_TRUE(dict->FindString("profile_id"));
  EXPECT_EQ(*dict->FindString("profile_id"), "p");
  ASSERT_TRUE(dict->FindString("instance_id"));
  EXPECT_EQ(*dict->FindString("instance_id"), "i");
  ASSERT_TRUE(dict->FindString("socket_path"));
  EXPECT_EQ(*dict->FindString("socket_path"), e.result.socket_path.value());
  ASSERT_TRUE(dict->FindString("credential_path"));
  EXPECT_EQ(*dict->FindString("credential_path"), e.result.credential_path.value());
  EXPECT_TRUE(S_ISSOCK(e.socket_stat.st_mode));
  EXPECT_EQ(e.socket_stat.st_uid, geteuid());
  EXPECT_EQ(e.token_stat.st_mode & 0777, 0600);
  EXPECT_EQ(e.descriptor_stat.st_mode & 0777, 0600);
  EXPECT_EQ(e.runtime_stat.st_mode & 0777, 0700);
  EXPECT_TRUE(e.socket_removed);
  EXPECT_TRUE(e.token_removed);
  EXPECT_TRUE(e.descriptor_removed);
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     CancelBeforePreparePublishesNothingAndReturnsCancelled) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto c = base::MakeRefCounted<TransportCancellationState>();
  c->Cancel();
  Evidence e = RunWorker(Inputs(t.GetPath()), c);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCancelled);
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     ExistingSocketCollisionIsPreserved) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::CreateDirectory(in.runtime_directory));
  ASSERT_TRUE(base::WriteFile(in.socket_path, "foreign"));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kCollision);
  EXPECT_TRUE(base::PathExists(in.socket_path));
  std::string foreign; ASSERT_TRUE(base::ReadFileToString(in.socket_path, &foreign)); EXPECT_EQ(foreign, "foreign");
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     NonEnoentPreflightErrorFailsClosed) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::WriteFile(t.GetPath().AppendASCII("r"), "file"));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_FALSE(e.result.succeeded);
  EXPECT_EQ(e.result.status,
            PreferenceControlTransportWorker::WorkerPrepareStatus::kRuntimeDirectory);
  EXPECT_FALSE(base::PathExists(in.socket_path));
  EXPECT_FALSE(base::PathExists(in.credential_path));
  EXPECT_FALSE(base::PathExists(in.descriptor_path));
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     PublicationFailureRollsBackSocketAndCredential) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::CreateDirectory(in.runtime_directory));
  ASSERT_TRUE(base::CreateDirectory(in.descriptor_path));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_FALSE(e.result.succeeded);
  EXPECT_FALSE(base::PathExists(in.socket_path));
  EXPECT_FALSE(base::PathExists(in.credential_path));
}
TEST(LimitlessPreferenceControlTransportWorkerTest,
     CleanupIsIdempotentAndForeignFilesSurvive) {
  base::test::TaskEnvironment env;
  base::ScopedTempDir t;
  ASSERT_TRUE(t.CreateUniqueTempDir());
  auto in = Inputs(t.GetPath());
  ASSERT_TRUE(base::CreateDirectory(in.runtime_directory));
  ASSERT_TRUE(base::WriteFile(in.descriptor_path, "foreign"));
  Evidence e =
      RunWorker(in, base::MakeRefCounted<TransportCancellationState>());
  EXPECT_TRUE(base::PathExists(in.descriptor_path));
}
}  // namespace
}  // namespace limitless
