#!/usr/bin/env bash
# P7-witness: live end-to-end run of the REAL production cadence as the LOW-PRIVILEGE
# runtime role (aimee_kb_runtime), on a hardened-provisioned Postgres.
#
# This is the live counterpart to p7_witness_runtime_role_pg_test.sql: instead of
# SET ROLE in a psql script, it runs the actual cadence C code (produce -> emit ->
# verify) in a real process that has downgraded to aimee_kb_runtime, exactly as the
# kb does on the hardened tier. It proves the runtime-role grants are sufficient for
# the real code path, and that no witness operation trips a permission error live.
#
#   run-p7-witness-runtime-live.sh <superuser libpq url>
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$ROOT/src"
HARNESS="${AIMEE_WITNESS_HARNESS:-$SRC/build/obj/tests/aimee-witness-cadence-harness}"
VERIFY="${AIMEE_WITNESS_VERIFY:-$ROOT/aimee-witness-verify}"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
log(){ printf '%s\n' "p7-witness-runtime-live: $*"; }
fail(){ log "FAIL — $*"; exit 1; }
[ -n "$BASE_URL" ] || { log "SKIP — no Postgres URL"; exit 0; }
for b in "$HARNESS" "$VERIFY"; do [ -x "$b" ] || fail "missing binary: $b"; done

ADMIN_URL="${BASE_URL%/*}/postgres"
DB="aimee_p7_witness_runtime_live"
DB_URL="${BASE_URL%/*}/$DB"
WORK="$(mktemp -d)"; HOME_DIR="$(mktemp -d)"
cleanup(){ [ -n "${PID:-}" ] && kill -9 "$PID" 2>/dev/null || true
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf "$WORK" "$HOME_DIR"; }
trap cleanup EXIT INT TERM

log "provision a hardened DB (three-role split + schema + grants) as owner"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $DB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$SRC/modules/db2/c/schema_roles.sql" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$SRC/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$SRC/modules/db2/c/schema_grants.sql" >/dev/null

log "seed evidence as owner (definer-nested write path is owner-run on the real tier)"
for i in $(seq 1 6); do
  psql -v ON_ERROR_STOP=1 "$DB_URL" -c \
    "SELECT org_vault_witness_append(1::smallint,'live$i','!kb','!reseal','','p','','g','2026-07-23T00:00:00Z',decode(repeat('c$((i%10))',32),'hex'),false,decode(repeat('00',32),'hex'))" >/dev/null
done

log "run the REAL cadence as aimee_kb_runtime (SET ROLE after owner-applied schema)"
AIMEE_TEST_PG_URL="$DB_URL" AIMEE_HOME="$HOME_DIR" \
  AIMEE_WITNESS_HARNESS_ROLE=aimee_kb_runtime AIMEE_WITNESS_CADENCE_TEST_S=1 \
  AIMEE_LOG_LEVEL=debug "$HARNESS" >"$WORK/out" 2>"$WORK/err" &
PID=$!
# Wait for the cadence to emit a checkpoint frame (proves produce+emit ran as runtime).
ok=""
for _ in $(seq 1 60); do
  grep -q "kb.witness.evidence: kind=checkpoint" "$WORK/err" 2>/dev/null && { ok=1; break; }
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.25
done
# Let a continuous-verify pass run too (it also reads the checkpoint table as runtime).
sleep 1
kill -TERM "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null; PID=""

# The harness must have actually downgraded, and nothing may have hit a permission error.
grep -q "acting as role aimee_kb_runtime" "$WORK/err" || fail "harness did not SET ROLE aimee_kb_runtime"
if grep -qiE "permission denied|must be owner|insufficient privilege" "$WORK/err"; then
  log "--- permission errors seen while running as runtime: ---"; grep -iE "permission denied|must be owner|insufficient" "$WORK/err" | head; fail "the real cadence hit a permission error as runtime"
fi
[ -n "$ok" ] || { log "--- harness stderr tail ---"; tail -20 "$WORK/err"; fail "cadence did not produce+emit a checkpoint as runtime"; }
grep -q "kb.witness.evidence: kind=record" "$WORK/err" || fail "no record evidence emitted as runtime"
grep -q "checkpoint run verified" "$WORK/err" && log "continuous verification ran as runtime"
n=$(grep -c "kb.witness.evidence" "$WORK/err")
log "cadence produced + emitted $n evidence frames while acting as aimee_kb_runtime"

log "the DB has checkpoints produced by the runtime-role process"
cpn=$(psql -tA -v ON_ERROR_STOP=1 "$DB_URL" -c "SELECT count(*) FROM kb_vault_witness_checkpoint")
[ "${cpn:-0}" -ge 1 ] || fail "no checkpoints in the DB after the runtime-role cadence run"
log "checkpoints in DB: $cpn"

log "the emitted bytes verify offline from the anchor alone"
anchor="$(awk '/^ANCHOR /{print $2; exit}' "$WORK/out")"
[ -n "$anchor" ] && [ "$anchor" != none ] || fail "no anchor published"
printf '%s\n' "$anchor" > "$WORK/anchor.txt"
: > "$WORK/stream.bin"
grep -oE "b64=[A-Za-z0-9+/=]+" "$WORK/err" | sed 's/^b64=//' | while read -r b; do printf '%s' "$b" | base64 -d >> "$WORK/stream.bin"; done
"$VERIFY" "$WORK/stream.bin" "$WORK/anchor.txt" >/dev/null || fail "emitted stream did not verify offline"

log "PASS — the real witness cadence produces/emits/verifies as aimee_kb_runtime, end to end"
exit 0
