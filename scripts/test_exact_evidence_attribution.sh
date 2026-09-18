#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" -j4 build/obj/tests/unit-test-curator-queue
"$root/src/build/obj/tests/unit-test-curator-queue"
"$root/scripts/test_temporal_assertion_retrieval.sh"
