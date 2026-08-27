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
SHELL_URL=chrome://webui-browser/
CHROMIUM_REVISION=""
EXPECTED_BUILD_ID=""

usage() {
  echo "Usage: $0 [--app PATH] [--artifacts DIR] [--startup-timeout SECONDS] [--shutdown-timeout SECONDS]" >&2
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$LIMITLESS_ROOT/chromium.version" ]] || \
  fail "Chromium pin not found: $LIMITLESS_ROOT/chromium.version"
CHROMIUM_REVISION="$(<"$LIMITLESS_ROOT/chromium.version")"
[[ "$CHROMIUM_REVISION" =~ ^[0-9a-f]{40}$ ]] || \
  fail "chromium.version does not contain a lowercase 40-character SHA"
EXPECTED_BUILD_ID="limitless-phase1-macos-arm64-${CHROMIUM_REVISION:0:8}"

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

[[ "$(uname -s)" == "Darwin" ]] || fail "the Phase 1 launch smoke test supports macOS only"
[[ "$(uname -m)" == "arm64" ]] || fail "the Phase 1 launch smoke test requires macOS arm64"
[[ -d "$APP_PATH" ]] || fail "app bundle not found: $APP_PATH"
[[ -f "$APP_PATH/Contents/Info.plist" ]] || fail "Info.plist not found in app bundle"

if [[ -z "$ARTIFACT_DIR" ]]; then
  ARTIFACT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/limitless-e2e.XXXXXX")"
else
  mkdir -p "$ARTIFACT_DIR"
  ARTIFACT_DIR="$(cd "$ARTIFACT_DIR" && pwd -P)"
fi

PROFILE_DIR="$(mktemp -d "$ARTIFACT_DIR/profile.XXXXXX")"
STDOUT_LOG="$ARTIFACT_DIR/browser.stdout.log"
STDERR_LOG="$ARTIFACT_DIR/browser.stderr.log"
CHROME_LOG="$ARTIFACT_DIR/chrome_debug.log"
RESULT_FILE="$ARTIFACT_DIR/result.txt"
CDP_RESULT="$ARTIFACT_DIR/cdp_validation.json"
touch "$STDOUT_LOG" "$STDERR_LOG" "$CHROME_LOG"
PLIST_BUDDY=/usr/libexec/PlistBuddy
[[ -x "$PLIST_BUDDY" ]] || fail "PlistBuddy is required to resolve the app executable"

BUNDLE_EXECUTABLE="$($PLIST_BUDDY -c 'Print :CFBundleExecutable' "$APP_PATH/Contents/Info.plist")"
BINARY_PATH="$APP_PATH/Contents/MacOS/$BUNDLE_EXECUTABLE"
[[ -x "$BINARY_PATH" ]] || fail "app executable is missing or not executable: $BINARY_PATH"

BROWSER_PID=""
TEST_SUCCEEDED=0
browser_is_running() {
  [[ -n "$BROWSER_PID" ]] || return 1
  kill -0 "$BROWSER_PID" 2>/dev/null || return 1
  local process_state
  process_state="$(ps -p "$BROWSER_PID" -o stat= 2>/dev/null || true)"
  [[ -n "$process_state" && "$process_state" != Z* ]]
}

cleanup() {
  local exit_status=$?
  trap - EXIT INT TERM
  if browser_is_running; then
    kill -TERM "$BROWSER_PID" 2>/dev/null || true
    wait "$BROWSER_PID" 2>/dev/null || true
  fi
  if [[ "$TEST_SUCCEEDED" -eq 1 ]]; then
    rm -rf "$PROFILE_DIR"
  else
    echo "E2E artifacts retained at: $ARTIFACT_DIR" >&2
  fi
  exit "$exit_status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$BINARY_PATH" \
  --user-data-dir="$PROFILE_DIR" \
  --enable-features=Webium,SurfaceEmbed \
  --no-first-run \
  --no-default-browser-check \
  --remote-debugging-port=0 \
  --enable-logging=stderr \
  --log-file="$CHROME_LOG" \
  about:blank \
  >"$STDOUT_LOG" 2>"$STDERR_LOG" &
BROWSER_PID=$!

SECONDS_WAITED=0
while [[ ! -s "$PROFILE_DIR/DevToolsActivePort" ]]; do
  browser_is_running || fail "browser exited before publishing DevToolsActivePort"
  [[ "$SECONDS_WAITED" -lt "$STARTUP_TIMEOUT" ]] || fail "browser did not become ready within ${STARTUP_TIMEOUT}s"
  sleep 1
  SECONDS_WAITED=$((SECONDS_WAITED + 1))
done

python3 "$SCRIPT_DIR/cdp_close.py" \
  "$PROFILE_DIR" \
  --timeout "$STARTUP_TIMEOUT" \
  --url "$SHELL_URL" \
  --expected-build-id "$EXPECTED_BUILD_ID" \
  --expected-chromium-revision "$CHROMIUM_REVISION" \
  >"$CDP_RESULT"

SECONDS_WAITED=0
while browser_is_running; do
  [[ "$SECONDS_WAITED" -lt "$SHUTDOWN_TIMEOUT" ]] || fail "browser did not exit cleanly within ${SHUTDOWN_TIMEOUT}s"
  sleep 1
  SECONDS_WAITED=$((SECONDS_WAITED + 1))
done

set +e
wait "$BROWSER_PID"
BROWSER_STATUS=$?
set -e
BROWSER_PID=""
[[ "$BROWSER_STATUS" -eq 0 ]] || fail "browser exited with status $BROWSER_STATUS"

for singleton_path in SingletonCookie SingletonLock SingletonSocket; do
  [[ ! -e "$PROFILE_DIR/$singleton_path" && ! -L "$PROFILE_DIR/$singleton_path" ]] || \
    fail "unclean profile lock remains: $singleton_path"
done

if grep -E 'FATAL:|CHECK failed|AddressSanitizer|UndefinedBehaviorSanitizer' "$STDERR_LOG" "$CHROME_LOG" >/dev/null 2>&1; then
  fail "fatal or sanitizer output was captured in browser logs"
fi

{
  echo "result=pass"
  echo "app=$APP_PATH"
  echo "executable=$BINARY_PATH"
  echo "profile_is_temporary=true"
  echo "shell_url=$SHELL_URL"
  echo "shell_ready=true"
  echo "build_id=$EXPECTED_BUILD_ID"
  echo "chromium_revision=$CHROMIUM_REVISION"
  echo "cdp_validation=$CDP_RESULT"
  echo "shutdown=Browser.close"
  echo "stdout_log=$STDOUT_LOG"
  echo "stderr_log=$STDERR_LOG"
  echo "chrome_log=$CHROME_LOG"
} >"$RESULT_FILE"

TEST_SUCCEEDED=1
echo "PASS: live Limitless shell validation and clean shutdown"
echo "Artifacts: $ARTIFACT_DIR"
