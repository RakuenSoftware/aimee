#!/usr/bin/env bash
set -euo pipefail

db=${1:?usage: p7_vault_rewrap_concurrency.sh postgres-url}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

sql() { psql -v ON_ERROR_STOP=1 -Atq "$db" "$@"; }

wait_activity() {
  local app=$1 condition=$2
  local i
  for i in $(seq 1 100); do
    if [ "$(sql -c "SELECT count(*) FROM pg_stat_activity WHERE application_name='$app' AND $condition")" -gt 0 ]; then
      return 0
    fi
    sleep 0.05
  done
  echo "P7 rewrap concurrency FAIL: no rendezvous for $app ($condition)" >&2
  return 1
}

sql <<'SQL'
SELECT set_config('aimee.principal','owner',false);
INSERT INTO kb_team(id,name) VALUES(970723,'p7_rewrap_concurrency');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('owner',970723,0);
SELECT org_vault_salt_ensure('team:970723:provider:bedrock','\x01');
SELECT org_vault_kek_check_set('team:970723:provider:bedrock',decode(repeat('31',40),'hex'));
SELECT org_vault_put('team:970723:provider:bedrock',970723,'bedrock','primary',1,
  decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),'\x13',decode(repeat('14',16),'hex'));
-- Prior crash-recovery gates intentionally leave throwaway active rotations.
UPDATE org_vault_rotation SET state='retired',updated_at=pg_now_text() WHERE state<>'retired';
SQL

# A protected writer holds the shared barrier through commit.  Two independent
# begin attempts queue behind it; after it drains exactly one may install a barrier.
PGAPPNAME=p7_prebarrier_writer psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/writer.out" 2>&1 <<'SQL' &
BEGIN;
SELECT set_config('aimee.principal','owner',true);
SELECT org_vault_put('team:970723:provider:bedrock',970723,'bedrock','primary',2,
  decode(repeat('15',40),'hex'),decode(repeat('16',12),'hex'),'\x17',decode(repeat('18',16),'hex'));
SELECT pg_sleep(3);
COMMIT;
SQL
writer_pid=$!
wait_activity p7_prebarrier_writer "state='active' AND wait_event='PgSleep'"

PGAPPNAME=p7_begin_one psql -v ON_ERROR_STOP=1 -Atq "$db" >"$tmp/begin-one.out" 2>&1 \
  -c "SELECT state FROM org_vault_rewrap_begin('owner','concurrent-one','11111111111111111111111111111111',20,21)" &
begin_one_pid=$!
PGAPPNAME=p7_begin_two psql -v ON_ERROR_STOP=1 -Atq "$db" >"$tmp/begin-two.out" 2>&1 \
  -c "SELECT state FROM org_vault_rewrap_begin('owner','concurrent-two','22222222222222222222222222222222',20,21)" &
begin_two_pid=$!
wait_activity p7_begin_one "wait_event_type='Lock'"
wait_activity p7_begin_two "wait_event_type='Lock'"
if [ "$(sql -c "SELECT count(*) FROM kb_vault_rewrap_operation WHERE request_id LIKE 'concurrent-%'")" -ne 0 ]; then
  echo 'P7 rewrap concurrency FAIL: begin crossed an undrained protected writer' >&2
  exit 1
fi
wait "$writer_pid"
set +e
wait "$begin_one_pid"; one_rc=$?
wait "$begin_two_pid"; two_rc=$?
set -e
if [ $(( (one_rc == 0) + (two_rc == 0) )) -ne 1 ] ||
   [ "$(sql -c "SELECT count(*) FROM kb_vault_rewrap_operation WHERE request_id LIKE 'concurrent-%' AND state='preparing'")" -ne 1 ] ||
   [ "$(sql -c "SELECT count(*) FROM org_vault_secret WHERE principal='team:970723:provider:bedrock' AND version=2")" -ne 1 ]; then
  echo "P7 rewrap concurrency FAIL: begin winner/drain mismatch (one=$one_rc two=$two_rc)" >&2
  exit 1
