#!/bin/bash
# Wait for the noise-floor run, then rebuild the quant ledger from scratch.
#
# Both need the whole 5080 and neither may overlap: the previous ledger was
# invalidated precisely because one arm's servers survived into the next.
#
# The ledger starts EMPTY. Its predecessor is quarantined under
# results/quant-ledger-CONTAMINATED/ and none of it is reused, because the
# resumable "SKIP if the prediction file is complete" path would otherwise carry
# contaminated arms straight back into the new ledger.
set -u
cd "$(dirname "$0")/.." || exit 1
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a results/chain_5080.log; }

say "waiting for noise_floor to finish"
while pgrep -f noise_floor.sh >/dev/null 2>&1; do sleep 60; done
say "noise_floor done"

# Never resume into the quarantined results.
if [ -d results/quant-ledger ] && [ ! -f results/quant-ledger/.rebuilt ]; then
  say "removing a stale results/quant-ledger before the rebuild"
  rm -rf results/quant-ledger
fi
mkdir -p results/quant-ledger && touch results/quant-ledger/.rebuilt

say "starting quant ledger rebuild"
bash harness/rebuild_quant_ledger.sh
say "chain complete"

# The noise floor died on its first line (a `local` + `set -u` bug) and the
# chain read that as "finished". It is re-queued here, AFTER the ledger, so the
# two never share the card.
say "starting noise floor"
bash harness/noise_floor.sh
say "noise floor complete"
