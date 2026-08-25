#!/usr/bin/env bash
# Run only against a throwaway PostgreSQL 17 database after schema + grants.
set -euo pipefail

if [[ $# -ne 1 || -z $1 ]]; then
  echo "usage: $0 postgres-url" >&2
  exit 2
fi

p5c1c_url=$1
p5c1c_tmp=$(mktemp -d)
trap 'rm -rf -- "$p5c1c_tmp"' EXIT

psql -X -v ON_ERROR_STOP=1 "$p5c1c_url" <<'SQL' >/dev/null
INSERT INTO public.kb_team(id,name) VALUES(9761,'p5c1c-concurrency');
INSERT INTO public.kb_team_membership(identity_key,team,is_default)
  VALUES('oidc:https%3A%25issuer:concurrent',9761,1);
INSERT INTO public.kb_team_lead(identity_key,team,granted_by)
  VALUES('oidc:https%3A%25issuer:concurrent',9761,'owner');
INSERT INTO public.kb_enrollments(id,scope,fingerprint,serial,state,expires_at,revoked_at,
  authority_id,cert_issuer,cert_serial_norm) VALUES
  (97611,'p5-kb-management',repeat('1',64),'11','active',
    to_char(now()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),'',repeat('1',32),'issuer-local','11'),
  (97612,'p5-server-management',repeat('2',64),'22','active',
    to_char(now()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),'',repeat('2',32),'issuer-target','22');
INSERT INTO public.kb_server_registry(server_id,cert_cn,mgmt_cert_cn,owner_issuer,
  owner_subject,team_id,endpoint,status,mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
VALUES('srv-concurrent','server:srv-concurrent:client','server:srv-concurrent:management',
  'test','owner',9761,'https://192.0.2.2:7443','active','issuer-target','22',repeat('2',64));
INSERT INTO public.kb_management_instance_grant(installation_id,replacement_lineage_id,
  team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,creator_identity,state,consumed_at)
VALUES(repeat('c',32),repeat('c',32),9761,'spiffe://p5c1c.concurrent','kb-node',
  repeat('4',64),repeat('5',64),repeat('6',64),'issuer-local',repeat('7',64),
  'owner','consumed',now());
INSERT INTO public.kb_management_instance(installation_id,replacement_lineage_id,authority_id,
  team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,current_generation,current_enrollment_id,state)
VALUES(repeat('c',32),repeat('c',32),repeat('1',32),9761,'spiffe://p5c1c.concurrent',
  'kb-node',repeat('4',64),repeat('5',64),repeat('6',64),'issuer-local',repeat('7',64),
  1,97611,'active');
INSERT INTO public.kb_management_instance_issue(operation_id,installation_id,issue_kind,
  generation,csr_digest,csr_spki_digest,public_bundle_digest,cert_issuer,cert_serial_norm,
  cert_fingerprint,cert_spki_digest,cert_not_before,cert_not_after,enrollment_id,state,
  created_at,pending_expires_at,activated_at)
VALUES(repeat('8',64),repeat('c',32),'initial',1,repeat('9',64),repeat('a',64),
  repeat('b',64),'issuer-local','11',repeat('1',64),repeat('a',64),now()-interval '1 minute',
  now()+interval '1 hour',97611,'active',now()-interval '2 minutes',
  now()-interval '1 minute',now()-interval '30 seconds');
INSERT INTO public.kb_management_token_intent_namespace(correlation_id,jti,kind)
VALUES(repeat('f',64),repeat('e',64),'action');
INSERT INTO public.kb_management_action_intent(correlation_id,jti,kind,team_id,actor_identity,
  capability,target_server_id,request_sha256,token_issuer,audience,kid,issued_at,expires_at,
  installation_id,installation_generation,installation_enrollment_id,local_cert_issuer,
  local_cert_serial_norm,local_cert_fingerprint,target_enrollment_id,target_mgmt_issuer,
  target_mgmt_serial_norm,target_mgmt_fingerprint,revocation_generation)
VALUES(repeat('f',64),repeat('e',64),'action',9761,
  'oidc:https%3A%25issuer:concurrent','remote_writes',
  'srv-concurrent',repeat('d',64),'issuer','srv-concurrent','kid-1',1000,1090,repeat('c',32),
  1,97611,'issuer-local','11',repeat('1',64),97612,'issuer-target','22',repeat('2',64),1);
SQL

# The admission advisory barrier must converge identical first-use callers just
# like the outcome barrier: one creates the immutable intent/WORM pair and the
# other 31 observe exact replay.
for p5c1c_i in $(seq 1 32); do
  (
    psql -X -At -v ON_ERROR_STOP=1 "$p5c1c_url" <<'SQL'
BEGIN;
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:concurrent',9761);
SELECT replayed FROM public.kb_management_action_intent_start(
  repeat('a',64),repeat('b',64),9761,'srv-concurrent','remote_writes',repeat('7',64),
  'https://kb.concurrent.test','kid-1',60,repeat('c',32));
COMMIT;
SQL
  ) >"$p5c1c_tmp/start-$p5c1c_i.out" 2>"$p5c1c_tmp/start-$p5c1c_i.err" &
done
wait

cat "$p5c1c_tmp"/start-*.err >&2
p5c1c_start_false=$(cat "$p5c1c_tmp"/start-*.out | grep -xc 'f' || true)
p5c1c_start_true=$(cat "$p5c1c_tmp"/start-*.out | grep -xc 't' || true)
if [[ $p5c1c_start_false -ne 1 || $p5c1c_start_true -ne 31 ]]; then
  echo "concurrent start mismatch: fresh=$p5c1c_start_false replay=$p5c1c_start_true" >&2
  exit 1
fi
p5c1c_start_counts=$(psql -X -At -v ON_ERROR_STOP=1 "$p5c1c_url" <<'SQL'
SELECT (SELECT count(*) FROM public.kb_management_action_intent
          WHERE correlation_id=repeat('a',64))::TEXT||':'||
       (SELECT count(*) FROM public.kb_audit_outbox
          WHERE action='management.action.intent' AND subject=repeat('a',64))::TEXT;
SQL
)
if [[ $p5c1c_start_counts != 1:1 ]]; then
  echo "concurrent start structured/WORM count mismatch: $p5c1c_start_counts" >&2
  exit 1
fi

for p5c1c_i in $(seq 1 32); do
  (
    psql -X -At -v ON_ERROR_STOP=1 "$p5c1c_url" <<'SQL'
BEGIN;
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:concurrent',9761);
SELECT replayed FROM public.kb_management_action_outcome_append(
  repeat('f',64),'indeterminate','transport_ambiguous',NULL,NULL);
COMMIT;
SQL
  ) >"$p5c1c_tmp/outcome-$p5c1c_i.out" 2>"$p5c1c_tmp/outcome-$p5c1c_i.err" &
done
wait

cat "$p5c1c_tmp"/outcome-*.err >&2
p5c1c_false=$(cat "$p5c1c_tmp"/outcome-*.out | grep -xc 'f' || true)
p5c1c_true=$(cat "$p5c1c_tmp"/outcome-*.out | grep -xc 't' || true)
if [[ $p5c1c_false -ne 1 || $p5c1c_true -ne 31 ]]; then
  echo "concurrent replay mismatch: fresh=$p5c1c_false replay=$p5c1c_true" >&2
  exit 1
fi

p5c1c_counts=$(psql -X -At -v ON_ERROR_STOP=1 "$p5c1c_url" <<'SQL'
SELECT (SELECT count(*) FROM public.kb_management_action_outcome
          WHERE correlation_id=repeat('f',64))::TEXT||':'||
       (SELECT count(*) FROM public.kb_audit_outbox
          WHERE action='management.action.outcome' AND subject=repeat('f',64))::TEXT;
SQL
)
if [[ $p5c1c_counts != 1:1 ]]; then
  echo "concurrent structured/WORM count mismatch: $p5c1c_counts" >&2
  exit 1
fi

echo "p5c1c_management_action_concurrency_pg17: ok"