fi
winner=$(sql -c "SELECT operation_id FROM kb_vault_rewrap_operation WHERE request_id LIKE 'concurrent-%'")
winner_fence=$(sql -c "SELECT fencing_token FROM kb_vault_rewrap_operation WHERE operation_id='$winner'")
sql -c "SELECT org_vault_rewrap_abort('$winner',$winner_fence,'concurrency_done')" >/dev/null

# A terminal operation and its old fence cannot be reused by a stale staging driver.
if sql -c "BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT * FROM org_vault_rewrap_secret_page('$winner',$winner_fence,0,1); COMMIT" >"$tmp/stale.out" 2>&1; then
  echo 'P7 rewrap concurrency FAIL: stale driver retained access' >&2
  exit 1
fi

# A staging transaction and abort take the same exclusive gate in the same order.
# The abort waits (rather than deadlocking), then removes the committed stage rows.
lock_op=33333333333333333333333333333333
lock_fence=$(sql -c "SELECT fencing_token FROM org_vault_rewrap_begin('owner','lock-order','$lock_op',21,22)")
sql -c "SELECT org_vault_rewrap_record_prepared('$lock_op',$lock_fence,'\\x6c6f636b',sha256('\\x6c6f636b'::bytea))" >/dev/null
PGAPPNAME=p7_stage_holder psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/stage-holder.out" 2>&1 <<SQL &
BEGIN ISOLATION LEVEL SERIALIZABLE;
DO \$\$ DECLARE r RECORD; n BYTEA; BEGIN
  SELECT * INTO STRICT r FROM org_vault_rewrap_secret_page('$lock_op',$lock_fence,0,1);
  n:=sha256(int8send(r.source_id)||decode('c1','hex'))||
     substring(sha256(int8send(r.source_id)||decode('c2','hex')) FROM 1 FOR 8);
  PERFORM org_vault_rewrap_stage_dek('$lock_op',$lock_fence,r.source_id,r.principal,r.agent,
    r.cred,r.version,r.source_digest,n);
END \$\$;
SELECT pg_sleep(3);
COMMIT;
SQL
stage_pid=$!
wait_activity p7_stage_holder "state='active' AND wait_event='PgSleep'"
PGAPPNAME=p7_abort_waiter psql -v ON_ERROR_STOP=1 -Atq "$db" >"$tmp/abort-waiter.out" 2>&1 \
  -c "SELECT org_vault_rewrap_abort('$lock_op',$lock_fence,'lock_order_done')" &
abort_pid=$!
wait_activity p7_abort_waiter "wait_event_type='Lock'"
wait "$stage_pid"
wait "$abort_pid"
if [ "$(sql -c "SELECT count(*) FROM kb_vault_rewrap_dek_stage WHERE operation_id='$lock_op'")" -ne 0 ]; then
  echo 'P7 rewrap concurrency FAIL: serialized abort left stage rows' >&2
  exit 1
fi

# Prepare a resealed operation, then kill its backend while the first source-row
# UPDATE is blocked in a test trigger.  PostgreSQL must expose either the complete
# promoted transaction or the unchanged resealed transaction, never a partial set.
promote_op=44444444444444444444444444444444
promote_fence=$(sql -c "SELECT fencing_token FROM org_vault_rewrap_begin('owner','disconnect-promote','$promote_op',22,23)")
sql -c "SELECT org_vault_rewrap_record_prepared('$promote_op',$promote_fence,decode(repeat('70',208),'hex'),sha256(decode(repeat('70',208),'hex')))" >/dev/null
sql <<SQL
BEGIN ISOLATION LEVEL SERIALIZABLE;
DO \$\$ DECLARE r RECORD; n BYTEA; BEGIN
  FOR r IN SELECT * FROM org_vault_rewrap_secret_page('$promote_op',$promote_fence,0,128) LOOP
    n:=sha256(int8send(r.source_id)||decode('d1','hex'))||
       substring(sha256(int8send(r.source_id)||decode('d2','hex')) FROM 1 FOR 8);
    PERFORM org_vault_rewrap_stage_dek('$promote_op',$promote_fence,r.source_id,r.principal,
      r.agent,r.cred,r.version,r.source_digest,n);
  END LOOP;
  FOR r IN SELECT * FROM org_vault_rewrap_check_page('$promote_op',$promote_fence,''::bytea,128) LOOP
    n:=CASE WHEN octet_length(r.kek_check)=0 THEN ''::bytea ELSE
       sha256(convert_to(r.principal,'UTF8')||decode('e1','hex'))||
       substring(sha256(convert_to(r.principal,'UTF8')||decode('e2','hex')) FROM 1 FOR 8) END;
    PERFORM org_vault_rewrap_stage_check('$promote_op',$promote_fence,r.principal,
      r.source_digest,n);
  END LOOP;
  PERFORM org_vault_rewrap_stage_finish('$promote_op',$promote_fence);
