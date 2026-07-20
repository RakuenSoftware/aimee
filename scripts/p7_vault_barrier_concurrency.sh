#!/usr/bin/env bash
set -euo pipefail

db=${1:?usage: p7_vault_barrier_concurrency.sh postgres-url}
export PGOPTIONS='-c statement_timeout=15s -c lock_timeout=12s'
tmp=$(mktemp -d)
fifo="$tmp/admitter.in"
mkfifo "$fifo"

cleanup() {
  if [ -n "${admitter_pid:-}" ]; then kill "$admitter_pid" 2>/dev/null || true; fi
  if [ -n "${barrier_pid:-}" ]; then kill "$barrier_pid" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  psql -v ON_ERROR_STOP=1 -q "$db" <<'SQL' >/dev/null 2>&1 || true
UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id=''
 WHERE singleton=1;
DELETE FROM org_vault_key_use_intent WHERE team_id=970722;
DELETE FROM org_vault_rotation WHERE team_id=970722;
DELETE FROM org_vault_current WHERE principal='team:970722:provider:bedrock';
DELETE FROM org_vault_secret WHERE team_id=970722;
DELETE FROM org_vault_salt WHERE principal='team:970722:provider:bedrock';
DELETE FROM kb_team WHERE id=970722;
DELETE FROM org_vault_salt WHERE principal='p7-racing-mutation';
SQL
  rm -rf "$tmp"
}
trap cleanup EXIT

psql -v ON_ERROR_STOP=1 -q "$db" <<'SQL'
UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id=''
 WHERE singleton=1;
DELETE FROM org_vault_key_use_intent WHERE team_id=970722;
DELETE FROM org_vault_rotation WHERE team_id=970722;
DELETE FROM org_vault_current WHERE principal='team:970722:provider:bedrock';
DELETE FROM org_vault_secret WHERE team_id=970722;
DELETE FROM org_vault_salt WHERE principal='team:970722:provider:bedrock';
DELETE FROM kb_team WHERE id=970722;
INSERT INTO kb_team(id,name) VALUES(970722,'p7_barrier_concurrency');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('owner',970722,0);
SELECT set_config('aimee.principal','owner',false);
SELECT org_vault_put('team:970722:provider:bedrock',970722,'bedrock','primary',1,
  decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),decode('03','hex'),
  decode(repeat('04',16),'hex'));
DO $$ DECLARE rid BIGINT; BEGIN
  rid:=org_vault_rotation_start('owner','team:970722|bedrock|primary',
    'team:970722:provider:bedrock',970722,'bedrock','primary',1,false);
  PERFORM org_vault_rotation_stage('owner',rid,decode(repeat('11',40),'hex'),
    decode(repeat('12',12),'hex'),decode('13','hex'),decode(repeat('14',16),'hex'));
  PERFORM org_vault_rotation_transition('owner',rid,'staged','probed','');
  PERFORM org_vault_rotation_transition('owner',rid,'probed','activating','');
  PERFORM org_vault_rotation_finalize('owner',rid,'\xaabbcc'::bytea);
END $$;
SQL

# Session A admits durably inside an uncommitted transaction.  Its control-row
# FOR SHARE lock is the rendezvous: the barrier must queue behind it.
{
  printf '%s\n' \
    'BEGIN;' \
    "SELECT set_config('aimee.principal','owner',true);" \
    "SELECT newly_admitted FROM org_vault_key_use_admit('owner',970722,'cert:test-ca:barrier','held-use','team:970722|bedrock|primary','team:970722:provider:bedrock','bedrock','primary',2,repeat('a',64),'bedrock','anthropic.claude','invoke','\\xaabbcc'::bytea);" \
    "\\! touch $tmp/admitter.ready"
  cat "$fifo"
} | PGAPPNAME=p7-barrier-admitter psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/admitter.log" 2>&1 &
admitter_pid=$!
exec 3>"$fifo"

for _ in $(seq 1 100); do
  [ -f "$tmp/admitter.ready" ] && break
  sleep 0.05
done
[ -f "$tmp/admitter.ready" ] || { echo 'P7 barrier concurrency FAIL: admitter rendezvous timeout' >&2; exit 1; }

