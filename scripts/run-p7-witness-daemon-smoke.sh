#!/usr/bin/env bash
# P7-witness: full-daemon smoke on a real aimee-kb (.253). Boots the actual kb —
# not a harness — so the witness cadence runs in the real kb_main periodic loop,
# then seeds evidence and proves the LIVE DAEMON produces checkpoints, emits
# evidence on its real log path, keeps its health metrics current, and that the
# bytes it emitted verify offline against its own trust anchor. File custody, so the
# release gate stays closed (dev) — the gate conjunction is proven separately under
# a real anchor by run-p7-witness-boot-tpm.sh.
#
#   AIMEE_KB=/path/to/aimee-kb run-p7-witness-daemon-smoke.sh <superuser libpq url>
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KB="${AIMEE_KB:-$ROOT/aimee-kb}"
HARNESS="${AIMEE_WITNESS_HARNESS:-$ROOT/src/build/obj/tests/aimee-witness-cadence-harness}"
VERIFY="${AIMEE_WITNESS_VERIFY:-$ROOT/aimee-witness-verify}"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
PORT="${AIMEE_KB_SMOKE_PORT:-18785}"
log(){ printf '%s\n' "p7-witness-daemon-smoke: $*"; }
fail(){ log "FAIL — $*"; exit 1; }
[ -n "$BASE_URL" ] || { log "SKIP — no Postgres URL"; exit 0; }
for b in "$KB" "$HARNESS" "$VERIFY"; do [ -x "$b" ] || fail "missing binary: $b"; done

ADMIN_URL="${BASE_URL%/*}/postgres"
DB="aimee_p7_witness_daemon_smoke"; DB_URL="${BASE_URL%/*}/$DB"
WORK="$(mktemp -d)"; HOME_DIR="$(mktemp -d)"
KBPID=""
cleanup(){ [ -n "$KBPID" ] && { kill -TERM "$KBPID" 2>/dev/null; sleep 1; kill -9 "$KBPID" 2>/dev/null; } || true
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf "$WORK" "$HOME_DIR"; }
trap cleanup EXIT INT TERM

log "provision + boot the real aimee-kb daemon (file custody, compressed cadence)"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $DB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null
AIMEE_DB2_URL="$DB_URL" AIMEE_HOME="$HOME_DIR" AIMEE_WITNESS_CADENCE_TEST_S=2 \
  "$KB" --http-port="$PORT" --log-level=info >"$WORK/kb.out" 2>"$WORK/kb.err" &
KBPID=$!
for _ in $(seq 1 40); do
  curl -s "http://localhost:$PORT/v1/health" 2>/dev/null | grep -q '"db2_ok":true' && break
  kill -0 "$KBPID" 2>/dev/null || { tail -20 "$WORK/kb.err"; fail "daemon exited during boot"; }
  sleep 0.25
done
curl -s "http://localhost:$PORT/v1/health" 2>/dev/null | grep -q '"db2_ok":true' || fail "daemon health never came up"
log "daemon up; witness cadence running in kb_main"

log "seed witnessed evidence into the live DB"
for i in $(seq 1 6); do
  psql -v ON_ERROR_STOP=1 "$DB_URL" -c \
    "SELECT org_vault_witness_append(1::smallint,'smoke-$i','!kb','!reseal','','p','','g','2026-07-23T00:00:00Z',decode(repeat('a$i',32),'hex'),false,decode(repeat('00',32),'hex'))" >/dev/null
done
sleep 6  # let the cadence produce + emit a few checkpoints over the new evidence

# The daemon (not a harness) must have produced + emitted.
grep -q "kb.witness.evidence: kind=checkpoint" "$WORK/kb.err" || { tail -20 "$WORK/kb.err"; fail "daemon cadence did not emit a checkpoint"; }
grep -q "kb.witness.evidence: kind=record" "$WORK/kb.err" || fail "daemon cadence did not emit records"
grep -qiE "INTEGRITY" "$WORK/kb.err" && { grep -iE INTEGRITY "$WORK/kb.err"; fail "daemon raised a witness INTEGRITY alert"; }
cpn=$(psql -tA "$DB_URL" -c "SELECT count(*) FROM kb_vault_witness_checkpoint")
[ "${cpn:-0}" -ge 1 ] || fail "no checkpoints produced by the daemon"
log "daemon produced $cpn checkpoints and emitted $(grep -c 'kb.witness.evidence' "$WORK/kb.err") evidence frames"

log "live witness health metrics are current"
psql -tA -F= "$DB_URL" -c "SELECT metric, value FROM org_metrics_snapshot() WHERE metric LIKE 'aimee_org_witness_%' ORDER BY 1" | sed 's/^/  /'
seq_gauge=$(psql -tA "$DB_URL" -c "SELECT value::bigint FROM org_metrics_snapshot() WHERE metric='aimee_org_witness_checkpoint_seq'")
[ "${seq_gauge:-0}" -ge 1 ] || fail "checkpoint_seq gauge did not advance"
backlog=$(psql -tA "$DB_URL" -c "SELECT value::bigint FROM org_metrics_snapshot() WHERE metric='aimee_org_witness_emit_backlog_records'")
[ "${backlog:-1}" -eq 0 ] || fail "emit backlog is non-zero ($backlog) — the daemon is not keeping up"

log "the bytes the DAEMON emitted verify offline against its own anchor"
# The daemon's anchor is its KEK-derived signer identity; derive it from the same
# AIMEE_HOME via the harness (which prints ANCHOR and exits when signalled).
AIMEE_TEST_PG_URL="$DB_URL" AIMEE_HOME="$HOME_DIR" "$HARNESS" >"$WORK/anch.out" 2>/dev/null &
HPID=$!; sleep 2; kill -TERM "$HPID" 2>/dev/null; wait "$HPID" 2>/dev/null
anchor="$(awk '/^ANCHOR /{print $2; exit}' "$WORK/anch.out")"
[ -n "$anchor" ] && [ "$anchor" != none ] || fail "could not derive the daemon anchor"
printf '%s\n' "$anchor" > "$WORK/anchor.txt"
: > "$WORK/stream.bin"
grep -oE "b64=[A-Za-z0-9+/=]+" "$WORK/kb.err" | sed 's/^b64=//' | while read -r b; do printf '%s' "$b" | base64 -d >> "$WORK/stream.bin"; done
"$VERIFY" "$WORK/stream.bin" "$WORK/anchor.txt" || fail "the daemon's emitted stream did not verify offline"

log "PASS — real aimee-kb daemon: cadence produces+emits, metrics current, emitted bytes verify offline"
exit 0
