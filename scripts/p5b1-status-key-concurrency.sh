#!/usr/bin/env bash
set -euo pipefail

db=${1:?usage: p5b1-status-key-concurrency.sh postgres-url}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

psql -v ON_ERROR_STOP=1 -q "$db" <<'SQL'
INSERT INTO kb_team(id,name) VALUES(975101,'p5b1-concurrency');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES
 ('cert:issuer:01',975101,1),('cert:rev-issuer:02',975101,0);
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,expires_at,revoked_at,legacy,
 cert_issuer,cert_serial_norm,authority_id) VALUES
 ('p5-kb-management',repeat('a',64),'01','active','2099-01-01T00:00:00Z','',0,
  'issuer','01',repeat('a',32)),
 ('p5-kb-management',repeat('c',64),'02','active','2099-01-01T00:00:00Z','',0,
  'rev-issuer','02',repeat('c',32)),
 ('p5-server-management',repeat('b',64),'03','active','2099-01-01T00:00:00Z','',0,
  'target-issuer','03',repeat('b',32));
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
 mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
VALUES('status-target','client-cn','mgmt-cn',975101,'https://127.0.0.1:1','active',
 'target-issuer','03',repeat('b',64));
INSERT INTO org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,ciphertext,
 tag,hwm_attestation) VALUES('org:p5-status',NULL,'management','ed25519',2,
 decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),decode(repeat('13',32),'hex'),
 decode(repeat('14',16),'hex'),'\xaabbcc'::bytea);
INSERT INTO org_vault_current(principal,agent,cred,version)
 VALUES('org:p5-status','management','ed25519',2);
INSERT INTO org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,to_version,state)
 VALUES('platform:p5-status','org:p5-status',NULL,'management','ed25519',1,2,'activated');
INSERT INTO kb_management_status_key(singleton,bootstrap_id,custody_key_id,wire_key_id,public_key,enabled)
 VALUES(1,repeat('9',64),'platform:p5-status','status-1',decode(repeat('44',32),'hex'),true);
SQL

generation() { psql -Atq "$db" -c "SELECT generation FROM kb_cert_revocation_generation WHERE singleton=1"; }
admit_sql() {
  local use=$1 issuer=$2 serial=$3 fp=$4 gen=$5
  printf "SET ROLE aimee_kb_status; SELECT * FROM kb_management_status_key_admit('%s','platform:p5-status','status-1',2,repeat('d',64),'%s','%s','%s','status-target',repeat('b',64),%s,'\\xaabbcc'::bytea);" "$use" "$issuer" "$serial" "$fp" "$gen"
}
wait_ready() {
  local f=$1
  for _ in $(seq 1 100); do grep -q READY "$f" 2>/dev/null && return; sleep 0.02; done
  echo "P5-B1 concurrency blocker did not become ready" >&2; exit 1
}
must_fail() {
  local label=$1 sql=$2
  if psql -v ON_ERROR_STOP=1 -q "$db" -c "$sql" >"$tmp/$label.out" 2>&1; then
    echo "P5-B1 concurrency FAIL: $label admitted" >&2; exit 1
  fi
}

# Caller revocation commits while admission waits on the exact enrollment row.
gen=$(generation)
psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/revoke.block" 2>&1 <<'SQL' &
BEGIN;
SELECT 1 FROM kb_enrollments WHERE scope='p5-kb-management' AND cert_issuer='rev-issuer'
  AND cert_serial_norm='02' FOR UPDATE;
\echo READY
SELECT pg_sleep(0.5);
UPDATE kb_enrollments SET state='revoked',revoked_at=clock_timestamp()::text
 WHERE scope='p5-kb-management' AND cert_issuer='rev-issuer' AND cert_serial_norm='02';
COMMIT;
SQL
pid=$!; wait_ready "$tmp/revoke.block"
must_fail revoke "$(admit_sql "$(printf '2%.0s' {1..64})" rev-issuer 02 "$(printf 'c%.0s' {1..64})" "$gen")"
wait "$pid"

# Fixed registry disable commits while admission waits on the registry row lock.
gen=$(generation)
psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/disable.block" 2>&1 <<'SQL' &
BEGIN; SELECT 1 FROM kb_management_status_key WHERE singleton=1 FOR UPDATE;
\echo READY
SELECT pg_sleep(0.5); UPDATE kb_management_status_key SET enabled=false WHERE singleton=1; COMMIT;
SQL
pid=$!; wait_ready "$tmp/disable.block"
must_fail disable "$(admit_sql "$(printf '3%.0s' {1..64})" issuer 01 "$(printf 'a%.0s' {1..64})" "$gen")"
wait "$pid"
psql -q "$db" -c "UPDATE kb_management_status_key SET enabled=true WHERE singleton=1"

# Rotation transition commits under the P7 slot advisory lock before admission.
gen=$(generation)
psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/rotation.block" 2>&1 <<'SQL' &
BEGIN; SELECT pg_advisory_xact_lock(hashtext('orgvault:org:p5-status|management|ed25519'));
\echo READY
SELECT pg_sleep(0.5); UPDATE org_vault_rotation SET state='revoked'
 WHERE key_id='platform:p5-status'; COMMIT;
SQL
pid=$!; wait_ready "$tmp/rotation.block"
must_fail rotation "$(admit_sql "$(printf '4%.0s' {1..64})" issuer 01 "$(printf 'a%.0s' {1..64})" "$gen")"
wait "$pid"
psql -q "$db" -c "UPDATE org_vault_rotation SET state='activated' WHERE key_id='platform:p5-status'"

# Candidate must wait behind the primary exclusive barrier and then observe sealed.
psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/seal.block" 2>&1 <<'SQL' &
BEGIN; SELECT org_vault_control_lock_exclusive();
\echo READY
SELECT pg_sleep(0.5); UPDATE kb_vault_control SET sealed=true WHERE singleton=1; COMMIT;
SQL
pid=$!; wait_ready "$tmp/seal.block"
must_fail seal "SET ROLE aimee_kb_status; SELECT * FROM kb_management_status_key_candidate('platform:p5-status','status-1',2)"
wait "$pid"
psql -q "$db" -c "UPDATE kb_vault_control SET sealed=false,seal_epoch=seal_epoch+1 WHERE singleton=1"

count=$(psql -Atq "$db" -c "SELECT count(*) FROM kb_management_status_key_use_intent")
if [ "$count" -ne 0 ]; then
  echo "P5-B1 concurrency FAIL: rejected races left $count intents" >&2; exit 1
fi
psql -v ON_ERROR_STOP=1 -q "$db" <<'SQL'
DELETE FROM kb_management_status_key WHERE singleton=1;
DELETE FROM org_vault_rotation WHERE key_id='platform:p5-status';
DELETE FROM org_vault_current WHERE principal='org:p5-status';
DELETE FROM org_vault_secret WHERE principal='org:p5-status';
DELETE FROM kb_server_registry WHERE server_id='status-target';
DELETE FROM kb_enrollments WHERE cert_issuer IN ('issuer','rev-issuer','target-issuer');
DELETE FROM kb_team_membership WHERE team=975101;
DELETE FROM kb_team WHERE id=975101;
SQL
echo "== P5-B1 concurrency: PASSED (revoke/disable/rotation/seal linearized) =="
