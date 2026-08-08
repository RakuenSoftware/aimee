#!/bin/bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

python3 "${ROOT_DIR}/benchmarks/memory/poison_gate.py" \
   --output "${ROOT_DIR}/benchmarks/results/memory_poison_report.json"
"${ROOT_DIR}/benchmarks/run-llm.sh"
