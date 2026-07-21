#!/bin/bash
# Real-TPM-semantics gate for P7 prepared receipt discovery and capsule KEK
# recovery. Every harness invocation is a fresh process. The gate skips cleanly
# when libtss2 or swtpm is unavailable.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/src"
HARNESS_REL="build/obj/tests/p7-tpm2-harness"
HARNESS="$SRC_DIR/$HARNESS_REL"
SWTPM_PORT=2321
SWTPM_CTRL_PORT=2322

log() { printf '%s\n' "p7-tpm2-recovery-swtpm: $*"; }
skip() { log "SKIP — $*"; exit 0; }
fail() { log "FAIL — $*"; exit 1; }

command -v swtpm >/dev/null 2>&1 || skip "swtpm not installed"
pkg-config --exists tss2-esys 2>/dev/null || skip "libtss2-esys not installed"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/p7-tpm2-recovery.XXXXXX")" || fail "mktemp"
STATE_DIR="$WORK/tpmstate"
BLOB="$WORK/tpm2-kek.blob"
BUNDLE="$BLOB.reseal.bundle"
RECEIPT="$BLOB.test-receipt"
PIDFILE="$WORK/swtpm.pid"
mkdir -p "$STATE_DIR"
SWTPM_PID=""

stop_swtpm() {
   if [ -n "$SWTPM_PID" ] && kill -0 "$SWTPM_PID" 2>/dev/null; then
      kill "$SWTPM_PID" 2>/dev/null
      wait "$SWTPM_PID" 2>/dev/null
   fi
   SWTPM_PID=""
   rm -f "$PIDFILE"
}
cleanup() { stop_swtpm; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

start_swtpm() {
   swtpm socket --tpmstate "dir=$STATE_DIR" \
      --ctrl "type=tcp,port=$SWTPM_CTRL_PORT" \
      --server "type=tcp,port=$SWTPM_PORT" --tpm2 \
      --flags not-need-init,startup-clear --pid "file=$PIDFILE" --daemon || return 1
   for _ in $(seq 1 50); do [ -s "$PIDFILE" ] && break; sleep 0.1; done
   [ -s "$PIDFILE" ] || return 1
   SWTPM_PID="$(cat "$PIDFILE")"
   kill -0 "$SWTPM_PID" 2>/dev/null
}

log "building WITH_TPM2 harness"
if ! make -C "$SRC_DIR" WITH_TPM2=1 "$HARNESS_REL" >"$WORK/build.log" 2>&1; then
   cat "$WORK/build.log"
   fail "harness build"
fi
[ -x "$HARNESS" ] || fail "harness missing"
start_swtpm || fail "start swtpm"

KEK0="0123456789abcdeffedcba98765432100f1e2d3c4b5a69788796a5b4c3d2e1f0"
KEK1="1111111111111111222222222222222233333333333333334444444444444444"
SECRET="p7-recovery-high-entropy-operator-secret-CHANGE-ME"
WRONG_SECRET="not-the-right-secret"
export AIMEE_VAULT_TPM2_BLOB_PATH="$BLOB"
export AIMEE_VAULT_TPM2_NV_INDEX="0x01500003"

PROVISIONED=0
for TCTI in "swtpm:host=127.0.0.1,port=$SWTPM_PORT" \
            "mssim:host=127.0.0.1,port=$SWTPM_PORT"; do
   export AIMEE_VAULT_TPM2_TCTI="$TCTI"
   rm -f "$BLOB"
   if "$HARNESS" provision "$KEK0" "$SECRET" >"$WORK/provision.log" 2>&1; then
      PROVISIONED=1
      break
   fi
done
[ "$PROVISIONED" = 1 ] || { cat "$WORK/provision.log"; fail "provision"; }

run() {
   local label="$1"
   shift
   if "$HARNESS" "$@" >"$WORK/step.log" 2>&1; then
      log "PASS ($label): $(tail -1 "$WORK/step.log")"
   else
      cat "$WORK/step.log"
      fail "$label"
   fi
}

restore_bundle() {
   rm -f "$BUNDLE"
   install -m 0600 "$WORK/bundle.good" "$BUNDLE" || fail "restore prepared bundle"
}

nv_gen() {
   "$HARNESS" nv-read "$SECRET" >"$WORK/nv.log" 2>&1 || fail "NV read"
   sed -n 's/.*gen=\([0-9][0-9]*\).*/\1/p' "$WORK/nv.log"
}

G0="$(nv_gen)"
run "absent discovery" prepared-discover-absent "$SECRET"
cp "$BLOB" "$WORK/blob.g0"

# The helper exits after durable prepare without saving its receipt: this is the
# exact process-crash window between custody prepare and database T2.
run "prepare without T2 receipt" prepared-prepare-drop-receipt "$KEK1" "$SECRET"
[ -f "$BUNDLE" ] || fail "prepared bundle missing"
[ ! -f "$RECEIPT" ] || fail "receipt unexpectedly persisted"
cp "$BUNDLE" "$WORK/bundle.good"

# Restart the TPM as well as the helper process; recovery uses only TPM state,
# the predecessor active blob, and the canonical prepared bundle.
stop_swtpm
start_swtpm || fail "restart swtpm after prepare"
run "discover after crash" prepared-discover "$SECRET"
run "recover exact KEK" prepared-recover "$KEK1" "$SECRET"
run "repeat recovery" prepared-recover "$KEK1" "$SECRET"
[ "$(nv_gen)" = "$G0" ] || fail "discovery/recovery changed NV"

if command -v flock >/dev/null 2>&1; then
   flock "$BLOB.reseal.lock" sleep 10 &
   LOCK_PID=$!
   sleep 0.1
   run "second-process lock is BUSY" prepared-discover-busy "$SECRET"
   kill "$LOCK_PID" 2>/dev/null || true
   wait "$LOCK_PID" 2>/dev/null || true
fi

run "wrong-secret discovery" prepared-discover-wrong-secret "$WRONG_SECRET"
run "wrong-operation discovery" prepared-discover-wrong-op "$SECRET"
run "wrong-generation discovery" prepared-discover-wrong-generation "$SECRET"
run "wrong-secret recovery" prepared-recover-refused "$WRONG_SECRET"
for field in operation old-generation new-generation predecessor capsule future new-kek manifest; do
   run "mutated receipt $field" prepared-recover-refused "$SECRET" "$field"
done

# Restore the byte-identical valid bundle after each artifact fault. No fault may
# yield receipt/plaintext, advance NV, or make the provider cache available.
mv "$BUNDLE" "$WORK/bundle.missing"
run "missing bundle discovery" prepared-discover-absent "$SECRET"
run "missing bundle recovery" prepared-recover-refused "$SECRET"
mv "$WORK/bundle.missing" "$BUNDLE"

: >"$BUNDLE"
run "truncated bundle discovery" prepared-discover-corrupt "$SECRET"
run "truncated bundle recovery" prepared-recover-refused "$SECRET"
restore_bundle

printf 'Z' | dd of="$BUNDLE" bs=1 seek=176 count=1 conv=notrunc status=none
run "corrupt capsule discovery" prepared-discover-corrupt "$SECRET"
run "corrupt capsule recovery" prepared-recover-refused "$SECRET"
restore_bundle

BUNDLE_SIZE="$(wc -c <"$BUNDLE")"
printf 'Z' | dd of="$BUNDLE" bs=1 seek="$((BUNDLE_SIZE - 1))" count=1 conv=notrunc status=none
run "corrupt future discovery" prepared-discover-corrupt "$SECRET"
run "corrupt future recovery" prepared-recover-refused "$SECRET"
restore_bundle

chmod 0644 "$BUNDLE"
run "unsafe permissions discovery" prepared-discover-corrupt "$SECRET"
run "unsafe permissions recovery" prepared-recover-refused "$SECRET"
restore_bundle

mv "$BUNDLE" "$WORK/bundle.target"
ln -s "$WORK/bundle.target" "$BUNDLE"
run "symlink discovery" prepared-discover-corrupt "$SECRET"
run "symlink recovery" prepared-recover-refused "$SECRET"
rm "$BUNDLE"
mv "$WORK/bundle.target" "$BUNDLE"

[ "$(nv_gen)" = "$G0" ] || fail "fault tests changed NV"
run "abort prepared" prepared-abort "$SECRET"
run "repeat abort" prepared-abort "$SECRET"
run "old active KEK after abort" unseal-ok "$KEK0" "$SECRET"

# Full forward path: recovery is legal only at PREPARED/G. At G+1 and after
# cleanup it must return no plaintext; the new active blob alone unseals.
run "prepare forward flow" prepared-prepare "$KEK1" "$SECRET"
run "discover forward flow" prepared-discover "$SECRET"
run "recover before commit" prepared-recover "$KEK1" "$SECRET"
cp "$BLOB" "$WORK/blob.precommit"
run "commit prepared" prepared-commit "$SECRET"
G1="$(nv_gen)"
[ "$G1" -gt "$G0" ] || fail "commit did not advance NV"
run "recovery refused at installed" prepared-recover-refused "$SECRET"
run "new active KEK" unseal-ok "$KEK1" "$SECRET"
cp "$BLOB" "$WORK/blob.g1"
if command -v xxd >/dev/null 2>&1; then
   for artifact in "$BLOB" "$BUNDLE" "$WORK/bundle.good"; do
      if xxd -p "$artifact" | tr -d '\n' | grep -F "$KEK1" >/dev/null; then
         fail "raw KEK canary found in persisted artifact"
      fi
   done
fi
cp "$WORK/blob.precommit" "$BLOB"
run "old blob replay refused" unseal-refused "$SECRET"
cp "$WORK/blob.g1" "$BLOB"
run "cleanup" prepared-cleanup "$SECRET"
run "recovery refused after cleanup" prepared-recover-refused "$SECRET"
run "cleaned discovery is absent" prepared-discover-absent "$SECRET"

log "ALL PASS — prepared receipt discovery and capsule KEK recovery validated"
