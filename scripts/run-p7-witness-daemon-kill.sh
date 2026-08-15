#!/usr/bin/env bash
# P7-witness-e3 §1: real-process cadence liveness + hard-kill recovery.
#
# Drives the REAL production witness cadence (kb_witness_cadence_tick /
# kb_witness_boot_check, via aimee-witness-cadence-harness) in a separate process,
# proves it actually produces and emits evidence on the live log path, SIGKILLs it
# (the kill-matrix event — a hard kill, possibly mid-cadence, not a clean stop),
# restarts it, and verifies that the bytes it emitted to its real log verify offline
# from the trust anchor alone.
#
# This is the process-level complement to the deterministic recovery test: it
# exercises the actual wall-clock timer, a real SIGKILL (Postgres aborts any
# in-flight cadence transaction), and a real restart — against the production tick.
#
# Custody-independent: file custody suffices, since the cadence signs with the
# KEK-derived witness key. It therefore does NOT exercise the boot-refusal path
# (which needs a real custody anchor — TPM/HSM/KMS).
#
#   scripts/run-p7-witness-daemon-kill.sh postgres://.../postgres
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "usage: $0 <superuser libpq url>" >&2
  exit 2
fi
HARNESS="${AIMEE_WITNESS_HARNESS:-$ROOT/src/build/obj/tests/aimee-witness-cadence-harness}"
VERIFY="${AIMEE_WITNESS_VERIFY:-$ROOT/aimee-witness-verify}"
for b in "$HARNESS" "$VERIFY"; do
  [ -x "$b" ] || { echo "missing binary: $b" >&2; exit 1; }
done

ADMIN_URL="${BASE_URL%/*}/postgres"
DB="aimee_p7_witness_daemon_gate"
DB_URL="${BASE_URL%/*}/$DB"
WORK="$(mktemp -d)"
HOME_DIR="$(mktemp -d)"
cleanup() {
  [ -n "${PID:-}" ] && kill -9 "$PID" 2>/dev/null || true
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf "$WORK" "$HOME_DIR"
}
trap cleanup EXIT

echo "== provision =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $DB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null

echo "== seed witness evidence =="
for i in $(seq 1 8); do
  psql -v ON_ERROR_STOP=1 "$DB_URL" -c \
    "SELECT org_vault_witness_append(0::smallint,'seed$i','!kb','!audit','','p','','g','2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),true,decode(repeat('00',32),'hex'))" >/dev/null
done

launch() { # $1 = stderr log, $2 = stdout(anchor) log
  AIMEE_TEST_PG_URL="$DB_URL" AIMEE_HOME="$HOME_DIR" AIMEE_WITNESS_CADENCE_TEST_S=1 \
    AIMEE_LOG_LEVEL=info "$HARNESS" >"$2" 2>"$1" &
  PID=$!
}

echo "== run 1: real cadence, then a hard SIGKILL =="
launch "$WORK/run1.err" "$WORK/run1.out"
# Wait for the cadence to emit a checkpoint frame (INFO level; the "checkpoint
# signed" line is DEBUG). A checkpoint frame on the log path is the proof the real
# cadence produced AND emitted.
signalled=""
for _ in $(seq 1 60); do
  if grep -q "kb.witness.evidence: kind=checkpoint" "$WORK/run1.err" 2>/dev/null; then signalled=1; break; fi
  # bail early if the harness died at boot
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.25
done
kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; PID=""

[ -n "$signalled" ] || { echo "FAIL: real cadence never emitted a checkpoint on the log path"; echo "--- harness stderr ---"; sed -n "1,40p" "$WORK/run1.err"; exit 1; }
grep -q "kb.witness.evidence: kind=record" "$WORK/run1.err" || { echo "FAIL: no record evidence on the log path"; exit 1; }
n1=$(grep -c "kb.witness.evidence" "$WORK/run1.err" || true)
echo "run 1: cadence produced a checkpoint and emitted $n1 evidence frames, then was SIGKILLed"

echo "== run 2: restart after the hard kill =="
launch "$WORK/run2.err" "$WORK/run2.out"
# Poll for the restart to actually emit — do NOT rely on a fixed sleep being longer
# than the cadence period. The restart must RECOVER and emit, not merely come up:
# after a hard kill the cursor may be behind (re-emission) and the still-running
# cadence produces new checkpoints, so a healthy restart emits at least one frame.
# Zero would mean the post-kill cadence produced but the sink never delivered — a
# silent failure the clean-startup check alone would miss.
recovered=""
for _ in $(seq 1 60); do
  if grep -q "kb.witness.evidence" "$WORK/run2.err" 2>/dev/null; then recovered=1; break; fi
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.25
done
kill -TERM "$PID"; wait "$PID" 2>/dev/null || true; PID=""
grep -q "HARNESS STOPPED" "$WORK/run2.out" || { echo "FAIL: restart did not come up and stop cleanly"; sed -n '1,20p' "$WORK/run2.err"; exit 1; }
n2=$(grep -c "kb.witness.evidence" "$WORK/run2.err" || true)
[ -n "$recovered" ] && [ "${n2:-0}" -ge 1 ] || { echo "FAIL: restart came up but emitted no evidence (silent recovery failure)"; exit 1; }
echo "run 2: restarted and recovered, emitted $n2 evidence frames"

echo "== rebuild the retained stream from the REAL emitted log lines =="
# The anchor is on stdout; evidence is on stderr as 'kind=... b64=<BASE64>'.
anchor_line="$(awk '/^ANCHOR /{print $2; exit}' "$WORK/run1.out")"
[ -n "$anchor_line" ] && [ "$anchor_line" != "none" ] || { echo "FAIL: no anchor published"; exit 1; }
printf '%s\n' "$anchor_line" > "$WORK/anchor.txt"
# Concatenate every emitted frame from both runs, in log order, decoding base64.
: > "$WORK/stream.bin"
for f in "$WORK/run1.err" "$WORK/run2.err"; do
  grep -oE "b64=[A-Za-z0-9+/=]+" "$f" | sed 's/^b64=//' | while read -r b; do
    printf '%s' "$b" | base64 -d >> "$WORK/stream.bin"
  done
done
bytes=$(wc -c < "$WORK/stream.bin")
echo "retained stream: $bytes bytes from the real log path"
[ "$bytes" -gt 0 ] || { echo "FAIL: no evidence bytes captured from the log"; exit 1; }

echo "== verify the emitted bytes offline (anchor only, no database) =="
set +e
"$VERIFY" "$WORK/stream.bin" "$WORK/anchor.txt"
vrc=$?
set -e
[ "$vrc" -eq 0 ] || { echo "FAIL: the real emitted stream did not verify offline (rc=$vrc)"; exit 1; }

echo "== database still consistent after the hard kill =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "SELECT org_vault_witness_verify_shard('!kb','!audit')" >/dev/null
# No sequence gap: row count equals max(shard_seq).
gap=$(psql -tA -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT (count(*) <> max(shard_seq))::int FROM kb_vault_witness_log WHERE tenant='!kb' AND provider='!audit'")
[ "$gap" = "0" ] || { echo "FAIL: witness shard has a sequence gap after the kill"; exit 1; }

echo "== P7-witness-e3 daemon kill gate: PASSED =="
