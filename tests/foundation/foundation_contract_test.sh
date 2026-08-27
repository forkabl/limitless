#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/testlib.sh"

VERIFY="$LIMITLESS_ROOT/tools/chromium/verify"
APPLY_PATCHES="$LIMITLESS_ROOT/tools/chromium/apply-patches"
CDP_CONTRACT_TEST="$SCRIPT_DIR/cdp_smoke_contract_test.py"
SWIFT_COMPILER_TEST="$SCRIPT_DIR/compile_mac_swift_test.py"
EXPECTED_PIN=2b4613f0a24d234b139f1246028239a2f5dde58c
TEMP_ROOTS=()

cleanup() {
  local exit_status=$?
  local root
  trap - EXIT INT TERM
  for root in "${TEMP_ROOTS[@]-}"; do
    [[ -n "$root" && -d "$root" ]] && rm -rf "$root"
  done
  exit "$exit_status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

[[ "$(uname -s)" == "Darwin" ]] || fail "Phase 1 supports macOS only"
[[ "$(uname -m)" == "arm64" ]] || fail "Phase 1 requires macOS arm64"
pass "host is macOS arm64"

require_executable "$VERIFY"
require_executable "$APPLY_PATCHES"
require_file "$LIMITLESS_ROOT/chromium.version"
require_file "$LIMITLESS_ROOT/patches/chromium/manifest.json"
require_file "$LIMITLESS_ROOT/patches/chromium/series"
require_file "$CDP_CONTRACT_TEST"
require_file "$SWIFT_COMPILER_TEST"

PYTHONDONTWRITEBYTECODE=1 python3 "$CDP_CONTRACT_TEST" >/dev/null
pass "CDP launch smoke validates shell readiness and identity before shutdown"
PYTHONDONTWRITEBYTECODE=1 python3 "$SWIFT_COMPILER_TEST" >/dev/null
pass "product-owned Swift header generation is deterministic and fails closed"
PYTHONDONTWRITEBYTECODE=1 python3 "$SCRIPT_DIR/gate2b_app_contract_test.py" >/dev/null

RECORDED_PIN="$(tr -d '[:space:]' <"$LIMITLESS_ROOT/chromium.version")"
ACTUAL_PIN="$(git -C "$CHROMIUM_SRC" rev-parse HEAD)"
[[ "$RECORDED_PIN" == "$EXPECTED_PIN" ]] || fail "unexpected project pin: $RECORDED_PIN"
[[ "$ACTUAL_PIN" == "$EXPECTED_PIN" ]] || fail "unexpected Chromium checkout: $ACTUAL_PIN"
"$VERIFY" --require-clean --json >/dev/null
pass "pin, host, repository, manifest, and checksum verification succeeds"

FIXTURE="$(make_fixture_workspace)"
TEMP_ROOTS+=("$FIXTURE")
printf '%s\n' 0000000000000000000000000000000000000000 \
  >"$FIXTURE/limitless/chromium.version"
expect_failure "$FIXTURE/pin-mismatch.log" \
  "$FIXTURE/limitless/tools/chromium/verify"
assert_output_matches "$FIXTURE/pin-mismatch.log" 'pin|revision|sha|mismatch'
pass "pin mismatch is rejected"

FIXTURE="$(make_fixture_workspace)"
TEMP_ROOTS+=("$FIXTURE")
printf '%s\n' '{' >"$FIXTURE/limitless/patches/chromium/manifest.json"
expect_failure "$FIXTURE/malformed-manifest.log" \
  "$FIXTURE/limitless/tools/chromium/verify"
assert_output_matches "$FIXTURE/malformed-manifest.log" 'manifest|json|parse|invalid'
pass "malformed manifest is rejected"

FIXTURE="$(make_fixture_workspace)"
TEMP_ROOTS+=("$FIXTURE")
FIRST_PATCH="$(sed -e 's/[[:space:]]*#.*$//' -e '/^[[:space:]]*$/d' \
  "$FIXTURE/limitless/patches/chromium/series" | head -n 1)"
[[ -n "$FIRST_PATCH" ]] || fail "patch series is empty"
printf '%s\n' '# checksum failure fixture' \
  >>"$FIXTURE/limitless/patches/chromium/$FIRST_PATCH"
expect_failure "$FIXTURE/checksum-mismatch.log" \
  "$FIXTURE/limitless/tools/chromium/verify"
assert_output_matches "$FIXTURE/checksum-mismatch.log" 'checksum|sha|hash|integrity|mismatch'
pass "patch checksum mismatch is rejected"

FIXTURE="$(make_fixture_workspace)"
TEMP_ROOTS+=("$FIXTURE")
printf '%s\n' 9999-missing-fixture.patch \
  >>"$FIXTURE/limitless/patches/chromium/series"
expect_failure "$FIXTURE/series-mismatch.log" \
  "$FIXTURE/limitless/tools/chromium/verify"
assert_output_matches "$FIXTURE/series-mismatch.log" 'series|manifest|missing|patch'
pass "series and manifest mismatch is rejected"

BEFORE_STATE="$(chromium_state_fingerprint)"
if ! "$APPLY_PATCHES" --check >/dev/null; then
  fail "strict patch dry-run failed"
fi
AFTER_STATE="$(chromium_state_fingerprint)"
[[ "$AFTER_STATE" == "$BEFORE_STATE" ]] || fail "patch dry-run changed the Chromium worktree"
pass "patch dry-run succeeds without changing Chromium"
