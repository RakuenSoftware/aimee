#!/bin/bash
# p7_tpm2b_swtpm_test.sh: CT260 integration gate for the tpm2 custody provider's
# P7-tpm2b TPM-ENFORCED anti-rollback (NV monotonic counter + PolicyNV). Validates
# the REAL libtss2/ESAPI path against a software TPM 2.0 (swtpm) — swtpm implements
# the identical TPM2 NV/policy semantics, so it fully exercises the anti-rollback
# code (the TPM ITSELF refusing to unseal a stale-generation blob via PolicyNV).
#
# SKIPS CLEANLY (exit 0) when prerequisites are absent — like the tpm2a gate — so it
# is safe in the default CI/dev pipeline:
#   - libtss2-esys not installed (cannot build WITH_TPM2=1), or
#   - swtpm not installed.
#
# On a capable host it starts swtpm, builds the WITH_TPM2 harness
# (tests/test_vault_tpm2.c -> build/obj/tests/p7-tpm2-harness), provisions a KNOWN
# KEK, and drives the anti-rollback assertions:
#   (i) provision           first NV increment ok; NV_Read == the bound generation G0
#   (c) correct unseal      unseal -> KEK0
#   (j) reseal -> NV++      new blob unseals to KEK1; RESTORE the OLD (G0) blob ->
#                           Esys_Unseal FAILS via PolicyNV AT THE TPM (not our software:
#                           the failure is at Esys_PolicyNV, before Unseal + before the
#                           defence-in-depth gen-compare ever runs)
#   (l) two reseals         G0<G1<G2, each PRIOR blob refused, NV never decrements
#   (m) wrong-secret NV++    a reseal under the WRONG secret cannot bump the counter
#   (k) v1 blob             a tpm2a (v1) blob is REFUSED ("re-provision to v2")
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

log() { printf '%s\n' "p7-tpm2b-swtpm: $*"; }
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/p7-tpm2b-swtpm.XXXXXX")" || fail "mktemp failed"
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

for _ in $(seq 1 50); do
   [ -s "$PIDFILE" ] && break
   sleep 0.1
done
[ -s "$PIDFILE" ] || fail "swtpm did not write a pid file"
SWTPM_PID="$(cat "$PIDFILE")"
kill -0 "$SWTPM_PID" 2>/dev/null || fail "swtpm process not alive"

# ── Test vectors ─────────────────────────────────────────────────────────────
KEK0="0123456789abcdeffedcba98765432100f1e2d3c4b5a69788796a5b4c3d2e1f0"
KEK1="1111111111111111222222222222222233333333333333334444444444444444"
KEK2="aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd"
SECRET="p7-tpm2b-high-entropy-operator-unseal-secret-CHANGE-ME"
WRONG_SECRET="not-the-right-secret"

export AIMEE_VAULT_TPM2_BLOB_PATH="$BLOB"
# Use a dedicated NV index for the test (mirrors the config/env plumbing under test).
export AIMEE_VAULT_TPM2_NV_INDEX="0x01500001"

# ── Probe the TCTI string swtpm accepts, provisioning under the one that connects.
PROVISIONED=0
CHOSEN_TCTI=""
for TCTI in "swtpm:host=127.0.0.1,port=$SWTPM_PORT" "mssim:host=127.0.0.1,port=$SWTPM_PORT"; do
   export AIMEE_VAULT_TPM2_TCTI="$TCTI"
   rm -f "$BLOB"
   if "$HARNESS" provision "$KEK0" "$SECRET" >"$WORK/provision.log" 2>&1; then
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

# Each step is a FRESH process (a fresh provider instance re-loading the on-disk blob).
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

# Read the NV counter generation via the harness (prints "... gen=N").
nv_gen() { # nv_gen <secret>
   "$HARNESS" nv-read "$1" >"$WORK/nv.log" 2>&1 || {
      cat "$WORK/nv.log"
      fail "nv-read failed"
   }
   sed -n 's/.*gen=\([0-9][0-9]*\).*/\1/p' "$WORK/nv.log"
}

