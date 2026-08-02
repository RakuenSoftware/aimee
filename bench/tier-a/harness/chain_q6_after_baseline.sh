#!/bin/bash
# Run the Q6 arm the moment the v8 baseline finishes, so the 5080 never idles
# and the Q4/Q6 pair is taken back-to-back on the same card and server build.
cd "$(dirname "$0")/.." || exit 1
until grep -q "BASELINE DONE" results/v8-baseline/driver.log 2>/dev/null; do sleep 60; done
GOLD=$PWD/data/corpora/v5/gold_small.jsonl OUT=$PWD/results/v8-baseline \
  bash harness/sweep_e2b_q6.sh
