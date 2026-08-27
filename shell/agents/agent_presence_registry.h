// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_AGENTS_AGENT_PRESENCE_REGISTRY_H_
#define LIMITLESS_SHELL_AGENTS_AGENT_PRESENCE_REGISTRY_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "shell/agents/herdr_agent_provider.h"

namespace limitless {

class AgentPresenceRegistryObserver : public base::CheckedObserver {
 public:
  virtual void OnHerdrAgentSnapshotChanged(
      const HerdrAgentSnapshot& snapshot) = 0;
};

// Browser-process transient presence registry. Herdr remains authoritative;
// this class does not persist rows or grant any browser capability.
class AgentPresenceRegistry {
 public:
  explicit AgentPresenceRegistry(std::unique_ptr<HerdrAgentProvider> provider);
  AgentPresenceRegistry(const AgentPresenceRegistry&) = delete;
  AgentPresenceRegistry& operator=(const AgentPresenceRegistry&) = delete;
  ~AgentPresenceRegistry();

  void AddObserver(AgentPresenceRegistryObserver* observer);
  void RemoveObserver(AgentPresenceRegistryObserver* observer);

  const HerdrAgentSnapshot& snapshot() const { return snapshot_; }
  void RequestRefreshForTesting();

 private:
  void OnProviderSnapshot(const HerdrAgentSnapshot& snapshot);

  std::unique_ptr<HerdrAgentProvider> provider_;
  base::ObserverList<AgentPresenceRegistryObserver> observers_;
  HerdrAgentSnapshot snapshot_;
  bool started_ = false;
};

AgentPresenceRegistry& GetAgentPresenceRegistry();

}  // namespace limitless

#endif  // LIMITLESS_SHELL_AGENTS_AGENT_PRESENCE_REGISTRY_H_
