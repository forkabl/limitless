// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_SHORTCUTS_LIMITLESS_LEADER_SHORTCUT_CONTROLLER_H_
#define LIMITLESS_SHELL_SHORTCUTS_LIMITLESS_LEADER_SHORTCUT_CONTROLLER_H_

#include <cstdint>
#include <memory>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "ui/gfx/native_ui_types.h"

namespace base {
class CommandLine;
}

namespace input {
struct NativeWebKeyboardEvent;
}

namespace ui {
class KeyEvent;
}

namespace limitless {

inline constexpr char kLeaderShortcutsSwitch[] = "limitless-leader-shortcuts";

enum class LeaderCommand {
  kNextTab,
  kPreviousTab,
  kSelectTab1,
  kSelectTab2,
  kSelectTab3,
  kSelectTab4,
  kSelectTab5,
  kSelectTab6,
  kSelectTab7,
  kSelectTab8,
  kSelectTab9,
  kCloseTab,
  kToggleSidebar,
};

bool AreLeaderShortcutsEnabled(const base::CommandLine& command_line);
base::TimeDelta GetLeaderShortcutTimeout();

// Owns the complete leader state for one normal browser window. All input
// adapters call this object, and it emits only allowlisted typed commands.
class LeaderShortcutController {
 public:
  using CanHandleCallback = base::RepeatingCallback<bool()>;
  using ExecuteCommandCallback =
      base::RepeatingCallback<bool(LeaderCommand command)>;
  using ArmedChangedCallback = base::RepeatingCallback<void(bool armed)>;

  LeaderShortcutController(bool enabled,
                           CanHandleCallback can_handle,
                           ExecuteCommandCallback execute_command,
                           ArmedChangedCallback armed_changed);
  LeaderShortcutController(const LeaderShortcutController&) = delete;
  LeaderShortcutController& operator=(const LeaderShortcutController&) = delete;
  ~LeaderShortcutController();

  bool enabled() const { return enabled_; }
  bool armed() const { return armed_; }
  uint64_t arm_generation_for_testing() const { return arm_generation_; }
  base::TimeTicks arm_deadline_for_testing() const {
    return arm_timer_.desired_run_time();
  }

  bool HandleKeyEvent(const ui::KeyEvent& event);
  bool HandleNativeWebKeyboardEvent(const input::NativeWebKeyboardEvent& event);

  void SetNativeWindow(gfx::NativeWindow window);
  void Stop();
  bool ExpireForTesting();

  base::WeakPtr<LeaderShortcutController> GetWeakPtr();

 private:
  class Impl;

  bool HandleStroke(int key_code,
                    int modifiers,
                    bool pressed,
                    bool repeated,
                    bool track_native_key_up);
  void Arm();
  void Disarm();
  void OnTimeout();
  void OnNativeWindowResigned();
  void UpdateNativeMonitor();

  const bool enabled_;
  bool armed_ = false;
  bool stopped_ = false;
  uint64_t arm_generation_ = 0;
  CanHandleCallback can_handle_;
  ExecuteCommandCallback execute_command_;
  ArmedChangedCallback armed_changed_;
  base::OneShotTimer arm_timer_;
  base::OneShotTimer consumed_key_cleanup_timer_;
  std::unique_ptr<Impl> impl_;
  base::WeakPtrFactory<LeaderShortcutController> weak_ptr_factory_{this};
};

}  // namespace limitless

#endif  // LIMITLESS_SHELL_SHORTCUTS_LIMITLESS_LEADER_SHORTCUT_CONTROLLER_H_
