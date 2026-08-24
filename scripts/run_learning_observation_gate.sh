#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" -j4 build/obj/tests/unit-test-kb-mining
"$root/src/build/obj/tests/unit-test-kb-mining"
python3 "$root/scripts/validate_memory_gate_fixture.py" \
  "$root/benchmarks/fixtures/temporal_learning_gate.jsonl" --learning
