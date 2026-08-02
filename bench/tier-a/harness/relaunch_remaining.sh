#!/bin/bash
# Wait for the running driver to exit, then relaunch so the remaining arms run.
#
# The first driver had ssh without -n inside a `while read` loop, so the first
# ssh consumed the arm list and every lane ran exactly one arm before reporting
# LANE DONE. The fix is in overnight_10k.sh; this picks up where that left off.
# Arms whose prediction file already holds the full row count are skipped, so
# nothing already measured is recomputed.
#
# It waits rather than restarting immediately because an arm was still writing:
# relaunching over a live run would have two processes writing one prediction
# file.
#
# It runs a COPY of the driver. bash reads a script by byte offset as it
# executes, so editing the file a running driver is executing can splice new
# content into the running process — the same hazard that made editing a live
# sweep script dangerous earlier in this effort.
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT" || exit 1
SRC=bench/tier-a/harness/overnight_10k.sh
COPY=bench/tier-a/harness/.overnight_run2.sh
cp "$SRC" "$COPY"

while pgrep -f "overnight_10k.sh" >/dev/null 2>&1; do sleep 60; done
sleep 30
GOLD="$ROOT/bench/tier-a/data/corpora/v3/gold_large.jsonl" \
OUT="$ROOT/bench/tier-a/results/v3-large" \
bash "$COPY"
