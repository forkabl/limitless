// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_leader_shortcut_controller.h"

#import <AppKit/AppKit.h>

#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/apple/owned_objc.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "components/input/native_web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/content_accelerators/accelerator_util.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace limitless {
namespace {

constexpr base::TimeDelta kLeaderTimeout = base::Seconds(2);
constexpr base::TimeDelta kConsumedKeyCleanupTimeout = base::Seconds(4);
constexpr int kRelevantModifiers = ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN |
                                   ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN;

std::optional<LeaderCommand> CommandForStroke(int key_code, int modifiers) {
  if (modifiers == ui::EF_NONE) {
    switch (key_code) {
      case ui::VKEY_N:
        return LeaderCommand::kNextTab;
      case ui::VKEY_P:
        return LeaderCommand::kPreviousTab;
      case ui::VKEY_1:
      case ui::VKEY_NUMPAD1:
        return LeaderCommand::kSelectTab1;
      case ui::VKEY_2:
      case ui::VKEY_NUMPAD2:
        return LeaderCommand::kSelectTab2;
      case ui::VKEY_3:
      case ui::VKEY_NUMPAD3:
        return LeaderCommand::kSelectTab3;
      case ui::VKEY_4:
      case ui::VKEY_NUMPAD4:
        return LeaderCommand::kSelectTab4;
      case ui::VKEY_5:
      case ui::VKEY_NUMPAD5:
        return LeaderCommand::kSelectTab5;
      case ui::VKEY_6:
      case ui::VKEY_NUMPAD6:
        return LeaderCommand::kSelectTab6;
      case ui::VKEY_7:
      case ui::VKEY_NUMPAD7:
        return LeaderCommand::kSelectTab7;
      case ui::VKEY_8:
      case ui::VKEY_NUMPAD8:
        return LeaderCommand::kSelectTab8;
      case ui::VKEY_9:
      case ui::VKEY_NUMPAD9:
        return LeaderCommand::kSelectTab9;
      case ui::VKEY_B:
        return LeaderCommand::kToggleSidebar;
      default:
        return std::nullopt;
    }
  }
  if (key_code == ui::VKEY_X && modifiers == ui::EF_SHIFT_DOWN) {
    return LeaderCommand::kCloseTab;
  }
  return std::nullopt;
}

}  // namespace

class LeaderShortcutController::Impl {
 public:
  explicit Impl(LeaderShortcutController* owner) : owner_(owner) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    RemoveMonitor();
    RemoveWindowObserver();
  }

  void SetNativeWindow(NSWindow* window) {
    if (native_window_ == window) {
      return;
    }
    RemoveMonitor();
    RemoveWindowObserver();
    ClearCache();
    native_window_ = window;
    if (native_window_) {
      base::WeakPtr<LeaderShortcutController> weak_owner = owner_->GetWeakPtr();
      resign_observer_ = [NSNotificationCenter.defaultCenter
          addObserverForName:NSWindowDidResignKeyNotification
                      object:native_window_
                       queue:NSOperationQueue.mainQueue
                  usingBlock:^(NSNotification* notification) {
                    if (weak_owner) {
                      weak_owner->OnNativeWindowResigned();
                    }
                  }];
    }
  }

  NSWindow* native_window() const { return native_window_; }

  bool HasConsumedKeyUps() const { return !consumed_key_ups_.empty(); }

  bool ConsumeKeyUp(int key_code) {
    const auto iter = consumed_key_ups_.find(key_code);
    if (iter == consumed_key_ups_.end()) {
      return false;
    }
    consumed_key_ups_.erase(iter);
    return true;
  }

  bool IsConsumedKey(int key_code) const {
    return consumed_key_ups_.contains(key_code);
  }

  void RememberConsumedKeyUp(int key_code) {
    consumed_key_ups_.insert(key_code);
  }

  void ClearConsumedKeyUps() { consumed_key_ups_.clear(); }

  bool IsCachedEvent(NSEvent* event) const {
    return event && event == cached_event_ &&
           event.timestamp == cached_timestamp_ &&
           event.windowNumber == cached_window_number_ &&
           event.keyCode == cached_key_code_;
  }

  bool cached_result() const { return cached_result_; }

  void CacheEvent(NSEvent* event, bool result) {
    if (!event) {
      return;
    }
    cached_event_ = event;
    cached_timestamp_ = event.timestamp;
    cached_window_number_ = event.windowNumber;
    cached_key_code_ = event.keyCode;
    cached_result_ = result;
  }

  void ClearCache() { cached_event_ = nil; }

  void EnsureMonitor(bool needed) {
    if (!needed || !native_window_) {
      RemoveMonitor();
      return;
    }
    if (monitor_) {
      return;
    }
    base::WeakPtr<LeaderShortcutController> weak_owner = owner_->GetWeakPtr();
    monitor_ = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown |
                                             NSEventMaskKeyUp
                                     handler:^NSEvent*(NSEvent* event) {
                                       if (!weak_owner) {
                                         return event;
                                       }
                                       NSWindow* expected_window =
                                           weak_owner->impl_->native_window();
                                       if (!expected_window ||
                                           event.window != expected_window) {
                                         return event;
                                       }
                                       input::NativeWebKeyboardEvent web_event{
                                           base::apple::OwnedNSEvent(event)};
                                       return weak_owner
                                                      ->HandleNativeWebKeyboardEvent(
                                                          web_event)
                                                  ? nil
                                                  : event;
                                     }];
  }

  void RemoveMonitor() {
    if (monitor_) {
      [NSEvent removeMonitor:monitor_];
      monitor_ = nil;
    }
  }

  void RemoveWindowObserver() {
    if (resign_observer_) {
      [NSNotificationCenter.defaultCenter removeObserver:resign_observer_];
      resign_observer_ = nil;
    }
  }

 private:
  raw_ptr<LeaderShortcutController> owner_;
  NSWindow* __weak native_window_ = nil;
  id __strong monitor_ = nil;
  id __strong resign_observer_ = nil;
  std::set<int> consumed_key_ups_;
  NSEvent* __strong cached_event_ = nil;
  NSTimeInterval cached_timestamp_ = 0;
  NSInteger cached_window_number_ = 0;
  unsigned short cached_key_code_ = 0;
  bool cached_result_ = false;
};

