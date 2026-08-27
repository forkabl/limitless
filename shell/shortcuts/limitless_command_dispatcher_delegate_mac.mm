// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_command_dispatcher_delegate_mac.h"

#include <utility>

#include "base/apple/owned_objc.h"
#import "chrome/browser/ui/cocoa/chrome_command_dispatcher_delegate.h"
#include "components/input/native_web_keyboard_event.h"
#include "limitless_leader_shortcut_controller.h"

@interface LimitlessCommandDispatcherDelegate : ChromeCommandDispatcherDelegate

- (instancetype)initWithController:
    (base::WeakPtr<limitless::LeaderShortcutController>)controller;

@end

@implementation LimitlessCommandDispatcherDelegate {
  base::WeakPtr<limitless::LeaderShortcutController> _controller;
}

- (instancetype)initWithController:
    (base::WeakPtr<limitless::LeaderShortcutController>)controller {
  if ((self = [super init])) {
    _controller = std::move(controller);
  }
  return self;
}

- (ui::PerformKeyEquivalentResult)prePerformKeyEquivalent:(NSEvent*)event
                                                   window:(NSWindow*)window {
  if (_controller && event.window == window) {
    NSResponder* responder = window.firstResponder;
    if ([responder respondsToSelector:@selector(isKeyLocked:)] &&
        [(id)responder isKeyLocked:event]) {
      return ui::PerformKeyEquivalentResult::kUnhandled;
    }
    input::NativeWebKeyboardEvent web_event{base::apple::OwnedNSEvent(event)};
    if (_controller->HandleNativeWebKeyboardEvent(web_event)) {
      return ui::PerformKeyEquivalentResult::kHandled;
    }
  }
  return [super prePerformKeyEquivalent:event window:window];
}

@end

namespace limitless {

NSObject<CommandDispatcherDelegate>* CreateLeaderCommandDispatcherDelegate(
    base::WeakPtr<LeaderShortcutController> controller) {
  if (!controller || !controller->enabled()) {
    return nil;
  }
  return [[LimitlessCommandDispatcherDelegate alloc]
      initWithController:std::move(controller)];
}

}  // namespace limitless