PGAPPNAME=p7-barrier-writer psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/barrier.log" 2>&1 <<'SQL' &
BEGIN;
SET LOCAL lock_timeout='10s';
SELECT org_vault_control_lock_exclusive();
UPDATE kb_vault_control SET sealed=true,seal_epoch=seal_epoch+1,
  maintenance_kind='reseal',maintenance_id='concurrency-op',fencing_token=fencing_token+1,
  updated_at=pg_now_text() WHERE singleton=1;
COMMIT;
SQL
barrier_pid=$!

for _ in $(seq 1 100); do
  waiting=$(psql -Atq "$db" -c "SELECT count(*) FROM pg_stat_activity WHERE application_name='p7-barrier-writer' AND wait_event_type='Lock'")
  [ "$waiting" = 1 ] && break
  sleep 0.05
done
[ "${waiting:-0}" = 1 ] || { echo 'P7 barrier concurrency FAIL: FOR UPDATE did not block behind admission' >&2; exit 1; }

# Queue fresh admissions and an unrelated vault mutation behind the already-waiting
# exclusive barrier.  PostgreSQL lock ordering makes them observe the committed seal.
race_pids=()
for i in $(seq 1 12); do
  (
    if PGAPPNAME="p7-barrier-racer-$i" psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/racer-$i.log" 2>&1 <<SQL
BEGIN;
SELECT set_config('aimee.principal','owner',true);
SELECT * FROM org_vault_key_use_admit('owner',970722,'cert:test-ca:barrier','race-$i',
  'team:970722|bedrock|primary','team:970722:provider:bedrock','bedrock','primary',2,
  repeat('b',64),'bedrock','anthropic.claude','invoke','\xaabbcc'::bytea);
COMMIT;
SQL
    then
      exit 10
    fi
    grep -q 'org_vault_control: sealed' "$tmp/racer-$i.log"
  ) &
  race_pids+=("$!")
done
(
  if PGAPPNAME=p7-barrier-mutation psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/mutation.log" 2>&1 <<'SQL'
SELECT org_vault_salt_ensure('p7-racing-mutation','\x01'::bytea);
SQL
  then
    exit 10
  fi
  grep -q 'org_vault_control: sealed' "$tmp/mutation.log"
) &
race_pids+=("$!")

for _ in $(seq 1 100); do
  racers_waiting=$(psql -Atq "$db" -c "SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE 'p7-barrier-racer-%' AND wait_event_type='Lock'")
  mutation_waiting=$(psql -Atq "$db" -c "SELECT count(*) FROM pg_stat_activity WHERE application_name='p7-barrier-mutation' AND wait_event_type='Lock'")
  [ "$racers_waiting" = 12 ] && [ "$mutation_waiting" = 1 ] && break
  sleep 0.05
done
if [ "${racers_waiting:-0}" != 12 ] || [ "${mutation_waiting:-0}" != 1 ]; then
  echo "P7 barrier concurrency FAIL: queued racers=$racers_waiting mutation=$mutation_waiting" >&2
  exit 1
fi

printf '%s\n' 'COMMIT;' >&3
exec 3>&-
wait "$admitter_pid"
unset admitter_pid
wait "$barrier_pid"
unset barrier_pid
for race_pid in "${race_pids[@]}"; do
  if ! wait "$race_pid"; then
    echo 'P7 barrier concurrency FAIL: a queued operation did not reject as sealed' >&2
    for log in "$tmp"/racer-*.log "$tmp"/mutation.log; do
      [ -f "$log" ] && { echo "== $log ==" >&2; tail -20 "$log" >&2; }
    done
    exit 1
  fi
done

held=$(psql -Atq "$db" -c "SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970722 AND use_id='held-use'")
raced=$(psql -Atq "$db" -c "SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970722 AND use_id LIKE 'race-%'")
mutated=$(psql -Atq "$db" -c "SELECT count(*) FROM org_vault_salt WHERE principal='p7-racing-mutation'")
sealed=$(psql -Atq "$db" -c "SELECT sealed AND maintenance_id='concurrency-op' FROM kb_vault_control WHERE singleton=1")
if [ "$held" != 1 ] || [ "$raced" != 0 ] || [ "$mutated" != 0 ] || [ "$sealed" != t ]; then
  echo "P7 barrier concurrency FAIL: held=$held raced=$raced mutated=$mutated sealed=$sealed" >&2
  exit 1
fi

echo '== P7 vault barrier concurrency: PASSED (commit-order fence, 12 admissions, mutation) =='
