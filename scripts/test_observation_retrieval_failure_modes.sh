#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" schema_data.h
make -C "$root/src" -j4 build/obj/tests/unit-test-db2 build/obj/tests/unit-test-kb-mining
"$root/src/build/obj/tests/unit-test-db2"
"$root/src/build/obj/tests/unit-test-kb-mining"
python3 "$root/scripts/check-schema-alter-order.py"
