#!/usr/bin/env bash
# p4b_rate_concurrency.sh: the genuinely-parallel shared-window-not-N× gate for the P4b
# keyed fixed-window rate limiter. A sequential test cannot exercise the lost-update race
# — this harness fires N PARALLEL `psql -c "SELECT admitted FROM org_rate_check(...)"`
# (separate real connections, each its own txn) against ONE team whose policy admits
# EXACTLY K of the N checks (window_seconds wide enough that all land in the SAME window),
# then asserts:
#   * exactly K checks returned admitted=t and N-K returned admitted=f
#   * the shared window's count is EXACTLY K (never N× the limit across connections)
# The atomic lock-all/check-all/bump-all-or-none inside org_rate_check (row-level FOR
# UPDATE in deterministic dim_key order) is what serializes the N concurrent admissions,
# so no interleaving can over-run the shared Postgres window counter.
#
# Requires a real Postgres (schema_roles.sql + schema.sql + schema_grants.sql applied).
# Connection: pass a libpq URL as $1 (or set AIMEE_TEST_PG_URL). Example:
#   scripts/p4b_rate_concurrency.sh postgres://aimee:aimee@localhost:5432/aimee_p1_rls_gate
set -euo pipefail

DB_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$DB_URL" ]; then
  echo "p4b_rate_concurrency: no Postgres URL (arg1 or AIMEE_TEST_PG_URL). This gate does not skip." >&2
  exit 1
fi

TEAM=949101          # fixed seed team id (namespaced away from real data)
WINDOW=3600          # wide window: every parallel check lands in one bucket
K=5                  # the policy admits exactly K requests per window
N=20                 # concurrent checks (N-K must refuse)
DIMKEY="team:$TEAM"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== P4b rate concurrency: seeding team $TEAM with a team policy max=$K over ${WINDOW}s =="
# Idempotent reseed: clear any prior run's rows for this team, then set the policy. The
# owner principal passes org_rate_policy_set's admin gate. One session, one statement string.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<SQL
BEGIN;
SELECT set_config('aimee.principal', 'owner', true);
DELETE FROM org_rate_window WHERE dim_key = '$DIMKEY';
DELETE FROM org_rate_policy WHERE dim = 'team' AND scope_key = '$TEAM';
DELETE FROM kb_team WHERE id = $TEAM;
INSERT INTO kb_team(id, name) VALUES ($TEAM, 'p4b_conc_team');
SELECT org_rate_policy_set('team', '$TEAM', $WINDOW, $K);
COMMIT;
SQL

echo "== P4b rate concurrency: firing $N parallel org_rate_check against one window =="
pids=()
for i in $(seq 1 "$N"); do
  (
    psql -v ON_ERROR_STOP=1 -tA "$DB_URL" \
      -c "SELECT admitted FROM org_rate_check($TEAM, NULL, NULL, NULL)" \
      > "$WORK/out-$i.txt" 2> "$WORK/err-$i.txt" || echo "ERR" > "$WORK/out-$i.txt"
  ) &
  pids+=("$!")
done
# A backgrounded psql that exits non-zero must NOT abort the harness (set -e) before the
# assertions run — its "ERR" sentinel is collected and asserted below instead.
for p in "${pids[@]}"; do wait "$p" || true; done

# The `{ grep ... || true; } | wc -l` guard: a zero-match grep exits 1, which under set -e
# would abort the harness before the assertions run — swallow it so a legitimate 0 count
# is asserted, not fatal.
admitted=$( { grep -lx 't' "$WORK"/out-*.txt 2>/dev/null || true; } | wc -l | tr -d ' ' )
refused=$( { grep -lx 'f' "$WORK"/out-*.txt 2>/dev/null || true; } | wc -l | tr -d ' ' )
errored=$( { grep -lx 'ERR' "$WORK"/out-*.txt 2>/dev/null || true; } | wc -l | tr -d ' ' )

echo "   admitted=$admitted refused=$refused errored=$errored (of $N)"

# Read the shared window back: count must be EXACTLY K and never N× the limit.
count=$(psql -v ON_ERROR_STOP=1 -tA "$DB_URL" \
  -c "SELECT COALESCE(max(count),0) FROM org_rate_window WHERE dim_key='$DIMKEY'" \
  | tr -d ' ')

echo "   shared window count=$count  (limit K=$K, N=$N)"

fail=0
if [ "$errored" -ne 0 ]; then
  echo "P4b RATE CONCURRENCY FAIL: $errored check(s) errored (see $WORK/err-*.txt)"; fail=1
fi
if [ "$admitted" -ne "$K" ]; then
  echo "P4b RATE CONCURRENCY FAIL: admitted=$admitted (want exactly $K)"; fail=1
fi
if [ "$refused" -ne $((N - K)) ]; then
  echo "P4b RATE CONCURRENCY FAIL: refused=$refused (want $((N - K)))"; fail=1
fi
# The headline invariant: the shared window never over-ran the limit (not N×).
if [ "$count" -ne "$K" ]; then
  echo "P4b RATE CONCURRENCY FAIL: shared window count=$count (want exactly $K — shared, not N×)"; fail=1
fi

# Cleanup the seed so the shared gate db stays tidy for re-runs.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<SQL || true
SELECT set_config('aimee.principal', 'owner', true);
DELETE FROM org_rate_window WHERE dim_key = '$DIMKEY';
DELETE FROM org_rate_policy WHERE dim = 'team' AND scope_key = '$TEAM';
DELETE FROM kb_team WHERE id = $TEAM;
SQL

if [ "$fail" -ne 0 ]; then
  echo "== P4b rate concurrency gate: FAILED =="
  exit 1
fi
echo "== P4b rate concurrency gate: PASSED (exactly $K admitted, shared window count $count == $K, not N×) =="
