#!/usr/bin/env bash
# Deterministic two-session gate for the P5-B2b preflight-to-begin window.
# Session A remains connected after each successful preflight.  The shell barriers
# let session B consume the first grant and let PostgreSQL's clock expire the
# second grant before A attempts begin_initial.
set -euo pipefail

db=${1:-${AIMEE_TEST_PG_URL:-}}
if [ -z "$db" ]; then
  echo "usage: p5b2b-preflight-race-concurrency.sh postgres-url --throwaway" >&2
  exit 1
fi
if [ "${2:-}" != '--throwaway' ]; then
  echo "p5b2b-preflight-race: requires --throwaway; lineage rows are intentionally retained" >&2
  exit 1
fi

tmp=$(mktemp -d)
a_pid=
cleanup() {
  if [ -n "$a_pid" ] && kill -0 "$a_pid" 2>/dev/null; then
    kill "$a_pid" 2>/dev/null || true
    wait "$a_pid" 2>/dev/null || true
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT

wait_marker() {
  local marker=$1 label=$2
  for _ in $(seq 1 200); do
    [ -e "$marker" ] && return 0
    if [ -n "$a_pid" ] && ! kill -0 "$a_pid" 2>/dev/null; then
      echo "p5b2b-preflight-race: session A exited before $label" >&2
      sed -n '1,240p' "$tmp/a.log" >&2 || true
      return 1
    fi
    sleep 0.05
  done
  echo "p5b2b-preflight-race: timeout waiting for $label" >&2
  return 1
}

hex16() { openssl rand -hex 16; }
hex32() { openssl rand -hex 32; }

race_install=$(hex16)
expiry_install=$(hex16)
authority_a=$(hex16)
authority_b=$(hex16)
authority_expiry=$(hex16)
op_a=$(hex32)
op_b=$(hex32)
op_expiry=$(hex32)
issuer='spiffe://p5b2b-race.test'
race_subject="race-$race_install"
expiry_subject="expiry-$expiry_install"
race_proof=$(hex32)
race_custody=$(hex32)
expiry_proof=$(hex32)
expiry_custody=$(hex32)
ca_fp=$(hex32)
race_csr=$(hex32)
race_spki=$(hex32)
expiry_csr=$(hex32)
expiry_spki=$(hex32)

team=$(psql -v ON_ERROR_STOP=1 -Atq "$db" -v name="p5b2b-race-$race_install" <<'SQL'
INSERT INTO public.kb_team(name) VALUES(:'name') RETURNING id;
SQL
)
race_binding=$(psql -v ON_ERROR_STOP=1 -Atq "$db" \
  -v issuer="$issuer" -v subject="$race_subject" -v proof="$race_proof" \
  -v custody="$race_custody" <<'SQL'
SELECT public.kb_management_instance_binding_digest(:'issuer',:'subject',:'proof',:'custody');
SQL
)
expiry_binding=$(psql -v ON_ERROR_STOP=1 -Atq "$db" \
  -v issuer="$issuer" -v subject="$expiry_subject" -v proof="$expiry_proof" \
  -v custody="$expiry_custody" <<'SQL'
SELECT public.kb_management_instance_binding_digest(:'issuer',:'subject',:'proof',:'custody');
SQL
)

psql -v ON_ERROR_STOP=1 -q "$db" \
  -v install="$race_install" -v team="$team" -v issuer="$issuer" \
  -v subject="$race_subject" -v proof="$race_proof" -v custody="$race_custody" \
  -v binding="$race_binding" -v ca_fp="$ca_fp" <<'SQL'
INSERT INTO public.kb_management_instance_grant(
  installation_id,replacement_lineage_id,team_id,workload_issuer,workload_subject,
  proof_anchor,custody_anchor,binding_digest,expected_ca_issuer,
  expected_ca_fingerprint,creator_identity)
VALUES(:'install',:'install',:'team'::BIGINT,:'issuer',:'subject',:'proof',:'custody',
  :'binding','/CN=p5b2b-race-ca',:'ca_fp','owner:p5b2b-race');
SQL

# The pending grant's lineage is immutable under kb_mi_grant_update_guard, so a
# direct lineage rewrite cannot create the mismatch race.  The ordinary tracked
# P5-B2b SQL gate separately covers an incorrect expected_lineage_id at begin.
if psql -v ON_ERROR_STOP=1 -v VERBOSITY=verbose -q "$db" -v install="$race_install" \
    >"$tmp/lineage-mutation.log" 2>&1 <<'SQL'; then
UPDATE public.kb_management_instance_grant SET replacement_lineage_id=repeat('0',32)
 WHERE installation_id=:'install';
SQL
  echo "p5b2b-preflight-race: pending grant lineage mutation was allowed" >&2
  exit 1
fi
if ! grep -q '55000' "$tmp/lineage-mutation.log"; then
  echo "p5b2b-preflight-race: lineage guard returned the wrong SQLSTATE" >&2
  cat "$tmp/lineage-mutation.log" >&2
  exit 1
fi

# Session A performs both preflights and is deliberately held at bounded external
# barriers. timeout(1) bounds the complete connected-session lifetime.
timeout 30s psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/a.log" 2>&1 <<SQL &
SET ROLE aimee_kb_runtime;
SELECT * FROM public.kb_management_instance_grant_preflight(
  '$race_install','$issuer','$race_subject','$race_proof','$race_custody','$race_binding');
\! touch '$tmp/race.ready'
\! while [ ! -e '$tmp/race.release' ]; do sleep 0.05; done
DO \$\$
BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_initial(
      '$op_a','$authority_a','$race_install','$race_install','$issuer','$race_subject',
      '$race_proof','$race_custody','$race_binding','$race_csr','$race_spki');
    RAISE EXCEPTION 'session A consumed an already-consumed grant';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END \$\$;
\! touch '$tmp/race.done'
\! while [ ! -e '$tmp/expiry.seeded' ]; do sleep 0.05; done
SELECT * FROM public.kb_management_instance_grant_preflight(
  '$expiry_install','$issuer','$expiry_subject','$expiry_proof','$expiry_custody',
  '$expiry_binding');
\! touch '$tmp/expiry.ready'
\! while [ ! -e '$tmp/expiry.release' ]; do sleep 0.05; done
DO \$\$
BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_initial(
      '$op_expiry','$authority_expiry','$expiry_install','$expiry_install','$issuer',
      '$expiry_subject','$expiry_proof','$expiry_custody','$expiry_binding',
      '$expiry_csr','$expiry_spki');
    RAISE EXCEPTION 'session A consumed an expired grant';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL;
  END;
END \$\$;
\! touch '$tmp/a.done'
SQL
a_pid=$!

wait_marker "$tmp/race.ready" 'race preflight'

# A distinct real connection consumes the exact pending grant while A is paused.
psql -v ON_ERROR_STOP=1 -q "$db" <<SQL
SET ROLE aimee_kb_runtime;
SELECT * FROM public.kb_management_instance_begin_initial(
  '$op_b','$authority_b','$race_install','$race_install','$issuer','$race_subject',
  '$race_proof','$race_custody','$race_binding','$race_csr','$race_spki');
SQL
touch "$tmp/race.release"
wait_marker "$tmp/race.done" 'session-A conflict result'

# Seed the short-lived grant only after the first race, so A always has time to
# preflight it.  Its immutable expiry is two seconds in the future.
psql -v ON_ERROR_STOP=1 -q "$db" \
  -v install="$expiry_install" -v team="$team" -v issuer="$issuer" \
  -v subject="$expiry_subject" -v proof="$expiry_proof" -v custody="$expiry_custody" \
  -v binding="$expiry_binding" -v ca_fp="$ca_fp" <<'SQL'
INSERT INTO public.kb_management_instance_grant(
  installation_id,replacement_lineage_id,team_id,workload_issuer,workload_subject,
  proof_anchor,custody_anchor,binding_digest,expected_ca_issuer,
  expected_ca_fingerprint,creator_identity,created_at,expires_at)
VALUES(:'install',:'install',:'team'::BIGINT,:'issuer',:'subject',:'proof',:'custody',
  :'binding','/CN=p5b2b-race-ca',:'ca_fp','owner:p5b2b-race',clock_timestamp(),
  clock_timestamp()+interval '2 seconds');
SQL
touch "$tmp/expiry.seeded"
wait_marker "$tmp/expiry.ready" 'expiry preflight'

# Cross expiry according to the authoritative database clock, not host sleep time.
expired=0
for _ in $(seq 1 100); do
  expired=$(psql -v ON_ERROR_STOP=1 -Atq "$db" -v install="$expiry_install" \
    <<'SQL'
SELECT (expires_at<=clock_timestamp())::INT
  FROM public.kb_management_instance_grant WHERE installation_id=:'install';
SQL
  )
  [ "$expired" = 1 ] && break
  sleep 0.05
done
if [ "$expired" != 1 ]; then
  echo "p5b2b-preflight-race: timeout waiting for database expiry" >&2
  exit 1
fi
touch "$tmp/expiry.release"
wait_marker "$tmp/a.done" 'session-A expiry result'
wait "$a_pid"
a_pid=

# Owner-side retained-state assertions prove opA never created or activated an
# issue, opB alone consumed the first grant, and expiry consumed no state.
psql -v ON_ERROR_STOP=1 -q "$db" \
  -v race_install="$race_install" -v expiry_install="$expiry_install" \
  -v op_a="$op_a" -v op_b="$op_b" -v op_expiry="$op_expiry" <<'SQL'
SELECT set_config('p5b2b_race.race_install',:'race_install',false),
       set_config('p5b2b_race.expiry_install',:'expiry_install',false),
       set_config('p5b2b_race.op_a',:'op_a',false),
       set_config('p5b2b_race.op_b',:'op_b',false),
       set_config('p5b2b_race.op_expiry',:'op_expiry',false);
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM public.kb_management_instance_issue
              WHERE operation_id=current_setting('p5b2b_race.op_a')) THEN
    RAISE EXCEPTION 'losing operation A left issue or activation state';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM public.kb_management_instance_issue
                  WHERE operation_id=current_setting('p5b2b_race.op_b') AND
                    installation_id=current_setting('p5b2b_race.race_install')
                    AND state='pending') OR
     NOT EXISTS (SELECT 1 FROM public.kb_management_instance_grant
                  WHERE installation_id=current_setting('p5b2b_race.race_install') AND
                    state='consumed') OR
     NOT EXISTS (SELECT 1 FROM public.kb_management_instance
                  WHERE installation_id=current_setting('p5b2b_race.race_install') AND
                    current_generation=0
                    AND current_enrollment_id IS NULL) THEN
    RAISE EXCEPTION 'winning operation B state mismatch';
  END IF;
  IF EXISTS (SELECT 1 FROM public.kb_management_instance_issue
              WHERE operation_id=current_setting('p5b2b_race.op_expiry')) OR
     EXISTS (SELECT 1 FROM public.kb_management_instance
              WHERE installation_id=current_setting('p5b2b_race.expiry_install')) OR
     NOT EXISTS (SELECT 1 FROM public.kb_management_instance_grant
                  WHERE installation_id=current_setting('p5b2b_race.expiry_install') AND
                    state='pending'
                    AND consumed_at IS NULL AND expires_at<=clock_timestamp()) THEN
    RAISE EXCEPTION 'expired begin consumed instance state';
  END IF;
END $$;
SQL

echo "== P5-B2b preflight race: PASSED (consume conflict + authoritative expiry denial) =="
