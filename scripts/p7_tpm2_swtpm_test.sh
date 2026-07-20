#!/bin/bash
# p7_tpm2_swtpm_test.sh: CT260 integration gate for the tpm2 custody provider
# (P7-tpm2a). Validates the REAL libtss2/ESAPI seal barrier against a software
# TPM 2.0 (swtpm) — swtpm implements the identical TPM2 2.0 API/semantics, so it
# fully exercises our custody code (physical anti-tamper is a hardware-deployment
# property, not something the code proves).
#
# SKIPS CLEANLY (exit 0) when the prerequisites are absent — like the Postgres
# gate skips without a URL — so it is safe in the default CI/dev pipeline:
#   - libtss2-esys not installed (cannot build WITH_TPM2=1), or
#   - swtpm not installed.
#
# On a capable host it: starts swtpm, builds the WITH_TPM2 harness
# (tests/test_vault_tpm2.c -> build/obj/tests/p7-tpm2-harness), points the TCTI at
# swtpm, provisions a KNOWN KEK under a secret, and drives the FIXED test order:
#   (a) boots SEALED           get_kek fails, live_keys FALSE
#   (b) WRONG-secret unseal     refused, STAYS sealed   (BEFORE any success)
#   (c) correct-secret unseal   get_kek == the KEK, live_keys TRUE
#   (d) seal                    sealed again, no KEK, live_keys FALSE
#   (e) fresh provider instance  re-loads the blob + unseals (persistence)
#   (f) raw blob                 the KEK bytes are ABSENT (TPM-sealed)
#   (g) re-provision w/ a blob   REFUSED (create-once)
#   (h) truncated blob           load fails closed (still sealed)
#
# The parent RUNS this on CT260; here it is authored + kept skip-clean.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)" # scripts/ lives at the repo root
SRC_DIR="$REPO_ROOT/src"
HARNESS_REL="build/obj/tests/p7-tpm2-harness"
HARNESS="$SRC_DIR/$HARNESS_REL"

SWTPM_PORT=2321
SWTPM_CTRL_PORT=2322

log() { printf '%s\n' "p7-tpm2-swtpm: $*"; }
skip() {
   log "SKIP — $*"
   exit 0
}
fail() {
   log "FAIL — $*"
   exit 1
}

# ── Prerequisite probes (skip cleanly if unmet) ──────────────────────────────
command -v swtpm >/dev/null 2>&1 || skip "swtpm not installed"
if ! pkg-config --exists tss2-esys 2>/dev/null; then
   skip "libtss2-esys not installed (cannot build WITH_TPM2=1)"
fi

# ── Isolated sandbox + cleanup ───────────────────────────────────────────────
WORK="$(mktemp -d "${TMPDIR:-/tmp}/p7-tpm2-swtpm.XXXXXX")" || fail "mktemp failed"
STATE_DIR="$WORK/tpmstate"
BLOB="$WORK/tpm2-kek.blob"
PIDFILE="$WORK/swtpm.pid"
mkdir -p "$STATE_DIR"

