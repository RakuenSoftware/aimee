#!/bin/bash
# Live gate for org-vault rotation requests and the KB daemon worker: real
# PostgreSQL, the signed-HWM KMS test helper, a mock vendor. Run as root inside a
# disposable test container with PostgreSQL and pgvector installed.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
db=aimee_rotation_worker_live
work="$repo/src/build/rotation-worker-live"
cleanup() {
  runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1 || true
  [ "$made_role" = 1 ] && runuser -u postgres -- dropuser --if-exists root >/dev/null 2>&1 || true
  [ "$made_provider" = 1 ] && rm -f -- "$provider" || true
  rm -rf -- "$work"
}
trap cleanup EXIT
made_role=0
made_provider=0
provider=/usr/local/libexec/aimee-modules/aimee-module-postgres
rm -rf -- "$work"
mkdir -p "$work"
chmod 0700 "$work"

runuser -u postgres -- dropdb --force --if-exists "$db"
# The fixture connects as the invoking OS user, like the other live gates.
if ! runuser -u postgres -- psql -tAc "SELECT 1 FROM pg_roles WHERE rolname='root'" | grep -q 1; then
  runuser -u postgres -- createuser -s root
  made_role=1
fi
runuser -u postgres -- createdb -E UTF8 -T template0 --locale=C.UTF-8 "$db"
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" \
  -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;'
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -d "$db" -f src/modules/kb/c/schema_roles.sql
sed 's/__EMBED_DIM__/1024/g' src/modules/kb/c/schema.sql | \
  runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db"
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" -f src/modules/kb/c/schema_grants.sql
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" <<'SQL'
\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES (982370,'rotation_worker_live');
INSERT INTO kb_team_membership(identity_key,team,is_default)
  VALUES ('cert:issuer-rot-worker:01AB',982370,1),('owner',982370,0);
SELECT org_catalog_bedrock_upsert('rot-worker-model','Rotation Worker Model','converse',
  'anthropic','foundation','aws','us-east-1',NULL,ARRAY['us-east-1'],NULL,'',true);
SELECT org_model_entitle('rot-worker-model',982370);
SELECT org_pricing_add_version('bedrock','rot-worker-billable',1.0,2.0,0.5,0.75);
COMMIT;
SQL

openssl genpkey -algorithm Ed25519 -out "$work/hwm-private.pem"
openssl pkey -in "$work/hwm-private.pem" -pubout -outform DER | tail -c 32 >"$work/hwm-public.raw"
head -c 32 /dev/urandom >"$work/kek.raw"
printf '1\n' >"$work/hwm.state"
: >"$work/hwm.lock"
install -m 0700 scripts/p2b_kms_helper.py "$work/kms-helper"
chmod 0600 "$work"/hwm-* "$work/kek.raw" "$work/hwm.state" "$work/hwm.lock"

make -C src -j"$(nproc)" OBJDIR=build/obj-rotation-live build/obj-rotation-live/aimee-module \
  build/obj-rotation-live/tests/unit-test-kb-vault-rotation-worker-live >/dev/null
# A local KB_STORE session runs through the PostgreSQL module provider, as in CI.
if [ ! -e "$provider" ]; then
  install -D -m 0755 src/build/obj-rotation-live/aimee-module "$provider"
  made_provider=1
fi

export AIMEE_TEST_PG_URL="postgres:///$db"
export AIMEE_HOME="$work/aimee-home"
mkdir -p "$AIMEE_HOME"
export AIMEE_VAULT_KMS_HELPER="$work/kms-helper"
export AIMEE_VAULT_KMS_KEY_ID=rot-worker-key
export AIMEE_VAULT_KMS_HWM_PUBKEY="$work/hwm-public.raw"
export AIMEE_VAULT_KMS_HWM_DOMAIN=rotation-worker-live
export AIMEE_P2B_KMS_PRIVATE_KEY="$work/hwm-private.pem"
export AIMEE_P2B_KMS_KEK="$work/kek.raw"
export AIMEE_P2B_KMS_HWM_STATE="$work/hwm.state"
export AIMEE_P2B_KMS_HWM_LOCK="$work/hwm.lock"
src/build/obj-rotation-live/tests/unit-test-kb-vault-rotation-worker-live
printf 'run-vault-rotation-worker-live: all gates passed\n'
