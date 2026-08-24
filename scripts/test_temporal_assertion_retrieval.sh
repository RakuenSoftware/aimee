#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" -j4 build/obj/tests/unit-test-typed-facts
"$root/src/build/obj/tests/unit-test-typed-facts"
