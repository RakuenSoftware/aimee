#!/usr/bin/env bash
# On-demand CT gate for the D2b default adapter: real PostgreSQL, libtss2 and
# swtpm.  The supplied database must be a disposable aimee_p7_d2b_* database.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
src="$root/src"
db_url=${1:-${AIMEE_TEST_PG_URL:-}}
test -n "$db_url" || { echo 'usage: p7_reseal_d2b_swtpm_pg_test.sh postgres-url' >&2; exit 2; }
case "$db_url" in *[?#]*) echo 'refusing database URL with query or fragment' >&2; exit 2;; esac
db=${db_url##*/}
if ! [[ "$db" =~ ^aimee_p7_d2b_[A-Za-z0-9_]+$ ]]; then
  echo "refusing non-scratch database identifier: $db" >&2
  exit 2
fi
admin_url=${db_url%/*}/postgres
harness=${P7_D2B_HARNESS:-$src/build/obj/tests/p7-reseal-d2b-live}
port=${P7_D2B_SWTPM_PORT:-2361}
ctrl_port=${P7_D2B_SWTPM_CTRL_PORT:-2362}
work=$(mktemp -d "${TMPDIR:-/tmp}/p7-d2b-live.XXXXXX")
state="$work/tpm-state"
blob="$work/tpm2-kek.blob"
pidfile="$work/swtpm.pid"
log="$work/harness.log"
swtpm_pid=

old=0123456789abcdeffedcba98765432100f1e2d3c4b5a69788796a5b4c3d2e1f0
fixture_new=1111111111111111222222222222222233333333333333334444444444444444
secret=p7-d2b-high-entropy-operator-secret-CHANGE-ME
wrong=definitely-not-the-p7-d2b-secret
op_start=70111111111111111111111111111111
op_preparing=70222222222222222222222222222222
op_prepared=70333333333333333333333333333333
op_corrupt=70444444444444444444444444444444

say(){ printf '%s\n' "p7-d2b-swtpm-pg: $*"; }
die(){ say "FAIL: $*" >&2; exit 1; }
command -v swtpm >/dev/null || { say 'SKIP: swtpm unavailable'; exit 0; }
pkg-config --exists tss2-esys || { say 'SKIP: libtss2-esys unavailable'; exit 0; }
command -v psql >/dev/null || { say 'SKIP: psql unavailable'; exit 0; }

stop_tpm(){
  if test -n "$swtpm_pid" && kill -0 "$swtpm_pid" 2>/dev/null; then
    kill "$swtpm_pid" 2>/dev/null || true
    wait "$swtpm_pid" 2>/dev/null || true
  fi
  swtpm_pid=
}
cleanup(){
  local rc=$?
  stop_tpm
  if test "$rc" -ne 0 && test "${P7_D2B_KEEP_FAILED:-0}" = 1; then
    say "preserving failed artifacts in $work"
    return "$rc"
  fi
  dropdb --maintenance-db="$admin_url" --if-exists "$db" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

if ! test -x "$harness"; then
  rel=${harness#"$src/"}
  say "building $rel"
  make -C "$src" -j"$(nproc)" WITH_TPM2=1 "$rel"
fi
test -x "$harness" || die 'live harness missing'

export AIMEE_TEST_PG_URL="$db_url"
export AIMEE_VAULT_TPM2_BLOB_PATH="$blob"
export AIMEE_VAULT_TPM2_NV_INDEX=0x01500020

reset_db(){
  dropdb --maintenance-db="$admin_url" --if-exists "$db" >/dev/null
  createdb --maintenance-db="$admin_url" "$db"
}

reset_tpm(){
  stop_tpm
  rm -rf "$state"
  mkdir -p "$state"
  rm -f "$blob" "$blob.reseal.bundle" "$pidfile"
  swtpm socket --tpmstate "dir=$state" --ctrl "type=tcp,port=$ctrl_port" \
    --server "type=tcp,port=$port" --tpm2 --flags not-need-init,startup-clear \
    --pid "file=$pidfile" --daemon
  for _ in $(seq 1 50); do test -s "$pidfile" && break; sleep 0.1; done
  test -s "$pidfile" || die 'swtpm pidfile missing'
  swtpm_pid=$(cat "$pidfile")
  kill -0 "$swtpm_pid" 2>/dev/null || die 'swtpm failed to start'
  export AIMEE_VAULT_TPM2_TCTI="swtpm:host=127.0.0.1,port=$port"
}

fresh(){
  reset_db
  reset_tpm
  : >"$log"
  "$harness" provision "$old" "$secret" >>"$log" 2>&1
  "$harness" seed "$old" >>"$log" 2>&1
}

run(){
  if ! "$harness" "$@" >>"$log" 2>&1; then
    cat "$log" >&2
    die "harness command: $*"
  fi
}

scan_hex_canary(){
  local hex=$1 label=$2 path
  for path in "$blob" "$blob.reseal.bundle"; do
    test -e "$path" || continue
    if od -An -v -tx1 "$path" | tr -d ' \n' | grep -F "$hex" >/dev/null; then
      die "$label raw key found in $(basename "$path")"
    fi
  done
  if grep -F "$hex" "$log" >/dev/null; then
    die "$label key found in harness log"
  fi
  local hits
  hits=$(psql -Atq "$db_url" -c "SELECT
    (SELECT count(*) FROM org_vault_secret WHERE position(decode('$hex','hex') in wrapped_dek)>0)+
    (SELECT count(*) FROM org_vault_salt WHERE position(decode('$hex','hex') in kek_check)>0)+
    (SELECT count(*) FROM kb_vault_rewrap_dek_stage
       WHERE position(decode('$hex','hex') in new_wrapped_dek)>0)+
    (SELECT count(*) FROM kb_vault_rewrap_check_stage
       WHERE position(decode('$hex','hex') in new_kek_check)>0)+
    (SELECT count(*) FROM kb_vault_rewrap_operation WHERE position(decode('$hex','hex') in receipt)>0)")
  test "$hits" = 0 || die "$label raw key found in PostgreSQL"
}

say 'scenario 1: wrong secret has no PostgreSQL edge'
fresh
run run start "$op_start" "$wrong" safe_retry
run assert-state none 0
say 'PASS: wrong secret fails closed'

say 'scenario 2: START, >2 pages, installed-key verification'
fresh
cp "$blob" "$work/blob.old"
run run start "$op_start" "$secret" completed
run assert-state completed 1
test -f "$blob.reseal.bundle" || die 'D2b unexpectedly lost prepared continuation bundle'
run verify "$old" "$secret"
cp "$blob" "$work/blob.new"
cp "$work/blob.old" "$blob"
run unseal-refused "$secret"
cp "$work/blob.new" "$blob"
scan_hex_canary "$old" old
say 'PASS: START happy path and anti-rollback'

say 'scenario 3: fresh-process RESUME from preparing'
fresh
run fixture-preparing "$op_preparing" "$secret"
run run resume "$op_preparing" "$secret" completed
run verify "$old" "$secret"
scan_hex_canary "$old" old
say 'PASS: preparing boundary resume'

say 'scenario 4: fresh-process RESUME from custody_prepared'
fresh
run fixture-prepared "$op_prepared" "$secret" "$fixture_new"
run run resume "$op_prepared" "$secret" completed
run verify "$old" "$secret"
scan_hex_canary "$old" old
scan_hex_canary "$fixture_new" new
say 'PASS: custody_prepared boundary resume'

say 'scenario 5: corrupted prepared artifact quarantines durably'
fresh
run fixture-prepared "$op_corrupt" "$secret" "$fixture_new"
test -f "$blob.reseal.bundle" || die 'prepared bundle missing before corruption'
printf Z | dd of="$blob.reseal.bundle" bs=1 seek=176 count=1 conv=notrunc status=none
run run resume "$op_corrupt" "$secret" recovery_required
run assert-state recovery_required 1
scan_hex_canary "$old" old
scan_hex_canary "$fixture_new" new
say 'ALL PASS: D2b real PG17+swtpm integration'
