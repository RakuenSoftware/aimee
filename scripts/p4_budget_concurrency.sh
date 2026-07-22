#!/usr/bin/env bash
# p4_budget_concurrency.sh: the genuinely-parallel over-commit-safety gate for the P4a
# budget reservation core. A sequential test cannot exercise the lost-update race — this
# harness fires N PARALLEL `psql -c "SELECT org_budget_reserve(...)"` (separate real
# connections, each its own txn) against ONE team whose day limit admits EXACTLY K of the
# N reserves, then asserts:
#   * exactly K reserves returned 'granted' and N-K returned 'refused:team budget exceeded'
#   * the counter's reserved_usd never exceeded the limit (spend + reserved <= limit)
#   * exactly K admitted reservation rows exist
# The atomic FOR-UPDATE-ordered check inside org_budget_reserve is what serializes the N
# concurrent admissions, so no interleaving can over-commit the shared Postgres counter.
#
# Requires a real Postgres (schema_roles.sql + schema.sql + schema_grants.sql applied).
# Connection: pass a libpq URL as $1 (or set AIMEE_TEST_PG_URL). Example:
#   scripts/p4_budget_concurrency.sh postgres://aimee:aimee@localhost:5432/aimee_p1_rls_gate
set -euo pipefail

DB_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$DB_URL" ]; then
  echo "p4_budget_concurrency: no Postgres URL (arg1 or AIMEE_TEST_PG_URL). This gate does not skip." >&2
  exit 1
fi

TEAM=949001          # fixed seed team id (namespaced away from real data)
UNIT=1               # per-reserve reserved_max (USD)
K=5                  # the limit admits exactly K reserves
N=20                 # concurrent reserve attempts (N-K must refuse)
LIMIT=$((K * UNIT))
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== P4 concurrency: seeding team $TEAM with a day limit of $LIMIT (admits exactly $K x $UNIT) =="
# Idempotent reseed: clear any prior run's rows for this team, then set the cap. The
# owner principal passes org_budget_set's admin gate. One session, one statement string.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<SQL
BEGIN;
SELECT set_config('aimee.principal', 'owner', true);
DELETE FROM org_budget_reservation_alloc a
  USING org_budget_reservation r WHERE a.reservation_id = r.id AND r.team_id = $TEAM;
DELETE FROM org_budget_reservation WHERE team_id = $TEAM;
DELETE FROM org_budget_counter WHERE team_id = $TEAM;
DELETE FROM org_budget WHERE team_id = $TEAM;
DELETE FROM kb_team WHERE id = $TEAM;
INSERT INTO kb_team(id, name) VALUES ($TEAM, 'p4_conc_team');
SELECT org_budget_set($TEAM, NULL, 'day', $LIMIT, NULL);
COMMIT;
SQL

echo "== P4 concurrency: firing $N parallel reserves of $UNIT each =="
# Each reserve is a DISTINCT (origin, request_id) so idempotency read-back does not
# collapse them onto one row — they genuinely contend for the same counter balance.
pids=()
for i in $(seq 1 "$N"); do
  (
    psql -v ON_ERROR_STOP=1 -tA "$DB_URL" \
      -c "SELECT org_budget_reserve('conc-origin', 'req-$i', $TEAM, NULL, 1, $UNIT, 3600)" \
      > "$WORK/out-$i.txt" 2> "$WORK/err-$i.txt" || echo "ERR" > "$WORK/out-$i.txt"
  ) &
  pids+=("$!")
done
# A backgrounded psql that exits non-zero must NOT abort the harness (set -e) before
# the assertions run — its "ERR" sentinel is collected and asserted below instead.
for p in "${pids[@]}"; do wait "$p" || true; done

granted=$( { grep -lx 'granted' "$WORK"/out-*.txt 2>/dev/null || true; } | wc -l | tr -d ' ' )
refused=$( { grep -lx 'refused:team budget exceeded' "$WORK"/out-*.txt 2>/dev/null || true; } | wc -l | tr -d ' ' )
errored=$( { grep -lx 'ERR' "$WORK"/out-*.txt 2>/dev/null || true; } | wc -l | tr -d ' ' )

echo "   granted=$granted refused=$refused errored=$errored (of $N)"

# Read the counter back: reserved_usd must be exactly K*UNIT and never exceed the limit.
reserved=$(psql -v ON_ERROR_STOP=1 -tA "$DB_URL" \
  -c "SELECT COALESCE(reserved_usd,0) FROM org_budget_counter WHERE team_id=$TEAM AND COALESCE(project_id,0)=0 AND period='day'" \
  | tr -d ' ')
admitted=$(psql -v ON_ERROR_STOP=1 -tA "$DB_URL" \
  -c "SELECT count(*) FROM org_budget_reservation WHERE team_id=$TEAM AND state='admitted'" \
  | tr -d ' ')

echo "   counter reserved_usd=$reserved  admitted reservations=$admitted  (limit=$LIMIT)"

fail=0
if [ "$errored" -ne 0 ]; then
  echo "P4 CONCURRENCY FAIL: $errored reserve(s) errored (see $WORK/err-*.txt)"; fail=1
fi
if [ "$granted" -ne "$K" ]; then
  echo "P4 CONCURRENCY FAIL: granted=$granted (want exactly $K)"; fail=1
fi
if [ "$refused" -ne $((N - K)) ]; then
  echo "P4 CONCURRENCY FAIL: refused=$refused (want $((N - K)))"; fail=1
fi
# The headline invariant: the shared counter never over-committed.
if awk "BEGIN{exit !($reserved > $LIMIT)}"; then
  echo "P4 CONCURRENCY FAIL: reserved_usd=$reserved EXCEEDED limit=$LIMIT (over-commit!)"; fail=1
fi
if [ "$admitted" -ne "$K" ]; then
  echo "P4 CONCURRENCY FAIL: admitted reservations=$admitted (want $K)"; fail=1
fi

# Cleanup the seed so the shared gate db stays tidy for re-runs.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<SQL || true
SELECT set_config('aimee.principal', 'owner', true);
DELETE FROM org_budget_reservation_alloc a
  USING org_budget_reservation r WHERE a.reservation_id = r.id AND r.team_id = $TEAM;
DELETE FROM org_budget_reservation WHERE team_id = $TEAM;
DELETE FROM org_budget_counter WHERE team_id = $TEAM;
DELETE FROM org_budget WHERE team_id = $TEAM;
DELETE FROM kb_team WHERE id = $TEAM;
SQL

if [ "$fail" -ne 0 ]; then
  echo "== P4 concurrency gate: FAILED =="
  exit 1
fi
echo "== P4 concurrency gate: PASSED (exactly $K granted, reserved $reserved <= limit $LIMIT) =="
