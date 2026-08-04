#!/usr/bin/env bash
# ThreadSanitizer lane for the event-bus arena lease table.
#
# bus_arena's lease table is host-private but not single-threaded: a co-located
# producer (D7) allocates and fills leases from its own thread while the host's
# pump thread publishes and releases them, and a consumer thread reads and
# releases in place (see docs/dev/EVENT_BUS_FEATURE_TREE.md). Every table
# transition is guarded by a->lock; the byte fill/read happen outside it, kept
# safe by the refcount.
#
# A normal build cannot prove the lock actually covers every shared access — the
# unit tests drive the arena single-threaded. ThreadSanitizer draws the line: it
# reports a data race the moment any table field is touched off-lock. This script
# builds ONLY the bus sources + the cross-thread harness under -fsanitize=thread
# and runs it, so the check is self-contained and dependency-free.
#
# Exit 0 = clean (no race, harness assertions hold, arena drained). Non-zero = a
# race was reported or the harness failed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/src"
CC="${CC:-gcc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Deterministic, loud TSan: any report aborts with a non-zero status.
export TSAN_OPTIONS="halt_on_error=1 exitcode=99 ${TSAN_OPTIONS:-}"

echo "== bus-arena TSan lane: building bus sources + race harness under -fsanitize=thread =="
# Mirror the project's warning posture (src/Makefile): the bus sources are built
# with -Wno-unused-parameter etc., so a bare -Wall -Wextra -Werror would reject
# code that ships clean.
"$CC" -std=c11 -fsanitize=thread -O1 -g -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-format-truncation -Wno-unused-result \
  -I"$SRC" -I"$SRC/core/event_bus/include" \
  "$SRC/core/event_bus/bus_attach.c" \
  "$SRC/core/event_bus/bus_arena.c" \
  "$SRC/core/event_bus/bus_host.c" \
  "$SRC/core/event_bus/bus_route.c" \
  "$SRC/core/event_bus/bus_region.c" \
  "$SRC/core/event_bus/bus_region_host.c" \
  "$SRC/core/event_bus/bus_ring.c" \
  "$SRC/core/event_bus/bus_wire.c" \
  "$SRC/core/event_bus/bus_client.c" \
  "$SRC/tests/test_bus_arena_tsan.c" \
  -o "$OUT/bus-arena-race-tsan" -lpthread

# ThreadSanitizer's shadow-memory layout collides with high-entropy ASLR on some
# kernels ("FATAL: ThreadSanitizer: unexpected memory mapping"). Run with ASLR
# disabled via setarch -R when available; it changes nothing the detector checks.
RUNNER=()
if command -v setarch >/dev/null 2>&1; then
  RUNNER=(setarch "$(uname -m)" -R)
fi

echo "== running under ThreadSanitizer =="
if "${RUNNER[@]}" "$OUT/bus-arena-race-tsan"; then
  echo "== bus-arena TSan lane: PASS (no data race; the lease table is properly serialised) =="
else
  rc=$?
  echo "== bus-arena TSan lane: FAIL (rc=$rc) — a lease-table access is not race-free ==" >&2
  exit "$rc"
fi
