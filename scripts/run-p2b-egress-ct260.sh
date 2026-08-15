#!/bin/bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
db=aimee_p2b_live_gate
work=$(mktemp -d /tmp/aimee-p2b-live.XXXXXX)
cleanup() {
  runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1 || true
  case "$work" in /tmp/aimee-p2b-live.*) rm -rf -- "$work" ;; esac
}
trap cleanup EXIT

runuser -u postgres -- dropdb --force --if-exists "$db"
runuser -u postgres -- createdb "$db"
url="postgres:///$db"
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" \
  -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;'
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" -f src/modules/db2/c/schema_roles.sql
sed 's/__EMBED_DIM__/1024/g' src/modules/db2/c/schema.sql | \
  runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db"
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" -f src/modules/db2/c/schema_grants.sql
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" \
  -f scripts/p2b_egress_pg_test.sql
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" -f scripts/p2b_egress_ct260_seed.sql

# Validate the real swtpm/libtss2 custody barrier in this same CT before the
# signed-HWM KMS composition (TPM2's provider intentionally has no HWM signer).
bash scripts/p7_tpm2_swtpm_test.sh

openssl genpkey -algorithm Ed25519 -out "$work/hwm-private.pem"
openssl pkey -in "$work/hwm-private.pem" -pubout -outform DER | tail -c 32 >"$work/hwm-public.raw"
head -c 32 /dev/urandom >"$work/kek.raw"
printf '1\n' >"$work/hwm.state"
: >"$work/hwm.lock"
install -m 0700 scripts/p2b_kms_helper.py "$work/kms-helper"
chmod 0600 "$work"/*
chmod 0700 "$work/kms-helper"

make -C src -j"$(nproc)" OBJDIR=build/obj-p2b-live WITH_TPM2=1 \
  P2B_INTEGRATION_TEST_OVERRIDE=1 \
  build/obj-p2b-live/tests/unit-test-kb-p2b-egress-live >/dev/null
make -C src -j"$(nproc)" OBJDIR=build/obj-p2b-hardened WITH_TPM2=1 \
  build/obj-p2b-hardened/tests/unit-test-kb-p2b-egress-live >/dev/null
AIMEE_P2B_EXPECT_DISABLED=1 \
  src/build/obj-p2b-hardened/tests/unit-test-kb-p2b-egress-live
if strings src/build/obj-p2b-hardened/tests/unit-test-kb-p2b-egress-live | \
   grep -q 'P2b integration-test override enabled'; then
  printf 'hardened P2b artifact contains override warning\n' >&2
  exit 1
fi

export AIMEE_TEST_PG_URL="$url"
export AIMEE_HOME="$work/aimee-home"
mkdir -p "$AIMEE_HOME"
export AIMEE_VAULT_KMS_HELPER="$work/kms-helper"
export AIMEE_VAULT_KMS_KEY_ID=p2b-live-key
export AIMEE_VAULT_KMS_HWM_PUBKEY="$work/hwm-public.raw"
export AIMEE_VAULT_KMS_HWM_DOMAIN=p2b-ct260-live
export AIMEE_P2B_KMS_PRIVATE_KEY="$work/hwm-private.pem"
export AIMEE_P2B_KMS_KEK="$work/kek.raw"
export AIMEE_P2B_KMS_HWM_STATE="$work/hwm.state"
export AIMEE_P2B_KMS_HWM_LOCK="$work/hwm.lock"
export AIMEE_P2B_LIVE_TARGET="$repo/src/build/obj-p2b-live/tests/unit-test-kb-p2b-egress-live"

bash scripts/run-p6c-egress-ct260.sh
printf 'run-p2b-egress-ct260: all gates passed\n'
