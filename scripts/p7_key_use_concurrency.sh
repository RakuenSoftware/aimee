#!/usr/bin/env bash
set -euo pipefail

db=${1:?usage: p7_key_use_concurrency.sh postgres-url}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

psql -v ON_ERROR_STOP=1 -q "$db" <<'SQL'
BEGIN;
SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES(970714,'p7_key_use_concurrency');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('owner',970714,1);
SELECT org_vault_put('team:970714:provider:bedrock',970714,'bedrock','primary',1,
  decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),decode('03','hex'),
  decode(repeat('04',16),'hex'));
DO $$ DECLARE rid BIGINT; BEGIN
  rid:=org_vault_rotation_start('owner','team:970714|bedrock|primary',
    'team:970714:provider:bedrock',970714,'bedrock','primary',1,false);
  PERFORM org_vault_rotation_stage('owner',rid,decode(repeat('11',40),'hex'),
    decode(repeat('12',12),'hex'),decode(repeat('13',23),'hex'),decode(repeat('14',16),'hex'));
  PERFORM org_vault_rotation_transition('owner',rid,'staged','probed','');
  PERFORM org_vault_rotation_transition('owner',rid,'probed','activating','');
  PERFORM org_vault_rotation_finalize('owner',rid,'\xaabbcc'::bytea);
END $$;
COMMIT;
SQL

for i in $(seq 1 12); do
  (
    psql -v ON_ERROR_STOP=1 -Atq "$db" -c \
      "BEGIN; SELECT set_tenant_context('owner',970714); SELECT newly_admitted FROM org_vault_key_use_admit('owner',970714,'cert:test-ca:01','same-use','team:970714|bedrock|primary','team:970714:provider:bedrock','bedrock','primary',2,repeat('e',64),'bedrock','anthropic.claude','invoke','\\xaabbcc'::bytea); COMMIT" \
      | grep -E '^[tf]$' >"$tmp/$i"
  ) &
done
wait

new=$(grep -h '^t$' "$tmp"/* | wc -l)
replay=$(grep -h '^f$' "$tmp"/* | wc -l)
intent=$(psql -Atq "$db" -c "SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970714 AND use_id='same-use'")
audit=$(psql -Atq "$db" -c "SELECT count(*) FROM kb_audit_outbox WHERE action='vault.key_use' AND subject='team:970714|bedrock|primary'")
if [ "$new" -ne 1 ] || [ "$replay" -ne 11 ] || [ "$intent" -ne 1 ] || [ "$audit" -ne 1 ]; then
  echo "P7 key-use concurrency FAIL: new=$new replay=$replay intent=$intent audit=$audit" >&2
  exit 1
fi
echo "== P7 key-use concurrency: PASSED (one new, 11 replay-only, one intent/audit) =="
