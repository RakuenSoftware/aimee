#!/usr/bin/env bash
# run-p1-rls-gate.sh: the mandatory P1 DB-layer tenancy-isolation gate.
#
# Provisions a throwaway database on the target Postgres with the three-role split
# (schema_roles.sql) + the full aimee schema (schema.sql), then runs the RLS
# isolation assertions (p1_rls_isolation_test.sql). Any failure exits non-zero, so
# this is a HARD CI gate — it does not skip. Requires a real Postgres with the
# pgvector + pg_trgm extensions (the SQLite shim cannot enforce RLS).
#
# Connection: pass a libpq base URL as $1, or set PG* env. The connecting role must
# be a superuser (needs CREATE DATABASE / EXTENSION / SET ROLE) — CI's pgvector
# sidecar 'aimee' user is one. Example:
#   scripts/run-p1-rls-gate.sh postgres://aimee:aimee@localhost:5432/postgres
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "run-p1-rls-gate: no Postgres URL (arg1 or AIMEE_TEST_PG_URL). This gate does not skip." >&2
  exit 1
fi

# Admin URL points at the maintenance db; derive one on the same server.
ADMIN_URL="${BASE_URL%/*}/postgres"
TESTDB="aimee_p1_rls_gate"
SCHEMA_ONLY_DB="aimee_schema_only_gate"

echo "== Schema-only developer load: provisioning $SCHEMA_ONLY_DB =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $SCHEMA_ONLY_DB;"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $SCHEMA_ONLY_DB;"
SCHEMA_ONLY_URL="${BASE_URL%/*}/$SCHEMA_ONLY_DB"
psql -v ON_ERROR_STOP=1 "$SCHEMA_ONLY_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;"
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$SCHEMA_ONLY_URL" -f - >/dev/null
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE $SCHEMA_ONLY_DB;"
echo "== Schema-only developer load: PASSED =="

echo "== P1 RLS gate: provisioning $TESTDB =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB;"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $TESTDB;"
DB_URL="${BASE_URL%/*}/$TESTDB"

psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;"

# Three-phase provisioning, matching the real hardened deploy order:
#   1. roles (create) -> 2. schema (DDL) -> 3. grants (runtime DML/EXECUTE).
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/modules/db2/c/schema_roles.sql"
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f -
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/modules/db2/c/schema_grants.sql"

echo "== P1 RLS gate: running isolation assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p1_rls_isolation_test.sql"

echo "== Memory row-scope, tombstone, and WORM assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/memory-governance-pg-test.sql"

echo "== Per-user write-tier grant isolation assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/per-user-write-tier-rls-test.sql"

echo "== Per-user identity token authority assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/per-user-identity-authority-pg17-test.sql"

# The subject grammar, against the corpus shared with the two C validators. Its
# own step because it reuses the fixture the file above builds, and because a
# failure here means "three copies of one rule have drifted", not "the authority
# is broken".
echo "== Subject grammar corpus (generated from src/tests/subject_corpus.h) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/gen/subject-corpus.sql"

echo "== P5-B status authority + revocation generation assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5b_status_pg17_test.sql"

echo "== P5-C3 action-checkpoint primary admission assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5c3-action-checkpoint-pg17-test.sql"

echo "== P5-B2b management-instance lineage assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5b2b_management_instance_pg_test.sql"

echo "== P5-B1 fixed status-key authority assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5b1-status-key-pg17-test.sql"

echo "== P5-B1b owner-only status-key bootstrap assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5b1b-status-bootstrap-pg17-test.sql"

echo "== P5-B1 status-key revoke/rotation/disable/seal concurrency =="
"$ROOT/scripts/p5b1-status-key-concurrency.sh" "$DB_URL"

echo "== P3a cost-attribution isolation assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p3a_rls_isolation_test.sql"

echo "== P10 kb-vault isolation assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p10_vault_rls_test.sql"

echo "== P7 anchor-authoritative rotation persistence + isolation assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_rotation_pg_test.sql"

echo "== P7 fenced vendor-operation workflow + recovery assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_rotation_ops_pg_test.sql"

echo "== P7 signed-HWM steady-state key-use admission assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_key_use_pg_test.sql"

echo "== P7 signed-HWM same-use concurrency gate =="
"$ROOT/scripts/p7_key_use_concurrency.sh" "$DB_URL"

echo "== P7 fenced vendor-operation multi-worker concurrency gate =="
"$ROOT/scripts/p7_rotation_ops_concurrency.sh" "$DB_URL"

echo "== P7 primary barrier grant-reapplication gate =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/modules/db2/c/schema_grants.sql"

echo "== P7 primary vault maintenance barrier assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_vault_barrier_pg_test.sql"

echo "== P7 primary vault maintenance barrier concurrency gate =="
"$ROOT/scripts/p7_vault_barrier_concurrency.sh" "$DB_URL"

echo "== P7 whole-vault re-wrap concurrency and failure assertions =="
"$ROOT/scripts/p7_vault_rewrap_concurrency.sh" "$DB_URL"

echo "== P7 whole-vault re-wrap staging and promotion assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_vault_rewrap_pg_test.sql"

echo "== P7-witness-e1 evidence store: C<->SQL digest parity, append, WORM, ACLs =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_witness_pg_test.sql"

# The cadence/boot/gate run as aimee_kb_runtime on the hardened tier; this DB has the
# three-role split + schema_grants, so exercise the full witness surface AS that role
# (every op must succeed; every forge/control path must stay denied). This is the
# gate that catches a missing runtime grant — the class of bug the owner-only tests
# never could.
echo "== P7-witness runtime-role least-privilege gate (as aimee_kb_runtime) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_witness_runtime_role_pg_test.sql"

echo "== P7-witness-e2 wiring gate (isolated DB: audit + reseal + open ledgers) =="
"$ROOT/scripts/run-p7-witness-wiring.sh" "$BASE_URL"

echo "== P2a org-model catalog + entitlement isolation assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p2a_catalog_rls_test.sql"

echo "== P6c bedrock catalog routing + fail-closed adapter-registry validation (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p6c_bedrock_catalog_test.sql"

echo "== P3b org spend-reporting authorization assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p3b_spend_rls_test.sql"

echo "== P4a budget reservation core correctness + isolation assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p4_budget_rls_test.sql"

echo "== P4a budget over-commit concurrency gate (genuinely parallel connections) =="
"$ROOT/scripts/p4_budget_concurrency.sh" "$DB_URL"

echo "== P4b keyed fixed-window rate limiter correctness + isolation assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p4b_rate_rls_test.sql"

echo "== P4b rate shared-window-not-N× concurrency gate (genuinely parallel connections) =="
"$ROOT/scripts/p4b_rate_concurrency.sh" "$DB_URL"

echo "== P9a telemetry export + content-free ingest correctness + isolation assertions (same provisioned db) =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p9_telemetry_rls_test.sql"

echo "== P5-C2b signed JWKS publication authority assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5c2b-jwks-publication-pg17-test.sql"

echo "== P5-C2c authenticated JWKS fetch authority assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p5c2c-jwks-fetch-pg17-test.sql"

echo "== P1 RLS gate: cleanup =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB;"
echo "== P1 RLS gate: PASSED =="
