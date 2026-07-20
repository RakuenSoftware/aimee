#!/usr/bin/env bash
# Real multi-connection fencing gate: one lease winner, expiry/steal advances the
# token, and the stale winner cannot checkpoint an external result afterward.
set -euo pipefail

DB_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$DB_URL" ]; then
  echo "p7_rotation_ops_concurrency: Postgres URL required" >&2
  exit 1
fi

TEAM=970713
N=12
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

rid=$(psql -v ON_ERROR_STOP=1 -tA "$DB_URL" <<SQL | tail -1
BEGIN;
SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES ($TEAM,'p7_ops_concurrency');
SELECT org_vault_put('team:$TEAM:provider:race',$TEAM,'race','primary',1,
  '\x01','\x02','\x03','\x04');
SELECT org_vault_rotation_start('owner','team:$TEAM|race|primary',
  'team:$TEAM:provider:race',$TEAM,'race','primary',1,false) AS rid \gset
COMMIT;
SELECT :rid;
SQL
)

pids=()
for i in $(seq 1 "$N"); do
  (
    psql -v ON_ERROR_STOP=1 -tA "$DB_URL" >"$WORK/out-$i" 2>"$WORK/err-$i" <<SQL || echo ERR >"$WORK/out-$i"
BEGIN;
SELECT set_config('aimee.principal','owner',true);
SELECT org_vault_rotation_claim('owner',$rid,'provision','worker-$i',30);
COMMIT;
SQL
  ) &
  pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid" || true; done

winner_count=0
winner_owner=
winner_token=
for i in $(seq 1 "$N"); do
  token=$(grep -E '^[0-9]+$' "$WORK/out-$i" | tail -1 || true)
  if [ -n "$token" ]; then
    winner_count=$((winner_count + 1))
    winner_owner="worker-$i"
    winner_token="$token"
  fi
done
if [ "$winner_count" -ne 1 ]; then
  echo "P7 OPS CONCURRENCY FAIL: winners=$winner_count, wanted 1" >&2
  exit 1
fi

resume_token=$(psql -v ON_ERROR_STOP=1 -tA "$DB_URL" <<SQL | tail -1
BEGIN;
SELECT set_config('aimee.principal','owner',true);
UPDATE org_vault_rotation SET claim_until=clock_timestamp()-interval '1 second' WHERE id=$rid;
SELECT org_vault_rotation_claim('owner',$rid,'provision','worker-resume',30) AS token \gset
COMMIT;
SELECT :token;
SQL
)
if [ "$resume_token" -le "$winner_token" ]; then
  echo "P7 OPS CONCURRENCY FAIL: fencing token did not advance" >&2
  exit 1
fi

if psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null 2>&1 <<SQL
BEGIN;
SELECT set_config('aimee.principal','owner',true);
SELECT org_vault_rotation_checkpoint_old_ref(
  'owner',$rid,'$winner_owner',$winner_token,'stale-result');
COMMIT;
SQL
then
  echo "P7 OPS CONCURRENCY FAIL: stale winner checkpointed" >&2
  exit 1
fi

old_ref=$(psql -v ON_ERROR_STOP=1 -tA "$DB_URL" \
  -c "SELECT old_vendor_ref FROM org_vault_rotation WHERE id=$rid")
if [ -n "$old_ref" ]; then
  echo "P7 OPS CONCURRENCY FAIL: stale result changed durable ref" >&2
  exit 1
fi

echo "== P7 ops concurrency gate: PASSED (one of $N won; token $winner_token -> $resume_token; stale fenced) =="
