// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_WORKSPACES_LIMITLESS_WORKSPACE_SESSION_PERSISTENCE_H_
#define LIMITLESS_SHELL_WORKSPACES_LIMITLESS_WORKSPACE_SESSION_PERSISTENCE_H_

#include "limitless_workspace_controller.h"

class Profile;

namespace limitless {

WorkspacePersistenceCallbacks CreateWorkspaceSessionPersistence(
    Profile* profile);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_WORKSPACES_LIMITLESS_WORKSPACE_SESSION_PERSISTENCE_H_
