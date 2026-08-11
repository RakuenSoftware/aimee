#!/bin/sh
# cache-ab.sh: measure what the fold does to PROMPT-CACHE behaviour.
#
# This is the half of the fold-vs-compactor question that no offline harness can answer
# (see benchmarks/compaction-quality/retention_probe.c, which measures retention and cost
# and stops there). Cache hits are a property of real traffic against a real provider, so
# the numbers have to come off realized usage - which aimee already records and
# `aimee insights` already reports.
#
# It SNAPSHOTS and DIFFS. It deliberately does NOT flip config and does NOT drive the
# workload:
#   - flipping is a live behaviour change and should be a deliberate command someone
#     types, not a side effect buried in a measurement script;
#   - the workload is a delegate dispatch, which happens through the MCP tool, not a
#     shell.
#
# USAGE
#   ./cache-ab.sh snapshot before-off
#   ...run the workload...
#   ./cache-ab.sh snapshot after-off
#   aimee config set fold.enabled true          # the deliberate step, typed by a human
#   ./cache-ab.sh snapshot before-on
#   ...run the SAME workload...
#   ./cache-ab.sh snapshot after-on
#   ./cache-ab.sh diff before-off after-off
#   ./cache-ab.sh diff before-on  after-on
#   aimee config set fold.enabled false         # restore
#
# READING IT. The number that matters is cache_read as a share of prompt tokens. The
# fold's freeze exists to keep the folded prefix byte-identical turn to turn; if it holds,
# cache_read/prompt should be comparable or better with the fold on, while prompt tokens
# themselves fall. If cache_read collapses while prompt falls, the fold is buying smaller
# requests by throwing away cache warmth - the regression the freeze is meant to prevent,
# and the reason this is measured rather than argued.
#
# CONFOUNDS, stated because they can invalidate the result:
#   - insights is AGGREGATE over aimee's own agent traffic. Any other delegate or
#     roundtable running in the window lands in the same totals. Run it quiet.
#   - cache_read only accrues on a warm prefix, so a workload of one short turn measures
#     nothing. Use several turns over the same context.
#   - the two arms must be the SAME work. Different tasks produce different cache shapes
#     and the comparison is then meaningless.
set -eu

DIR="${CACHE_AB_DIR:-benchmarks/compaction-quality/.cache-ab}"
DAYS="${CACHE_AB_DAYS:-1}"

usage() { sed -n '2,40p' "$0"; exit 2; }

case "${1:-}" in
snapshot)
   [ $# -eq 2 ] || usage
   mkdir -p "$DIR"
   aimee --json insights --days "$DAYS" > "$DIR/$2.json"
   echo "snapshot $2 -> $DIR/$2.json"
   ;;
diff)
   [ $# -eq 3 ] || usage
   python3 - "$DIR/$2.json" "$DIR/$3.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))

def d(k):
    return b.get(k, 0) - a.get(k, 0)

calls  = d("total_calls")
prompt = d("prompt_tokens")
cread  = d("cache_read_tokens")
cwrite = d("cache_write_tokens")

print(f"calls           {calls}")
print(f"prompt tokens   {prompt}")
print(f"cache read      {cread}")
print(f"cache write     {cwrite}")
if prompt > 0:
    print(f"cache_read/prompt   {100.0*cread/prompt:.1f}%")
    print(f"cache_write/prompt  {100.0*cwrite/prompt:.1f}%")
else:
    # A zero-delta window is not a 0% cache rate; it is no measurement at all, and
    # printing 0% would read as a catastrophic result rather than an empty one.
    print("cache_read/prompt   n/a - no prompt tokens in this window, nothing was measured")
if calls == 0:
    print("WARNING: zero calls in this window - the workload did not run, or ran outside it")
PY
   ;;
*)
   usage
   ;;
esac