END \$\$;
COMMIT;
SELECT org_vault_rewrap_mark_committing('$promote_op',$promote_fence);
SELECT org_vault_rewrap_mark_resealed('$promote_op',$promote_fence,
  sha256(decode(repeat('70',208),'hex')));
CREATE OR REPLACE FUNCTION p7_rewrap_pause_update() RETURNS trigger LANGUAGE plpgsql AS
\$\$ BEGIN PERFORM pg_sleep(10); RETURN NEW; END \$\$;
CREATE TRIGGER p7_rewrap_pause_update BEFORE UPDATE OF wrapped_dek ON org_vault_secret
  FOR EACH ROW EXECUTE FUNCTION p7_rewrap_pause_update();
SQL

PGAPPNAME=p7_forced_disconnect psql -v ON_ERROR_STOP=1 -q "$db" >"$tmp/promote.out" 2>&1 \
  -c "BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT org_vault_rewrap_promote('$promote_op',$promote_fence); COMMIT" &
promote_pid=$!
wait_activity p7_forced_disconnect "state='active' AND wait_event='PgSleep'"
backend_pid=$(sql -c "SELECT pid FROM pg_stat_activity WHERE application_name='p7_forced_disconnect'")
sql -c "SELECT pg_terminate_backend($backend_pid)" >/dev/null
set +e
wait "$promote_pid"
promote_rc=$?
set -e
sql -c 'DROP TRIGGER p7_rewrap_pause_update ON org_vault_secret; DROP FUNCTION p7_rewrap_pause_update()' >/dev/null

state=$(sql -c "SELECT state FROM kb_vault_rewrap_operation WHERE operation_id='$promote_op'")
matched_secrets=$(sql -c "SELECT count(*) FROM org_vault_secret s JOIN kb_vault_rewrap_dek_stage x ON x.operation_id='$promote_op' AND x.source_id=s.id WHERE s.wrapped_dek=x.new_wrapped_dek")
total_secrets=$(sql -c "SELECT count(*) FROM kb_vault_rewrap_dek_stage WHERE operation_id='$promote_op'")
matched_checks=$(sql -c "SELECT count(*) FROM org_vault_salt s JOIN kb_vault_rewrap_check_stage x ON x.operation_id='$promote_op' AND x.principal=s.principal WHERE x.source_digest<>sha256(x.new_kek_check) AND s.kek_check=x.new_kek_check")
total_checks=$(sql -c "SELECT count(*) FROM kb_vault_rewrap_check_stage WHERE operation_id='$promote_op' AND source_digest<>sha256(new_kek_check)")
if [ "$state" = resealed ]; then
  if [ "$matched_secrets" -ne 0 ] || [ "$matched_checks" -ne 0 ]; then
    echo 'P7 rewrap concurrency FAIL: disconnected promotion partially committed' >&2
    exit 1
  fi
  sql -c "BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT org_vault_rewrap_promote('$promote_op',$promote_fence); COMMIT" >/dev/null