# (i) provision did the first NV increment; NV_Read returns a usable generation G0,
# and the fresh blob unseals to KEK0 (the unseal's own defence-in-depth check asserts
# NV_Read == the bound generation).
G0="$(nv_gen "$SECRET")"
[ -n "$G0" ] || fail "i: could not read NV generation after provision"
log "PASS (i: provision+first-increment): NV generation G0=$G0"
run "c: correct-unseal-KEK0" unseal-ok "$KEK0" "$SECRET"

# Snapshot the G0 blob so we can attempt a rollback later.
cp -f "$BLOB" "$WORK/blob.g0" || fail "snapshot g0"

# (j) reseal to KEK1 -> NV bumps to G1; the NEW blob unseals to KEK1.
run "j: reseal-to-G1" reseal "$KEK1" "$SECRET"
G1="$(nv_gen "$SECRET")"
[ -n "$G1" ] || fail "j: could not read NV generation after reseal"
[ "$G1" -gt "$G0" ] || fail "j: NV not monotonic (G1=$G1 !> G0=$G0)"
log "PASS (j: NV incremented): G0=$G0 -> G1=$G1"
run "j: new-blob-unseals-KEK1" unseal-ok "$KEK1" "$SECRET"
cp -f "$BLOB" "$WORK/blob.g1" || fail "snapshot g1"

# (j) RESTORE the OLD (G0) blob -> unseal REFUSED at the TPM via PolicyNV (NV==G0 is
# now false). The harness confirms the provider stays sealed; the refusal originates
# at Esys_PolicyNV (before Esys_Unseal + before any software gen-compare).
cp -f "$WORK/blob.g0" "$BLOB" || fail "restore g0"
run "j: stale-G0-refused-by-TPM" unseal-refused "$SECRET"

# (l) second reseal G1 -> G2; each PRIOR generation's blob is refused; NV monotonic.
cp -f "$WORK/blob.g1" "$BLOB" || fail "restore g1 for reseal"
run "l: reseal-to-G2" reseal "$KEK2" "$SECRET"
G2="$(nv_gen "$SECRET")"
[ -n "$G2" ] || fail "l: could not read NV generation after 2nd reseal"
[ "$G2" -gt "$G1" ] || fail "l: NV not monotonic (G2=$G2 !> G1=$G1)"
log "PASS (l: NV monotonic): G0=$G0 < G1=$G1 < G2=$G2"
run "l: G2-blob-unseals-KEK2" unseal-ok "$KEK2" "$SECRET"
cp -f "$BLOB" "$WORK/blob.g2" || fail "snapshot g2"
# Both prior blobs are now stale -> refused.
cp -f "$WORK/blob.g1" "$BLOB" || fail "restore g1"
run "l: stale-G1-refused-by-TPM" unseal-refused "$SECRET"
cp -f "$WORK/blob.g0" "$BLOB" || fail "restore g0"
run "l: stale-G0-refused-by-TPM" unseal-refused "$SECRET"

# (m) a reseal under the WRONG secret cannot bump the NV counter (AUTHWRITE is gated
# by the secret-derived NV authValue); the counter must NOT advance.
cp -f "$WORK/blob.g2" "$BLOB" || fail "restore g2"
run "m: wrong-secret-reseal-refused" reseal-fail "$KEK0" "$WRONG_SECRET"
G_AFTER="$(nv_gen "$SECRET")"
[ "$G_AFTER" = "$G2" ] || fail "m: NV advanced on a wrong-secret reseal (G2=$G2 -> $G_AFTER)"
log "PASS (m: wrong-secret cannot bump NV): counter stayed at G2=$G2"

# (k) a v1 (tpm2a, generation-less) blob is REFUSED (must re-provision to v2).
run "k: craft-v1-blob" craft-v1-blob
run "k: v1-blob-refused" v1-refused "$SECRET"

log "ALL PASS — TPM-ENFORCED anti-rollback validated on swtpm (TCTI '$CHOSEN_TCTI', NV $AIMEE_VAULT_TPM2_NV_INDEX)"
exit 0
