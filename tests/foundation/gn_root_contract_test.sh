#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/testlib.sh"

GN="$CHROMIUM_SRC/buildtools/mac/gn"
VERIFY="$LIMITLESS_ROOT/tools/chromium/verify"
TARGET=//chrome/browser/ui/webui_browser:browser_tests
TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/limitless-gn.XXXXXX")"
trap 'rm -rf "$TEMP_ROOT"' EXIT INT TERM

require_executable "$GN"
require_executable "$VERIFY"
require_file "$LIMITLESS_ROOT/build/chromium/BUILD.gn"

[[ "$(uname -s)" == "Darwin" ]] || fail "Phase 1 GN tests support macOS only"
[[ "$(uname -m)" == "arm64" ]] || fail "Phase 1 GN tests require macOS arm64"
"$VERIFY" --require-patched >/dev/null

gn_args() {
  local root="$1"
  echo "target_os=\"mac\" target_cpu=\"arm64\" is_debug=true is_component_build=true use_remoteexec=false chrome_pgo_phase=0 limitless_root=\"$root\""
}

EMPTY_OUT="$TEMP_ROOT/empty"
"$GN" gen "$EMPTY_OUT" \
  --root="$CHROMIUM_SRC" \
  --root-target="$TARGET" \
  --args="$(gn_args '')" >/dev/null
EMPTY_DEPS="$TEMP_ROOT/empty-deps.txt"
"$GN" desc "$EMPTY_OUT" "$TARGET" deps --all \
  --root="$CHROMIUM_SRC" >"$EMPTY_DEPS"
if grep -F 'build/chromium:' "$EMPTY_DEPS" >/dev/null; then
  fail "empty limitless_root unexpectedly included Limitless targets"
fi
pass "GN generation with empty limitless_root preserves the Chromium-only graph"

VALID_OUT="$TEMP_ROOT/valid"
"$GN" gen "$VALID_OUT" \
  --root="$CHROMIUM_SRC" \
  --root-target="$TARGET" \
  --args="$(gn_args "$LIMITLESS_ROOT")" >/dev/null
VALID_DEPS="$TEMP_ROOT/valid-deps.txt"
"$GN" desc "$VALID_OUT" "$TARGET" deps --all \
  --root="$CHROMIUM_SRC" >"$VALID_DEPS"
grep -F 'build/chromium:webui_browser_tests' \
  "$VALID_DEPS" >/dev/null || fail "valid root did not add Limitless browser tests"
grep -F 'build/chromium:resources' \
  "$VALID_DEPS" >/dev/null || fail "valid root did not add Limitless resources"
pass "GN generation with valid limitless_root includes external resources and tests"

INVALID_ROOT="$TEMP_ROOT/not-a-limitless-root"
INVALID_OUT="$TEMP_ROOT/invalid"
set +e
"$GN" gen "$INVALID_OUT" \
  --root="$CHROMIUM_SRC" \
  --root-target="$TARGET" \
  --args="$(gn_args "$INVALID_ROOT")" \
  >"$TEMP_ROOT/invalid-root.log" 2>&1
INVALID_STATUS=$?
set -e
[[ "$INVALID_STATUS" -ne 0 ]] || fail "invalid limitless_root unexpectedly generated"
assert_output_matches "$TEMP_ROOT/invalid-root.log" 'limitless_root|BUILD.gn|not.*exist|invalid'
pass "GN generation with invalid limitless_root fails with an actionable diagnostic"
