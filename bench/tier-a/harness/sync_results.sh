#!/bin/bash
# Pull results out of the bench CT into the working tree, then rescore locally.
# Run from the repo root.
#
# Raw per-note predictions come back, and score files do NOT: the CT scores with
# whatever harness version it was staged with, so pulling its *.score.json
# silently reverted locally-fixed scoring three separate times (symmetry,
# inverses, alias folding). Predictions are the durable artefact; scores are
# derived and are always regenerated here against the current scorer.
set -eu
CT=${CT:-140}
HOST=${HOST:-root@192.168.1.253}
DEST=bench/tier-a/results

mkdir -p "$DEST"
ssh "$HOST" "pct exec $CT -- tar czf - -C /opt/tierA/bench/tier-a \
    --exclude='*.score.json' --exclude='*.score.nofloor.json' \
    --exclude='*.score.noalias.json' results 2>/dev/null" \
  > /tmp/tierA-results.tgz
tar xzf /tmp/tierA-results.tgz -C bench/tier-a

cd bench/tier-a
n=0
for p in results/*/*.pred.jsonl; do
  [ -e "$p" ] || continue
  b=${p%.pred.jsonl}
  python3 harness/score.py --gold data/gold.jsonl --pred "$p" \
      --json-out "$b.score.json" >/dev/null 2>&1 || echo "score failed: $p" >&2
  python3 harness/score.py --gold data/gold.jsonl --pred "$p" --no-alias \
      --json-out "$b.score.noalias.json" >/dev/null 2>&1 || true
  python3 harness/score.py --gold data/gold.jsonl --pred "$p" --pred-key pred_nofloor \
      --json-out "$b.score.nofloor.json" >/dev/null 2>&1 || true
  n=$((n + 1))
done
echo "synced and rescored $n prediction files"
