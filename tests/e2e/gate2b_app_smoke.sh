#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
LIMITLESS_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
WORKSPACE_ROOT="$(cd "$LIMITLESS_ROOT/.." && pwd -P)"
CHROMIUM_SRC="$WORKSPACE_ROOT/chromium/src"
APP_PATH="$CHROMIUM_SRC/out/LimitlessDebug/Limitless.app"
ARTIFACT_DIR=""
STARTUP_TIMEOUT=45
SHUTDOWN_TIMEOUT=30
EXPECTED_BUNDLE_ID=com.limitless.browser
EXPECTED_EXECUTABLE=Limitless

usage() {
  echo "Usage: $0 [--app PATH] [--artifacts DIR] [--startup-timeout SECONDS] [--shutdown-timeout SECONDS]" >&2
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      APP_PATH="$2"
      shift 2
      ;;
    --artifacts)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      ARTIFACT_DIR="$2"
      shift 2
      ;;
    --startup-timeout)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      STARTUP_TIMEOUT="$2"
      shift 2
      ;;
    --shutdown-timeout)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      SHUTDOWN_TIMEOUT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      fail "unknown argument: $1"
      ;;
  esac
done

[[ "$STARTUP_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || fail "startup timeout must be a positive integer"
[[ "$SHUTDOWN_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || fail "shutdown timeout must be a positive integer"
[[ "$(uname -s)" == "Darwin" ]] || fail "the Gate 2B app smoke supports macOS only"
[[ "$(uname -m)" == "arm64" ]] || fail "the Gate 2B app smoke requires macOS arm64"
[[ -f "$LIMITLESS_ROOT/chromium.version" ]] || fail "Chromium pin not found"
CHROMIUM_REVISION="$(tr -d '[:space:]' <"$LIMITLESS_ROOT/chromium.version")"
[[ "$CHROMIUM_REVISION" =~ ^[0-9a-f]{40}$ ]] || \
  fail "chromium.version does not contain a lowercase 40-character SHA"

[[ -d "$APP_PATH" ]] || fail "app bundle not found: $APP_PATH"
INFO_PLIST="$APP_PATH/Contents/Info.plist"
[[ -f "$INFO_PLIST" ]] || fail "Info.plist not found in app bundle"
PLIST_BUDDY=/usr/libexec/PlistBuddy
[[ -x "$PLIST_BUDDY" ]] || fail "PlistBuddy is required to resolve app identity"
BUNDLE_EXECUTABLE="$($PLIST_BUDDY -c 'Print :CFBundleExecutable' "$INFO_PLIST")"
BUNDLE_IDENTIFIER="$($PLIST_BUDDY -c 'Print :CFBundleIdentifier' "$INFO_PLIST")"
BUNDLE_SCM_REVISION="$($PLIST_BUDDY -c 'Print :SCMRevision' "$INFO_PLIST")"
[[ "$BUNDLE_EXECUTABLE" == "$EXPECTED_EXECUTABLE" ]] || \
  fail "unexpected app executable identity: $BUNDLE_EXECUTABLE"
[[ "$BUNDLE_IDENTIFIER" == "$EXPECTED_BUNDLE_ID" ]] || \
  fail "unexpected app bundle identity: $BUNDLE_IDENTIFIER"
[[ "$BUNDLE_SCM_REVISION" == "$CHROMIUM_REVISION"-* ]] || \
  fail "app build revision does not match chromium.version: $BUNDLE_SCM_REVISION"
BINARY_PATH="$APP_PATH/Contents/MacOS/$BUNDLE_EXECUTABLE"
[[ -x "$BINARY_PATH" ]] || fail "app executable is missing or not executable: $BINARY_PATH"

if [[ -z "$ARTIFACT_DIR" ]]; then
  ARTIFACT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/limitless-gate2b-e2e.XXXXXX")"
else
  mkdir -p "$ARTIFACT_DIR"
  ARTIFACT_DIR="$(cd "$ARTIFACT_DIR" && pwd -P)"
fi

PROFILE_DIR="$(mktemp -d "$ARTIFACT_DIR/profile.XXXXXX")"
STDOUT_LOG="$ARTIFACT_DIR/browser.stdout.log"
STDERR_LOG="$ARTIFACT_DIR/browser.stderr.log"
CHROME_LOG="$ARTIFACT_DIR/chrome_debug.log"
PROBE_BUILD_LOG="$ARTIFACT_DIR/macos_boundary_probe.build.log"
PROBE_BINARY="$ARTIFACT_DIR/macos_boundary_probe"
CDP_RESULT="$ARTIFACT_DIR/gate2b_cdp_validation.json"
LIFECYCLE_RESULT="$ARTIFACT_DIR/gate2b_native_lifecycle.json"
NATIVE_SCREENSHOT="$ARTIFACT_DIR/gate2b-native-whole-window.png"
RESULT_FILE="$ARTIFACT_DIR/result.txt"
BROWSER_STATUS_FILE="$ARTIFACT_DIR/browser_exit_status.txt"
touch "$STDOUT_LOG" "$STDERR_LOG" "$CHROME_LOG" "$PROBE_BUILD_LOG"

SWIFTC="$(xcrun --find swiftc 2>/dev/null)" || fail "Xcode swiftc is required"
[[ -x "$SWIFTC" ]] || fail "Xcode swiftc is not executable: $SWIFTC"
MACOS_SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)" || fail "macOS SDK is required"
[[ -d "$MACOS_SDK" ]] || fail "macOS SDK was not found: $MACOS_SDK"
if ! "$SWIFTC" -O \
  -warnings-as-errors \
  -sdk "$MACOS_SDK" \
  -o "$PROBE_BINARY" \
  "$SCRIPT_DIR/macos_boundary_probe.swift" \
  >"$PROBE_BUILD_LOG" 2>&1; then
  fail "macOS boundary probe failed to compile; see $PROBE_BUILD_LOG"
fi
[[ -x "$PROBE_BINARY" ]] || fail "compiled macOS boundary probe is missing"

BROWSER_PID=""
LAUNCHED_BROWSER_PID=""
BROWSER_REAPED=0
BROWSER_STATUS=""
TEST_SUCCEEDED=0
browser_is_running() {
  [[ -n "$LAUNCHED_BROWSER_PID" && "$BROWSER_REAPED" -eq 0 ]] || return 1
  kill -0 "$LAUNCHED_BROWSER_PID" 2>/dev/null || return 1
  local process_state
  process_state="$(ps -p "$LAUNCHED_BROWSER_PID" -o stat= 2>/dev/null || true)"
  [[ -n "$process_state" && "$process_state" != Z* ]]
}

reap_browser() {
  [[ -n "$LAUNCHED_BROWSER_PID" && "$BROWSER_REAPED" -eq 0 ]] || return 0
  if wait "$LAUNCHED_BROWSER_PID"; then
    BROWSER_STATUS=0
  else
    BROWSER_STATUS=$?
  fi
  BROWSER_REAPED=1
  {
    echo "browser_pid=$LAUNCHED_BROWSER_PID"
    echo "browser_exit_status=$BROWSER_STATUS"
  } >"$BROWSER_STATUS_FILE"
}

cleanup() {
  local exit_status=$?
  trap - EXIT INT TERM
  if [[ -n "$LAUNCHED_BROWSER_PID" && "$BROWSER_REAPED" -eq 0 ]]; then
    if browser_is_running; then
      kill -TERM "$LAUNCHED_BROWSER_PID" 2>/dev/null || true
      for _ in {1..50}; do
        browser_is_running || break
        sleep 0.1
      done
      if browser_is_running; then
        kill -KILL "$LAUNCHED_BROWSER_PID" 2>/dev/null || true
      fi
    fi
    reap_browser
  fi
  if [[ "$TEST_SUCCEEDED" -eq 1 ]]; then
    rm -rf "$PROFILE_DIR"
  else
    echo "Gate 2B E2E artifacts retained at: $ARTIFACT_DIR" >&2
  fi
  exit "$exit_status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

E2E_STARTED_SECONDS=$SECONDS
E2E_DEADLINE_SECONDS=$((E2E_STARTED_SECONDS + STARTUP_TIMEOUT))
"$BINARY_PATH" \
  --user-data-dir="$PROFILE_DIR" \
  --disable-features=Webium \
  --limitless-swiftui-boundary-probe \
  --no-first-run \
  --no-default-browser-check \
  --remote-debugging-port=0 \
  --enable-logging=stderr \
  --log-file="$CHROME_LOG" \
  about:blank \
  >"$STDOUT_LOG" 2>"$STDERR_LOG" &
BROWSER_PID=$!
LAUNCHED_BROWSER_PID="$BROWSER_PID"

while [[ ! -s "$PROFILE_DIR/DevToolsActivePort" ]]; do
  browser_is_running || fail "browser exited before publishing DevToolsActivePort"
  [[ "$SECONDS" -lt "$E2E_DEADLINE_SECONDS" ]] || \
    fail "browser did not become ready within ${STARTUP_TIMEOUT}s"
  sleep 1
done

VALIDATION_TIMEOUT=$((E2E_DEADLINE_SECONDS - SECONDS))
[[ "$VALIDATION_TIMEOUT" -gt 0 ]] || \
  fail "browser readiness exhausted the ${STARTUP_TIMEOUT}s end-to-end validation deadline"
PYTHONDONTWRITEBYTECODE=1 python3 "$SCRIPT_DIR/gate2b_cdp.py" \
  "$PROFILE_DIR" \
  --browser-pid "$BROWSER_PID" \
  --expected-chromium-revision "$CHROMIUM_REVISION" \
  --native-probe "$PROBE_BINARY" \
  --screenshot "$NATIVE_SCREENSHOT" \
  --timeout "$VALIDATION_TIMEOUT" \
  >"$CDP_RESULT"

[[ -s "$NATIVE_SCREENSHOT" ]] || fail "native whole-window screenshot was not captured"
[[ -s "$CDP_RESULT" ]] || fail "Gate 2B CDP validation result was not captured"
NATIVE_WINDOW_ID="$(PYTHONDONTWRITEBYTECODE=1 python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["native_boundary"]["cg_window_id"])' \
  "$CDP_RESULT")"
[[ "$NATIVE_WINDOW_ID" =~ ^[1-9][0-9]*$ ]] || fail "native window ID was invalid"

SECONDS_WAITED=0
while browser_is_running; do
  [[ "$SECONDS_WAITED" -lt "$SHUTDOWN_TIMEOUT" ]] || \
    fail "browser did not exit cleanly within ${SHUTDOWN_TIMEOUT}s"
  sleep 1
  SECONDS_WAITED=$((SECONDS_WAITED + 1))
done

reap_browser
[[ "$BROWSER_STATUS" -eq 0 ]] || fail "browser exited with status $BROWSER_STATUS"

"$PROBE_BINARY" \
  --pid "$LAUNCHED_BROWSER_PID" \
  --assert-absent-window-id "$NATIVE_WINDOW_ID" \
  --timeout "$SHUTDOWN_TIMEOUT" \
  >"$LIFECYCLE_RESULT"
[[ -s "$LIFECYCLE_RESULT" ]] || fail "native window lifecycle result was not captured"

for singleton_path in SingletonCookie SingletonLock SingletonSocket; do
  [[ ! -e "$PROFILE_DIR/$singleton_path" && ! -L "$PROFILE_DIR/$singleton_path" ]] || \
    fail "unclean profile lock remains: $singleton_path"
done

if grep -E 'FATAL:|CHECK failed|AddressSanitizer|UndefinedBehaviorSanitizer' \
  "$STDERR_LOG" "$CHROME_LOG" >/dev/null 2>&1; then
  fail "fatal or sanitizer output was captured in browser logs"
fi

{
  echo "result=pass"
  echo "app=$APP_PATH"
  echo "bundle_identifier=$BUNDLE_IDENTIFIER"
  echo "bundle_scm_revision=$BUNDLE_SCM_REVISION"
  echo "executable=$BINARY_PATH"
  echo "browser_pid=$LAUNCHED_BROWSER_PID"
  echo "webium=disabled"
  echo "swiftui_boundary_switch=enabled"
  echo "cdp_validation=$CDP_RESULT"
  echo "native_window_id=$NATIVE_WINDOW_ID"
  echo "native_whole_window_screenshot=$NATIVE_SCREENSHOT"
  echo "native_lifecycle=$LIFECYCLE_RESULT"
  echo "focus_method=macos-accessibility-AXFocused"
  echo "native_keyboard_input=false"
  echo "shutdown=Browser.close"
  echo "browser_exit_status_file=$BROWSER_STATUS_FILE"
  echo "profile_is_temporary=true"
  echo "stdout_log=$STDOUT_LOG"
  echo "stderr_log=$STDERR_LOG"
  echo "chrome_log=$CHROME_LOG"
} >"$RESULT_FILE"

TEST_SUCCEEDED=1
echo "PASS: real Limitless BrowserView hosted and focused the SwiftUI boundary in one native window"
echo "Artifacts: $ARTIFACT_DIR"
