#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
PYTHONPATH="$root" python3 "$root/benchmarks/tests/test_result_schema.py"
python3 "$root/scripts/validate_memory_gate_fixture.py" \
  "$root/benchmarks/fixtures/temporal_learning_gate.jsonl"
