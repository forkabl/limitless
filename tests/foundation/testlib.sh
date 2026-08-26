#!/bin/bash

set -euo pipefail

TESTLIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
LIMITLESS_ROOT="$(cd "$TESTLIB_DIR/../.." && pwd -P)"
WORKSPACE_ROOT="$(cd "$LIMITLESS_ROOT/.." && pwd -P)"
CHROMIUM_SRC="$WORKSPACE_ROOT/chromium/src"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

pass() {
  echo "PASS: $*"
}

require_file() {
  [[ -f "$1" ]] || fail "required file not found: $1"
}

require_executable() {
  [[ -x "$1" ]] || fail "required executable not found: $1"
}

expect_failure() {
  local output_file="$1"
  shift
  set +e
  "$@" >"$output_file" 2>&1
  local status=$?
  set -e
  [[ "$status" -ne 0 ]] || fail "command unexpectedly succeeded: $*"
}

assert_output_matches() {
  local output_file="$1"
  local pattern="$2"
  grep -E -i "$pattern" "$output_file" >/dev/null || {
    sed -n '1,160p' "$output_file" >&2
    fail "output did not match expected pattern: $pattern"
  }
}

make_fixture_workspace() {
  local fixture_root
  fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/limitless-foundation.XXXXXX")"
  cp -R "$LIMITLESS_ROOT" "$fixture_root/limitless"
  mkdir -p "$fixture_root/chromium"
  ln -s "$CHROMIUM_SRC" "$fixture_root/chromium/src"
  echo "$fixture_root"
}

chromium_state_fingerprint() {
  git -C "$CHROMIUM_SRC" status --porcelain=v1 --untracked-files=all
  git -C "$CHROMIUM_SRC" diff --binary HEAD
}