SWTPM_PID=""
cleanup() {
   if [ -n "$SWTPM_PID" ] && kill -0 "$SWTPM_PID" 2>/dev/null; then
      kill "$SWTPM_PID" 2>/dev/null
      wait "$SWTPM_PID" 2>/dev/null
   fi
   rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# ── Build the WITH_TPM2 harness ──────────────────────────────────────────────
log "building WITH_TPM2 harness ..."
if ! make -C "$SRC_DIR" WITH_TPM2=1 "$HARNESS_REL" >"$WORK/build.log" 2>&1; then
   cat "$WORK/build.log"
   fail "harness build (make WITH_TPM2=1 $HARNESS_REL) failed"
fi
[ -x "$HARNESS" ] || fail "harness binary missing after build"

# ── Start swtpm (MSSIM protocol on tcp: server=2321, ctrl=2322) ──────────────
log "starting swtpm ..."
swtpm socket \
   --tpmstate "dir=$STATE_DIR" \
   --ctrl "type=tcp,port=$SWTPM_CTRL_PORT" \
   --server "type=tcp,port=$SWTPM_PORT" \
   --tpm2 \
   --flags not-need-init,startup-clear \
   --pid "file=$PIDFILE" \
   --daemon || fail "swtpm failed to start"

# Wait for the pid file / listening socket.
for _ in $(seq 1 50); do
   [ -s "$PIDFILE" ] && break
   sleep 0.1
done
[ -s "$PIDFILE" ] || fail "swtpm did not write a pid file"
SWTPM_PID="$(cat "$PIDFILE")"
kill -0 "$SWTPM_PID" 2>/dev/null || fail "swtpm process not alive"

# ── Test vectors ─────────────────────────────────────────────────────────────
# A KNOWN 32-byte KEK (64 hex chars) so we can assert the exact unsealed value and
# that these bytes never appear in the on-disk blob.
KEK_HEX="0123456789abcdeffedcba98765432100f1e2d3c4b5a69788796a5b4c3d2e1f0"
SECRET="p7-tpm2a-high-entropy-operator-unseal-secret-CHANGE-ME"
WRONG_SECRET="not-the-right-secret"

export AIMEE_VAULT_TPM2_BLOB_PATH="$BLOB"

# ── Probe the TCTI string swtpm accepts (swtpm: vs mssim:), provisioning under
# the one that connects. Provision is create-once, so the winning candidate's
# provision IS the real one used by the rest of the test. ─────────────────────
PROVISIONED=0
CHOSEN_TCTI=""
for TCTI in "swtpm:host=127.0.0.1,port=$SWTPM_PORT" "mssim:host=127.0.0.1,port=$SWTPM_PORT"; do
   export AIMEE_VAULT_TPM2_TCTI="$TCTI"
   rm -f "$BLOB"
   if "$HARNESS" provision "$KEK_HEX" "$SECRET" >"$WORK/provision.log" 2>&1; then
      PROVISIONED=1
      CHOSEN_TCTI="$TCTI"
      break
   fi
done
if [ "$PROVISIONED" != "1" ]; then
   cat "$WORK/provision.log" 2>/dev/null || true
   fail "could not provision under any TCTI (swtpm/mssim)"
fi
export AIMEE_VAULT_TPM2_TCTI="$CHOSEN_TCTI"
log "provisioned under TCTI '$CHOSEN_TCTI'"

# Each step below is a FRESH process (a fresh provider instance re-loading the
# on-disk blob) — so (e) persistence is exercised inherently by re-running.
run() { # run <label> <harness-args...>
   local label="$1"
   shift
   if "$HARNESS" "$@" >"$WORK/step.log" 2>&1; then
      log "PASS ($label): $(tail -1 "$WORK/step.log")"
   else
      cat "$WORK/step.log"
      fail "$label"
   fi
}

# (a) boots SEALED
run "a: boots-sealed" sealed-check
# (b) WRONG secret -> refused, STAYS sealed (BEFORE any successful unseal)
run "b: wrong-secret-refused" unseal-fail "$WRONG_SECRET"
# (c) correct secret -> exact KEK, live_keys TRUE
run "c: correct-secret-unseal" unseal-ok "$KEK_HEX" "$SECRET"
# (d) seal -> sealed again
run "d: seal-reseals" seal-after-unseal "$KEK_HEX" "$SECRET"
# (e) fresh provider instance re-loads the blob + unseals (persistence)
run "e: fresh-instance-persistence" unseal-ok "$KEK_HEX" "$SECRET"

# (f) the KEK bytes never appear in the raw blob file (TPM-sealed).
if command -v xxd >/dev/null 2>&1; then
   BLOB_HEX="$(xxd -p "$BLOB" | tr -d '\n')"
else
   BLOB_HEX="$(od -An -v -tx1 "$BLOB" | tr -d ' \n')"
fi
case "$BLOB_HEX" in
*"$KEK_HEX"*) fail "f: KEK bytes FOUND in the raw blob (not sealed!)" ;;
*) log "PASS (f: kek-absent-from-blob): raw blob does not contain the KEK" ;;
esac

# (g) re-provision while a blob exists -> REFUSED (create-once)
run "g: reprovision-refused" reprovision-refused "$KEK_HEX" "$SECRET"

# (h) truncated/tampered blob -> load fails closed. Truncate to a stub and retry.
: >"$WORK/truncated.marker"
truncate -s 16 "$BLOB" 2>/dev/null || dd if=/dev/null of="$BLOB" bs=1 count=0 seek=16 2>/dev/null
run "h: truncated-blob-fails-closed" load-fail "$SECRET"

log "ALL PASS — real TPM2 seal barrier validated on swtpm (TCTI '$CHOSEN_TCTI')"
exit 0
