#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
overlay="$repo_root/ungoogled-chromium-macos/build/src/chrome/browser/ui/limitless"

"$repo_root/scripts/verify_rebrand.sh"

assert_contains() {
  pattern=$1
  file=$2
  if ! rg -q -F "$pattern" "$file"; then
    echo "error: expected '$pattern' in $file" >&2
    exit 1
  fi
}

assert_contains 'static let defaultHomepageURL = "limitless://newtab/"' \
  "$overlay/BrowserSettings.swift"
assert_contains '.appendingPathComponent("Limitless", isDirectory: true)' \
  "$overlay/BrowserStore.swift"
assert_contains '.appendingPathComponent("Native", isDirectory: true)' \
  "$overlay/BrowserStore.swift"
assert_contains '"serviceName": "limitless_browser"' \
  "$overlay/CodexAppServerClient.swift"
assert_contains 'BEGIN_LIMITLESS_TOOL_RESULT' \
  "$overlay/CodexAppServerClient.swift"
assert_contains 'const key = "l" + (++registry.seq);' \
  "$overlay/BrowserAutomation.swift"

expected_tools='limitless_browser_action
limitless_browser_snapshot
limitless_get_settings
limitless_organize_tabs
limitless_search_history
limitless_update_settings'
actual_tools=$(rg -o '"name": "limitless_[a-z_]+"' "$overlay/BrowserAutomation.swift" |
  sed -e 's/"name": "//' -e 's/"$//' | sort -u)
if [ "$actual_tools" != "$expected_tools" ]; then
  echo "error: assistant tool registry does not match the Limitless contract" >&2
  printf 'expected:\n%s\nactual:\n%s\n' "$expected_tools" "$actual_tools" >&2
  exit 1
fi

echo "Limitless rebrand contract tests passed."