bool AreLeaderShortcutsEnabled(const base::CommandLine& command_line) {
  if (!command_line.HasSwitch(kLeaderShortcutsSwitch)) {
    return true;
  }
  return command_line.GetSwitchValueASCII(kLeaderShortcutsSwitch) == "enabled";
}

base::TimeDelta GetLeaderShortcutTimeout() {
  return kLeaderTimeout;
}

LeaderShortcutController::LeaderShortcutController(
    bool enabled,
    CanHandleCallback can_handle,
    ExecuteCommandCallback execute_command,
    ArmedChangedCallback armed_changed)
    : enabled_(enabled),
      can_handle_(std::move(can_handle)),
      execute_command_(std::move(execute_command)),
      armed_changed_(std::move(armed_changed)),
      impl_(std::make_unique<Impl>(this)) {
  CHECK(can_handle_);
  CHECK(execute_command_);
  CHECK(armed_changed_);
}

LeaderShortcutController::~LeaderShortcutController() {
  Stop();
}

bool LeaderShortcutController::HandleKeyEvent(const ui::KeyEvent& event) {
  if (event.native_event()) {
    const input::NativeWebKeyboardEvent web_event(event);
    return HandleNativeWebKeyboardEvent(web_event);
  }
  return HandleStroke(event.key_code(), event.flags(),
                      event.type() == ui::EventType::kKeyPressed,
                      event.is_repeat(), false);
}

bool LeaderShortcutController::HandleNativeWebKeyboardEvent(
    const input::NativeWebKeyboardEvent& event) {
  NSEvent* native_event = event.os_event.Get();
  if (impl_->IsCachedEvent(native_event)) {
    return impl_->cached_result();
  }

  if (event.GetType() != blink::WebInputEvent::Type::kRawKeyDown &&
      event.GetType() != blink::WebInputEvent::Type::kKeyUp) {
    return false;
  }
  const ui::Accelerator accelerator =
      ui::GetAcceleratorFromNativeWebKeyboardEvent(event);
  const bool handled =
      HandleStroke(accelerator.key_code(), accelerator.modifiers(),
                   event.GetType() == blink::WebInputEvent::Type::kRawKeyDown,
                   accelerator.IsRepeat(), native_event != nil);
  impl_->CacheEvent(native_event, handled);
  return handled;
}

