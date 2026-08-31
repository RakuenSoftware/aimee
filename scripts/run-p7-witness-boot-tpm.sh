#!/usr/bin/env bash
# P7-witness-e3: the boot-REFUSAL path under a REAL custody anchor (swtpm/TPM2).
#
# Closes the one validation-pending kill-matrix item: kb_witness_boot_check only
# refuses under kb_vault_live_keys_allowed(), which requires a real, unsealed anchor.
# This starts swtpm, provisions a TPM-sealed KEK, and runs the boot-refusal harness
# (built WITH_TPM2=1) against a real Postgres to prove the composition — a no-op
# while sealed, 0 on verifiable evidence, -1 on a checkpoint signed by an
# underivable key.
#
# SKIPS CLEANLY (exit 0) when swtpm or libtss2-esys is absent, like the other TPM
# gates. Requires a superuser libpq URL for the scratch DB.
#
#   run-p7-witness-boot-tpm.sh <superuser libpq url>
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/src"
PROV_HARNESS="$SRC_DIR/build/obj/tests/p7-tpm2-harness"
BOOT_HARNESS="$SRC_DIR/build/obj/tests/aimee-witness-boot-tpm-harness"

BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
log()  { printf '%s\n' "p7-witness-boot-tpm: $*"; }
skip() { log "SKIP — $*"; exit 0; }
fail() { log "FAIL — $*"; exit 1; }

command -v swtpm >/dev/null 2>&1 || skip "swtpm not installed"
pkg-config --exists tss2-esys 2>/dev/null || skip "libtss2-esys not installed"
[ -n "$BASE_URL" ] || skip "no Postgres URL"

# Build both harnesses WITH_TPM2 if missing.
for pair in "$PROV_HARNESS:build/obj/tests/p7-tpm2-harness" \
            "$BOOT_HARNESS:build/obj/tests/aimee-witness-boot-tpm-harness"; do
  bin="${pair%%:*}"; tgt="${pair##*:}"
  [ -x "$bin" ] || { log "building $tgt WITH_TPM2 ..."; make -C "$SRC_DIR" WITH_TPM2=1 "$tgt" >/tmp/wbt-build.log 2>&1 || { cat /tmp/wbt-build.log; fail "build $tgt"; }; }
done

SWTPM_PORT=2321; SWTPM_CTRL_PORT=2322
WORK="$(mktemp -d "${TMPDIR:-/tmp}/p7-wbt.XXXXXX")" || fail "mktemp"
STATE_DIR="$WORK/tpmstate"; BLOB="$WORK/tpm2-kek.blob"; PIDFILE="$WORK/swtpm.pid"
mkdir -p "$STATE_DIR"
ADMIN_URL="${BASE_URL%/*}/postgres"
DB="aimee_p7_witness_boot_tpm_gate"; DB_URL="${BASE_URL%/*}/$DB"
SWTPM_PID=""
cleanup() {
  [ -n "$SWTPM_PID" ] && kill "$SWTPM_PID" 2>/dev/null && wait "$SWTPM_PID" 2>/dev/null
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

log "starting swtpm ..."
swtpm socket --tpmstate "dir=$STATE_DIR" --ctrl "type=tcp,port=$SWTPM_CTRL_PORT" \
  --server "type=tcp,port=$SWTPM_PORT" --tpm2 --flags not-need-init,startup-clear \
  --pid "file=$PIDFILE" --daemon || fail "swtpm start"
for _ in $(seq 1 50); do [ -s "$PIDFILE" ] && break; sleep 0.1; done
[ -s "$PIDFILE" ] || fail "swtpm no pidfile"
SWTPM_PID="$(cat "$PIDFILE")"

KEK_HEX="0123456789abcdeffedcba98765432100f1e2d3c4b5a69788796a5b4c3d2e1f0"
SECRET="p7-witness-boot-tpm-unseal-secret"
export AIMEE_VAULT_TPM2_BLOB_PATH="$BLOB"

# Provision the TPM-sealed KEK under whichever TCTI string swtpm accepts.
PROVISIONED=0; CHOSEN=""
for TCTI in "swtpm:host=127.0.0.1,port=$SWTPM_PORT" "mssim:host=127.0.0.1,port=$SWTPM_PORT"; do
  export AIMEE_VAULT_TPM2_TCTI="$TCTI"; rm -f "$BLOB"
  if "$PROV_HARNESS" provision "$KEK_HEX" "$SECRET" >/tmp/wbt-prov.log 2>&1; then PROVISIONED=1; CHOSEN="$TCTI"; break; fi
done
[ "$PROVISIONED" = 1 ] || { cat /tmp/wbt-prov.log 2>/dev/null; fail "provision under any TCTI"; }
export AIMEE_VAULT_TPM2_TCTI="$CHOSEN"
log "provisioned TPM-sealed KEK (TCTI '$CHOSEN')"

log "provisioning scratch Postgres ..."
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $DB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$SRC_DIR/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null

log "running the boot-refusal harness under the real anchor ..."
AIMEE_HOME="$WORK/home" AIMEE_TEST_PG_URL="$DB_URL" "$BOOT_HARNESS" "$SECRET"
rc=$?
if [ "$rc" -eq 3 ]; then fail "harness could not enter the live-keys regime (stub build / swtpm not wired)"; fi
[ "$rc" -eq 0 ] || fail "boot-refusal harness returned $rc"

log "PASS — boot-refusal composition proven under a real TPM2 anchor"
exit 0
