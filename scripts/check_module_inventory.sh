#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
command -v python3 >/dev/null 2>&1 || {
    echo "check_module_inventory: error: python3 is required" >&2
    exit 1
}
# Isolated, site-free execution prevents PYTHONPATH/sitecustomize from changing the gate.
exec python3 -I -S "$script_dir/check_module_inventory.py" "$@"
