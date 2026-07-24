#!/usr/bin/env bash
# p7_pkcs11_softhsm_test.sh: integration gate for the PKCS#11 vault custody
# provider, exercised against SoftHSM2 (a software PKCS#11 token that implements
# the same PKCS#11 API a real HSM does, so a green run here validates the
# provider's C_Login / C_FindObjects / C_GetAttributeValue path end to end).
#
# The PKCS#11 provider is the only build (no stub), but get_kek/unseal need a
# real token, so this is a standalone gate rather than a default unit test —
# the same treatment as the tpm2 custody provider's swtpm gate.
#
# It SKIPS CLEANLY (exit 0) when the token toolchain is unavailable:
#   - softhsm2-util not installed, or
#   - pkcs11-tool (opensc) not installed, or
#   - the SoftHSM2 module .so cannot be located.
#
# On a capable host it: creates an isolated SoftHSM2 token, writes a known
# 32-byte KEK as a CKO_DATA object labelled `aimee-kek` (the provider looks up
# by label and reads CKA_VALUE, which a data object always exposes), points the
# provider env at that token, builds the harness, and runs the fixed assertions
# in tests/test_vault_custody_pkcs11.c (unseal -> unsealed, get_kek nonzero,
# seal -> sealed).
set -euo pipefail

# In CI the token toolchain is provisioned deliberately, so a "skip" would mean
# the provisioning is broken and the test silently did not run. Set
# AIMEE_PKCS11_GATE_REQUIRE=1 there to turn every skip into a hard failure, so a
# missing dependency cannot masquerade as a pass. Unset (the default) keeps the
# gate skip-clean for developer/appliance hosts without SoftHSM2.
log()  { printf '%s\n' "p7-pkcs11-softhsm: $*"; }
skip() {
  if [ "${AIMEE_PKCS11_GATE_REQUIRE:-0}" = "1" ]; then
    log "FAIL (require mode): $*"
    exit 1
  fi
  log "SKIP: $*"
  exit 0
}

command -v softhsm2-util >/dev/null 2>&1 || skip "softhsm2-util not installed"
command -v pkcs11-tool   >/dev/null 2>&1 || skip "pkcs11-tool (opensc) not installed"

# Locate the SoftHSM2 PKCS#11 module.
MODULE=""
for cand in \
  /usr/lib/softhsm/libsofthsm2.so \
  /usr/lib/x86_64-linux-gnu/softhsm/libsofthsm2.so \
  /usr/local/lib/softhsm/libsofthsm2.so; do
  [ -f "$cand" ] && { MODULE="$cand"; break; }
done
[ -n "$MODULE" ] || skip "libsofthsm2.so not found"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HARNESS_REL="build/obj/tests/p7-pkcs11-harness"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# Isolated token store, so the gate never touches a host token database.
export SOFTHSM2_CONF="$WORK/softhsm2.conf"
mkdir -p "$WORK/tokens"
printf 'directories.tokendir = %s\n' "$WORK/tokens" > "$SOFTHSM2_CONF"

PIN="1234"
SO_PIN="5678"
LABEL="aimee-kek"

log "initialising SoftHSM2 token"
softhsm2-util --init-token --free --label aimee-vault --pin "$PIN" --so-pin "$SO_PIN" >/dev/null

# Known 32-byte KEK, written as a data object the provider reads by label.
head -c 32 /dev/urandom > "$WORK/kek.bin"
log "writing $LABEL data object"
pkcs11-tool --module "$MODULE" --login --pin "$PIN" \
  --write-object "$WORK/kek.bin" --type data --label "$LABEL" >/dev/null

log "building harness"
make -C "$REPO_ROOT/src" "$HARNESS_REL" >/dev/null

log "running provider assertions"
AIMEE_VAULT_PKCS11_MODULE="$MODULE" \
AIMEE_VAULT_PKCS11_PIN="$PIN" \
AIMEE_VAULT_PKCS11_LABEL="$LABEL" \
  "$REPO_ROOT/src/$HARNESS_REL"

log "PASS"