elif [ "$state" = promoted ]; then
  if [ "$matched_secrets" -ne "$total_secrets" ] || [ "$matched_checks" -ne "$total_checks" ]; then
    echo 'P7 rewrap concurrency FAIL: promoted outcome is incomplete' >&2
    exit 1
  fi
  replay=$(sql -c "BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT org_vault_rewrap_promote('$promote_op',$promote_fence); COMMIT")
  [ "$replay" = promoted ] || { echo 'P7 rewrap concurrency FAIL: promoted replay mismatch' >&2; exit 1; }
else
  echo "P7 rewrap concurrency FAIL: in-doubt outcome state=$state rc=$promote_rc" >&2
  exit 1
fi

# Completion and quarantine contend on the same exclusive gate and consumed
# operation fence.  Exactly one terminal checkpoint and one fence advance commit.
race_fence_before=$(sql -c 'SELECT fencing_token FROM kb_vault_control WHERE singleton=1')
PGAPPNAME=p7_complete_racer psql -v ON_ERROR_STOP=1 -v VERBOSITY=verbose -Atq "$db" >"$tmp/complete-racer.out" 2>&1 \
  -c "SELECT org_vault_rewrap_complete('$promote_op',$promote_fence,(SELECT receipt_digest FROM kb_vault_rewrap_operation WHERE operation_id='$promote_op'),(SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='$promote_op'),(SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='$promote_op'))" &
complete_pid=$!
PGAPPNAME=p7_recovery_racer psql -v ON_ERROR_STOP=1 -v VERBOSITY=verbose -Atq "$db" >"$tmp/recovery-racer.out" 2>&1 \
  -c "SELECT org_vault_rewrap_recovery_required('$promote_op',$promote_fence,'race_quarantine')" &
recovery_pid=$!
set +e
wait "$complete_pid"; complete_rc=$?
wait "$recovery_pid"; recovery_rc=$?
set -e
terminal_state=$(sql -c "SELECT state FROM kb_vault_rewrap_operation WHERE operation_id='$promote_op'")
terminal_events=$(sql -c "SELECT count(*) FROM kb_vault_rewrap_worm WHERE operation_id='$promote_op' AND event_kind IN ('completed','recovery_required')")
race_fence_after=$(sql -c 'SELECT fencing_token FROM kb_vault_control WHERE singleton=1')
if [ $(( (complete_rc == 0) + (recovery_rc == 0) )) -ne 1 ] ||
   [ "$terminal_events" -ne 1 ] || [ "$race_fence_after" -ne $((race_fence_before + 1)) ]; then
  echo "P7 rewrap concurrency FAIL: terminal race mismatch (complete=$complete_rc recovery=$recovery_rc state=$terminal_state events=$terminal_events)" >&2
  exit 1
fi
if [ "$terminal_state" = completed ]; then
  if [ "$complete_rc" -ne 0 ] ||
     [ "$(sql -c "SELECT count(*) FROM kb_vault_control WHERE singleton=1 AND sealed AND maintenance_kind='' AND maintenance_id=''")" -ne 1 ] ||
     ! grep -q '40001' "$tmp/recovery-racer.out"; then
    echo 'P7 rewrap concurrency FAIL: completed winner/barrier/loser mismatch' >&2
    exit 1
  fi
elif [ "$terminal_state" = recovery_required ]; then
  if [ "$recovery_rc" -ne 0 ] ||
     [ "$(sql -c "SELECT count(*) FROM kb_vault_control WHERE singleton=1 AND sealed AND maintenance_kind='tpm2-reseal' AND maintenance_id='$promote_op'")" -ne 1 ] ||
     ! grep -q 'P7C01' "$tmp/complete-racer.out"; then
    echo 'P7 rewrap concurrency FAIL: quarantine winner/barrier/loser mismatch' >&2
    exit 1
  fi
else
  echo "P7 rewrap concurrency FAIL: invalid terminal race state=$terminal_state" >&2
  exit 1
fi

# Test-only owner reset. Production exposes no terminal-clear operation in D1.
sql -c "UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id='',updated_at=pg_now_text() WHERE singleton=1" >/dev/null

echo '== P7-reseal-d1 concurrency/failure gate: PASSED =='
