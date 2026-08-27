#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
  echo "Usage: $0 pre-patch | post-patch" >&2
}

[[ $# -eq 1 ]] || { usage; exit 2; }
case "$1" in
  pre-patch)
    exec "$SCRIPT_DIR/foundation_contract_test.sh"
    ;;
  post-patch)
    exec "$SCRIPT_DIR/gn_root_contract_test.sh"
    ;;
  -h|--help)
    usage
    ;;
  *)
    usage
    exit 2
    ;;
esac
