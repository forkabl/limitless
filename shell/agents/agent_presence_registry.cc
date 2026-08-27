// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/agents/agent_presence_registry.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"

namespace limitless {

AgentPresenceRegistry::AgentPresenceRegistry(
    std::unique_ptr<HerdrAgentProvider> provider)
    : provider_(std::move(provider)) {}

AgentPresenceRegistry::~AgentPresenceRegistry() {
  if (started_) {
    provider_->Stop();
  }
}

void AgentPresenceRegistry::AddObserver(
    AgentPresenceRegistryObserver* observer) {
  const bool should_start = observers_.empty();
  observers_.AddObserver(observer);
  observer->OnHerdrAgentSnapshotChanged(snapshot_);
  if (!should_start) {
    return;
  }

  snapshot_ = HerdrAgentSnapshot();
  started_ = true;
  provider_->Start(base::BindRepeating(
      &AgentPresenceRegistry::OnProviderSnapshot, base::Unretained(this)));
}

void AgentPresenceRegistry::RemoveObserver(
    AgentPresenceRegistryObserver* observer) {
  observers_.RemoveObserver(observer);
  if (!observers_.empty()) {
    return;
  }
  provider_->Stop();
  started_ = false;
  snapshot_ = HerdrAgentSnapshot();
}

void AgentPresenceRegistry::RequestRefreshForTesting() {
  if (started_) {
    provider_->RequestRefresh();
  }
}

void AgentPresenceRegistry::OnProviderSnapshot(
    const HerdrAgentSnapshot& snapshot) {
  if (!started_ || snapshot_ == snapshot) {
    return;
  }
  snapshot_ = snapshot;
  for (AgentPresenceRegistryObserver& observer : observers_) {
    observer.OnHerdrAgentSnapshotChanged(snapshot_);
  }
}

AgentPresenceRegistry& GetAgentPresenceRegistry() {
  static base::NoDestructor<AgentPresenceRegistry> registry(
      CreateHerdrCliAgentProvider());
  return *registry;
}

}  // namespace limitless
