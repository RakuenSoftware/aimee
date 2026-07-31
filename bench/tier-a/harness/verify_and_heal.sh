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

healed=0
kept=0
for PRED in "$LANE"/*.pred.jsonl; do
  [ -e "$PRED" ] || continue
  LABEL=$(basename "$PRED" .pred.jsonl)
  if reason=$($PY harness/score.py --gold "$GOLD" --pred "$PRED" \
                  --json-out "$LANE/$LABEL.score.json" 2>&1 >/dev/null); then
    kept=$((kept + 1))
  else
    echo "HEAL $LABEL -> $(echo "$reason" | tail -1 | cut -c1-160)"
    rm -f "$PRED" "$LANE/$LABEL.score"*.json
    healed=$((healed + 1))
  fi
done
echo "verify_and_heal: $kept scoreable, $healed removed for re-run ($LANE)"