void LeaderShortcutController::SetNativeWindow(gfx::NativeWindow window) {
  NSWindow* native_window = window.GetNativeNSWindow();
  if (!native_window) {
    arm_timer_.Stop();
    consumed_key_cleanup_timer_.Stop();
    impl_->ClearConsumedKeyUps();
    Disarm();
  }
  impl_->SetNativeWindow(native_window);
  UpdateNativeMonitor();
}

void LeaderShortcutController::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  arm_timer_.Stop();
  consumed_key_cleanup_timer_.Stop();
  impl_->ClearConsumedKeyUps();
  impl_->SetNativeWindow(nil);
  if (armed_) {
    armed_ = false;
    armed_changed_.Run(false);
  }
}

bool LeaderShortcutController::ExpireForTesting() {
  if (!armed_ || !arm_timer_.IsRunning()) {
    return false;
  }
  arm_timer_.FireNow();
  return true;
}

base::WeakPtr<LeaderShortcutController> LeaderShortcutController::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

bool LeaderShortcutController::HandleStroke(int key_code,
                                            int modifiers,
                                            bool pressed,
                                            bool repeated,
                                            bool track_native_key_up) {
  if (!enabled_ || stopped_) {
    return false;
  }
  modifiers &= kRelevantModifiers;
  if (!pressed) {
    const bool consumed = impl_->ConsumeKeyUp(key_code);
    UpdateNativeMonitor();
    return consumed;
  }
  if (repeated && impl_->IsConsumedKey(key_code)) {
    return true;
  }

  if (armed_ && !can_handle_.Run()) {
    Disarm();
    return false;
  }

  const bool is_prefix =
      key_code == ui::VKEY_B && modifiers == ui::EF_CONTROL_DOWN;
  if (!armed_) {
    if (!is_prefix || !can_handle_.Run()) {
      return false;
    }
    if (!repeated) {
      if (track_native_key_up) {
        impl_->RememberConsumedKeyUp(key_code);
      }
      Arm();
    }
    UpdateNativeMonitor();
    return true;
  }

  if (is_prefix) {
    if (track_native_key_up) {
      impl_->RememberConsumedKeyUp(key_code);
    }
    UpdateNativeMonitor();
    return true;
  }

  if (key_code == ui::VKEY_ESCAPE && modifiers == ui::EF_NONE) {
    if (track_native_key_up) {
      impl_->RememberConsumedKeyUp(key_code);
    }
    Disarm();
    UpdateNativeMonitor();
    return true;
  }

  const std::optional<LeaderCommand> command =
      CommandForStroke(key_code, modifiers);
  if (!command) {
    Disarm();
    return false;
  }

  if (track_native_key_up) {
    impl_->RememberConsumedKeyUp(key_code);
  }
  Disarm();
  execute_command_.Run(*command);
  UpdateNativeMonitor();
  return true;
}

void LeaderShortcutController::Arm() {
  if (!armed_) {
    armed_ = true;
    ++arm_generation_;
    armed_changed_.Run(true);
  }
  arm_timer_.Start(FROM_HERE, kLeaderTimeout,
                   base::BindOnce(&LeaderShortcutController::OnTimeout,
                                  weak_ptr_factory_.GetWeakPtr()));
  UpdateNativeMonitor();
}

void LeaderShortcutController::Disarm() {
  arm_timer_.Stop();
  if (!armed_) {
    return;
  }
  armed_ = false;
  armed_changed_.Run(false);
  UpdateNativeMonitor();
}

void LeaderShortcutController::OnTimeout() {
  Disarm();
}

void LeaderShortcutController::OnNativeWindowResigned() {
  arm_timer_.Stop();
  consumed_key_cleanup_timer_.Stop();
  impl_->ClearConsumedKeyUps();
  Disarm();
  UpdateNativeMonitor();
}

void LeaderShortcutController::UpdateNativeMonitor() {
  const bool has_consumed_key_ups = impl_->HasConsumedKeyUps();
  impl_->EnsureMonitor(!stopped_ && (armed_ || has_consumed_key_ups));
  if (has_consumed_key_ups) {
    consumed_key_cleanup_timer_.Start(
        FROM_HERE, kConsumedKeyCleanupTimeout,
        base::BindOnce(
            [](base::WeakPtr<LeaderShortcutController> controller) {
              if (!controller) {
                return;
              }
              controller->impl_->ClearConsumedKeyUps();
              controller->UpdateNativeMonitor();
            },
            weak_ptr_factory_.GetWeakPtr()));
  } else {
    consumed_key_cleanup_timer_.Stop();
  }
}

}  // namespace limitless
