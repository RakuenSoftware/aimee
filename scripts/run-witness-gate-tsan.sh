#!/usr/bin/env bash
# ThreadSanitizer lane for the witness release-gate verification cell.
#
# The cell (src/kb/kb_witness_gate_state.c) is the one cross-thread hand-off in the
# witness egress path: written by the cadence (kb_main loop) thread, read by the HTTP
# egress gate (listener thread). It MUST be a C11 atomic — a bare volatile/plain int
# shared across threads is a data race the C memory model leaves undefined, and this
# gates real production egress.
#
# A normal (non-instrumented) build cannot tell an atomic cell from a volatile one:
# an aligned int load is atomic on our targets regardless, so the unit test passes
# either way. ThreadSanitizer is what draws the line — it reports a data race between
# kb_witness_gate_state_set and kb_witness_gate_state_get the moment the cell stops
# being a proper atomic. This script builds ONLY that tiny cell + its race harness
# under -fsanitize=thread and runs it, so the check is fast and dependency-free.
#
# Exit 0 = clean (no race, harness assertions hold). Non-zero = a race was reported
# or the harness failed. Wire this into the same place other sanitizer lanes run.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/src"
CC="${CC:-gcc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Deterministic, loud TSan: any report aborts with a non-zero status.
export TSAN_OPTIONS="halt_on_error=1 exitcode=99 ${TSAN_OPTIONS:-}"

echo "== witness-gate TSan lane: building cell + race harness under -fsanitize=thread =="
"$CC" -std=c11 -fsanitize=thread -O1 -g -Wall -Wextra -Werror \
  -I"$SRC" \
  "$SRC/kb/kb_witness_gate_state.c" \
  "$SRC/tests/test_witness_gate_race.c" \
  -o "$OUT/witness-gate-race-tsan" -lpthread

# ThreadSanitizer's shadow-memory layout collides with high-entropy ASLR on some
# kernels ("FATAL: ThreadSanitizer: unexpected memory mapping"). Run with ASLR
# disabled for the process via setarch -R when available; it changes nothing about
# what the race detector checks. Fall back to a bare run if setarch is absent.
RUNNER=()
if command -v setarch >/dev/null 2>&1; then
  RUNNER=(setarch "$(uname -m)" -R)
fi

echo "== running under ThreadSanitizer =="
if "${RUNNER[@]}" "$OUT/witness-gate-race-tsan"; then
  echo "== witness-gate TSan lane: PASS (no data race; hand-off is a correct atomic) =="
else
  rc=$?
  echo "== witness-gate TSan lane: FAIL (rc=$rc) — the gate cell is not a race-free atomic ==" >&2
  exit "$rc"
fi
