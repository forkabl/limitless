#include "shell/control_plane/limitless_preference_control_transport_worker.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace limitless { namespace {
struct Evidence { PreferenceControlTransportWorker::PrepareResult result; bool socket=false; bool token=false; bool descriptor=false; };
PreferenceControlTransportWorker::Inputs Inputs(const base::FilePath& p) { return {p.AppendASCII("r"),p.AppendASCII("r/d"),p.AppendASCII("r/t"),p.AppendASCII("r/s"),"p","i","cap"}; }
Evidence Journey(PreferenceControlTransportWorker::Inputs in, scoped_refptr<TransportCancellationState> c) { PreferenceControlTransportWorker w(std::move(in),c); Evidence e; e.result=w.Prepare(); e.socket=base::PathExists(e.result.socket_path); e.token=base::PathExists(e.result.credential_path); e.descriptor=base::PathExists(e.result.descriptor_path); return e; }
Evidence RunWorker(PreferenceControlTransportWorker::Inputs in, scoped_refptr<TransportCancellationState> c) { base::test::TestFuture<Evidence> f; base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,{base::MayBlock(),base::TaskPriority::USER_BLOCKING},base::BindOnce(&Journey,std::move(in),c),f.GetCallback()); return f.Take(); }
TEST(LimitlessPreferenceControlTransportWorkerTest, SuccessfulPreparePublishesExactPrivateArtifactsAndCleanup) { base::test::TaskEnvironment env; base::ScopedTempDir t; ASSERT_TRUE(t.CreateUniqueTempDir()); auto c=base::MakeRefCounted<TransportCancellationState>(); auto in=Inputs(t.GetPath()); Evidence e=RunWorker(in,c); EXPECT_TRUE(e.result.succeeded); EXPECT_TRUE(e.socket); EXPECT_TRUE(e.token); EXPECT_TRUE(e.descriptor); EXPECT_FALSE(base::PathExists(in.credential_path)); EXPECT_FALSE(base::PathExists(in.descriptor_path)); }
TEST(LimitlessPreferenceControlTransportWorkerTest, CancelBeforePreparePublishesNothingAndReturnsCancelled) { base::test::TaskEnvironment env; base::ScopedTempDir t; ASSERT_TRUE(t.CreateUniqueTempDir()); auto c=base::MakeRefCounted<TransportCancellationState>(); c->Cancel(); Evidence e=RunWorker(Inputs(t.GetPath()),c); EXPECT_EQ(e.result.status,PreferenceControlTransportWorker::WorkerPrepareStatus::kCancelled); }
TEST(LimitlessPreferenceControlTransportWorkerTest, ExistingSocketCollisionIsPreserved) { base::test::TaskEnvironment env; base::ScopedTempDir t; ASSERT_TRUE(t.CreateUniqueTempDir()); auto in=Inputs(t.GetPath()); ASSERT_TRUE(base::CreateDirectory(in.runtime_directory)); ASSERT_TRUE(base::WriteFile(in.socket_path,"foreign")); Evidence e=RunWorker(in,base::MakeRefCounted<TransportCancellationState>()); EXPECT_EQ(e.result.status,PreferenceControlTransportWorker::WorkerPrepareStatus::kCollision); EXPECT_TRUE(base::PathExists(in.socket_path)); }
TEST(LimitlessPreferenceControlTransportWorkerTest, PublicationFailureRollsBackSocketAndCredential) { base::test::TaskEnvironment env; base::ScopedTempDir t; ASSERT_TRUE(t.CreateUniqueTempDir()); auto in=Inputs(t.GetPath()); ASSERT_TRUE(base::CreateDirectory(in.runtime_directory)); ASSERT_TRUE(base::CreateDirectory(in.descriptor_path)); Evidence e=RunWorker(in,base::MakeRefCounted<TransportCancellationState>()); EXPECT_FALSE(e.result.succeeded); EXPECT_FALSE(base::PathExists(in.socket_path)); EXPECT_FALSE(base::PathExists(in.credential_path)); }
TEST(LimitlessPreferenceControlTransportWorkerTest, CleanupIsIdempotentAndForeignFilesSurvive) { base::test::TaskEnvironment env; base::ScopedTempDir t; ASSERT_TRUE(t.CreateUniqueTempDir()); auto in=Inputs(t.GetPath()); ASSERT_TRUE(base::CreateDirectory(in.runtime_directory)); ASSERT_TRUE(base::WriteFile(in.descriptor_path,"foreign")); Evidence e=RunWorker(in,base::MakeRefCounted<TransportCancellationState>()); EXPECT_TRUE(base::PathExists(in.descriptor_path)); }
} }
