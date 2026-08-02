#!/bin/bash
# Pull results from both hosts, re-score locally, and commit. Run on a loop.
#
# Results are the durable artefact; the hosts are scratch. Anything not pulled
# is lost if a container is recycled, and an overnight run that finishes at 3am
# with nothing in git is an overnight run wasted.
#
# Scoring happens HERE, not on the hosts, and the pulled score files are ignored.
# Earlier in this effort a sync script pulled host-side .score.json files and
# reverted local scorer fixes three separate times. The predictions are the raw
# data; the scores are derived and must be re-derived by whatever scorer is
# current.
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
A=bench/tier-a/results
B=bench/tier-b/results

pull() {  # <ssh-target> <remote-dir> <local-dir> [pct-id]
  local tgt=$1 rem=$2 loc=$3 pct=${4:-}
  mkdir -p "$loc"
  local listing
  if [ -n "$pct" ]; then
    listing=$(ssh -o ConnectTimeout=15 "$tgt" "pct exec $pct -- bash -lc 'ls $rem/*.pred.jsonl $rem/*.device.json 2>/dev/null'" 2>/dev/null)
  else
    listing=$(ssh -o ConnectTimeout=15 "$tgt" "ls $rem/*.pred.jsonl $rem/*.device.json 2>/dev/null" 2>/dev/null)
  fi
  for f in $listing; do
    local base; base=$(basename "$f")
    if [ -n "$pct" ]; then
      ssh -o ConnectTimeout=15 "$tgt" "pct exec $pct -- cat '$f'" > "$loc/$base.tmp" 2>/dev/null
    else
      ssh -o ConnectTimeout=15 "$tgt" "cat '$f'" > "$loc/$base.tmp" 2>/dev/null
    fi
    if [ -s "$loc/$base.tmp" ]; then mv "$loc/$base.tmp" "$loc/$base"; else rm -f "$loc/$base.tmp"; fi
  done
}

pull root@192.168.1.253 /opt/tierA/bench/tier-a/results/thinking "$A/thinking" 140
pull root@192.168.1.253 /opt/tierA/bench/tier-a/results/sub1b    "$A/sub1b"    140
pull root@192.168.1.253 /opt/tierA/bench/tier-b/results/sub1b    "$B/sub1b"    140
pull root@192.168.1.253 /opt/tierA/bench/tier-b/results/cpufit   "$B/cpufit"   140
pull admin@192.168.1.254 /mnt/media/tierbench/repo/bench/tier-a/results/challenger-254 "$A/challenger-254"

# Re-score locally with the CURRENT scorer. A refusal is recorded, not hidden:
# the run index marks it INVALID and the file stays for inspection.
for lane in "$A"/thinking "$A"/sub1b "$A"/challenger-254; do
  [ -d "$lane" ] || continue
  for p in "$lane"/*.pred.jsonl; do
    [ -e "$p" ] || continue
    l=${p%.pred.jsonl}
    python3 bench/tier-a/harness/score.py --gold bench/tier-a/data/gold.jsonl \
        --pred "$p" --json-out "$l.score.json" >/dev/null 2>&1 || rm -f "$l.score.json"
  done
done
for lane in "$B"/sub1b "$B"/cpufit; do
  [ -d "$lane" ] || continue
  for p in "$lane"/*.pred.jsonl; do
    [ -e "$p" ] || continue
    l=${p%.pred.jsonl}
    python3 bench/tier-b/harness/score_b.py --topics bench/tier-b/data/topics.jsonl \
        --pred "$p" --json-out "$l.score.json" >/dev/null 2>&1 || rm -f "$l.score.json"
  done
done

python3 bench/evidence/build_index.py >/dev/null 2>&1
if ! git diff --quiet bench/ || [ -n "$(git ls-files -o --exclude-standard bench/)" ]; then
  git add -A bench/
  # Commit ONLY bench/. A bare `git commit` takes the WHOLE index, not just what
  # this script staged, so any change another session had staged got swept into a
  # commit titled "bench: overnight results". That happened: a 59-line deletion of
  # Dockerfile.embedder landed in 944811335 under a benchmark-results message.
  # The pathspec makes the commit match its own subject line.
  git commit -q -m "bench: overnight results as of $(date -u +%Y-%m-%dT%H:%MZ)

Pulled from both hosts and re-scored locally with the current scorer. Host-side
score files are deliberately ignored: predictions are the raw artefact, scores
are derived, and pulling derived scores previously reverted scorer fixes three
times." -- bench/ && echo "committed $(date -u +%H:%MZ)"
else
  echo "no change $(date -u +%H:%MZ)"
fi
