#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 -I -S "$SCRIPT_DIR/check_git_core_contract.py" "$@"
