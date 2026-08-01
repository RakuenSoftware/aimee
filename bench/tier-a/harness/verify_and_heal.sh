#!/bin/bash
# Re-score every prediction file in a lane and delete the ones the scorer refuses.
#
# This exists because a sweep's "OK" line is not trustworthy evidence. The
# runners exit 0 even when every row carries a transport error, and until
# recently the sweeps tested only that exit status, so a run whose server died
# mid-note printed OK anyway (MEASUREMENT_LOG.md defect 28). Older prediction
# files on disk were produced under that regime.
#
# Rather than trust any summary line, this asks the scorer directly: can this run
# be scored? The scorer refuses incomplete runs, truncated rows and errored rows.
# Anything it refuses gets its prediction file removed, and because every sweep
# skips models that already have a prediction file, the next sweep pass re-runs
# exactly those and nothing else.
#
# Idempotent, and safe to run between sweeps. Never deletes a file it could score.
#
# Usage: verify_and_heal.sh <results-lane-dir> [gold-file]
set -u
LANE=${1:?usage: verify_and_heal.sh <results-lane-dir> [gold]}
GOLD=${2:-data/gold.jsonl}
PY=${PY:-/opt/bench/bin/python}

# A lane may carry a defer list: one model label per line in <lane>/DEFER.txt.
# A deferred model's partial predictions are left alone rather than deleted, so
# the sweep's own "skip if predictions exist" rule keeps it out of the queue.
#
# This is for a model that is real but ruinously expensive on the available
# hardware, where re-running it would consume the whole window and starve
# everything behind it. Qwen3.6-27B is the case: dense 27B at Q8_0 is ~29GB
# against a 16GB card, so llama.cpp serves it from CPU at 2.12 tok/s, which is
# 428 seconds per note and about 7 hours for one model.
#
# The partial file stays on disk and stays marked INVALID in evidence/RUNS.md.
# That is the honest record: it was started, it was not finished, and the reason
# is written down here.
DEFER="$LANE/DEFER.txt"

healed=0
kept=0
deferred=0
for PRED in "$LANE"/*.pred.jsonl; do
  [ -e "$PRED" ] || continue
  LABEL=$(basename "$PRED" .pred.jsonl)
  if [ -f "$DEFER" ] && grep -qxF "$LABEL" "$DEFER"; then
    echo "DEFER $LABEL -> left in place, see $DEFER"
    deferred=$((deferred + 1))
    continue
  fi
  if reason=$($PY harness/score.py --gold "$GOLD" --pred "$PRED" \
                  --json-out "$LANE/$LABEL.score.json" 2>&1 >/dev/null); then
    kept=$((kept + 1))
  else
    echo "HEAL $LABEL -> $(echo "$reason" | tail -1 | cut -c1-160)"
    rm -f "$PRED" "$LANE/$LABEL.score"*.json
    healed=$((healed + 1))
  fi
done
echo "verify_and_heal: $kept scoreable, $healed removed for re-run, $deferred deferred ($LANE)"
