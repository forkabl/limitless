// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_AGENTS_HERDR_AGENT_PROVIDER_H_
#define LIMITLESS_SHELL_AGENTS_HERDR_AGENT_PROVIDER_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/time/time.h"

namespace limitless {

inline constexpr char kHerdrExecutableSwitch[] = "limitless-herdr-executable";
inline constexpr int kSupportedHerdrProtocol = 20;

enum class HerdrAgentStatus {
  kIdle,
  kWorking,
  kBlocked,
  kDone,
  kUnknown,
};

enum class HerdrAgentSourceState {
  kLoading,
  kReady,
  kMissingExecutable,
  kPermissionDenied,
  kUnavailable,
  kTimedOut,
  kMalformedResponse,
  kIncompatibleProtocol,
  kPrivateProfileDisabled,
};

struct HerdrAgentPresence {
  std::string pane_id;
  std::string workspace_id;
  std::u16string workspace_label;
  std::string agent;
  HerdrAgentStatus status = HerdrAgentStatus::kUnknown;
  std::u16string terminal_title;
  bool focused = false;

  bool operator==(const HerdrAgentPresence&) const = default;
};

struct HerdrAgentSnapshot {
  HerdrAgentSourceState state = HerdrAgentSourceState::kLoading;
  std::string version;
  int protocol = 0;
  std::vector<HerdrAgentPresence> agents;

  bool operator==(const HerdrAgentSnapshot&) const = default;
};

std::string_view HerdrAgentStatusName(HerdrAgentStatus status);
std::string_view HerdrAgentSourceStateName(HerdrAgentSourceState state);

class HerdrAgentProvider {
 public:
  using SnapshotCallback =
      base::RepeatingCallback<void(const HerdrAgentSnapshot&)>;

  virtual ~HerdrAgentProvider() = default;

  virtual void Start(SnapshotCallback callback) = 0;
  virtual void RequestRefresh() = 0;
  virtual void Stop() = 0;
};

// Creates the read-only CLI adapter. The executable is resolved once from an
// explicit command-line override or a small trusted path allowlist. Every poll
// uses the exact argv `[herdr, api, snapshot]` without a shell.
std::unique_ptr<HerdrAgentProvider> CreateHerdrCliAgentProvider();

// Test-only constructor for an isolated deterministic executable. Production
// code must use CreateHerdrCliAgentProvider().
std::unique_ptr<HerdrAgentProvider> CreateHerdrCliAgentProviderForTesting(
    const base::FilePath& executable,
    base::TimeDelta poll_interval,
    base::TimeDelta request_timeout);

// Strict parser seam used by deterministic product-owned tests.
HerdrAgentSnapshot ParseHerdrAgentSnapshotForTesting(std::string_view output);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_AGENTS_HERDR_AGENT_PROVIDER_H_
