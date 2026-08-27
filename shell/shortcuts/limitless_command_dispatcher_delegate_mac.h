// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_SHORTCUTS_LIMITLESS_COMMAND_DISPATCHER_DELEGATE_MAC_H_
#define LIMITLESS_SHELL_SHORTCUTS_LIMITLESS_COMMAND_DISPATCHER_DELEGATE_MAC_H_

#import <AppKit/AppKit.h>

#include "base/memory/weak_ptr.h"
#import "ui/base/cocoa/command_dispatcher.h"

namespace limitless {

class LeaderShortcutController;

NSObject<CommandDispatcherDelegate>* CreateLeaderCommandDispatcherDelegate(
    base::WeakPtr<LeaderShortcutController> controller);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_SHORTCUTS_LIMITLESS_COMMAND_DISPATCHER_DELEGATE_MAC_H_
