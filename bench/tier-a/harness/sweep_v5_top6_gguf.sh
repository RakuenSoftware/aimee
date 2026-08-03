#!/bin/bash
# Article 1's blocker: re-run the top six on the CURRENT corpus and ontology.
#
# Every number in the 16-model table predates three fixes to the benchmark
# itself -- the "runs on" corpus template (finding 4), the seed ontology that
# did not define 19% of its own gold's predicates and is now 24 relations rather
# than 17 (finding 6), and the prompt clause that silently suppressed
# gemma-4-E4B's reasoning (finding 1). It was also taken at 69 notes, where the
# comparable 95% interval is near +/- 0.12 and cannot resolve granite-4.0-1b
# (0.592) against gemma-4-E2B (0.593) at all. At 1001 notes it is roughly
# +/- 0.03.
#
# Runs on the 5080 in CT 140 while the XTX works the 10k quant ladder.
#
# THE CONFIGURATION IS UNIFORM ACROSS ALL SIX, and every choice below is forced
# by that rather than by taste. This sweep ranks models against each other, so
# anything that differs between them is a confound, and this benchmark moves
# more between configurations than between the models it compares.
#
#   quant     UD-Q4_K_XL for all six. Verified available on every repo. Q8 would
#             be closer to the original bf16 table, but gemma-3n-E4B at Q8 is
#             9.86 GiB and will not fit the 15.5 GiB card with room to serve.
#
#   NPROC=1   Forced by the largest model, not chosen. gemma-3n-E4B at Q4 is
#             5.02 GiB and three of them plus KV do not fit. Sharding the small
#             models and not the big ones would make process count a variable
#             across the ranking, and process count is worth 0.0105 F1 here
#             (finding 19) -- larger than most of the gaps being measured.
#
#   DRAFT=""  MTP OFF for all six. Only gemma-4 publishes an mtp-*.gguf;
#             granite-4.0-1b, granite-4.1-3b, gemma-3n-E4B and Qwen3-1.7B have
#             none. Speculation on two models and not the other four would put
#             the draft head inside the comparison, and MTP moves 26 of 100
#             notes. Costs roughly 1.8x wall clock. Worth it.
#
#   cache-ram 1024 MiB, matching the ladder (finding 20). Results-affecting.
#
# Consequence, stated rather than buried: these numbers are NOT comparable to
# the bf16 transformers table they replace, and not comparable to the 10k ladder
# arms either (those run MTP at 3 processes). They are comparable to each other,
# which is the only thing article 1's ranking claim needs.
#
# Model order front-loads the claims most at risk: the E2B / granite-4.0-1b
# near-tie first, then the incumbent, then the rest.
set -u
cd "$(dirname "$0")/.." || exit 1

GOLD=${GOLD:-data/corpora/v5/gold_small.jsonl}
OUT=${OUT:-results/v5-rerun-gguf}
PORT=${PORT:-8500}
mkdir -p "$OUT"
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/sweep.log"; }

# label|gguf repo:quant
ARMS="\
gemma-4-E2B-it|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL
granite-4.0-1b|unsloth/granite-4.0-1b-GGUF:UD-Q4_K_XL
gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL
granite-4.1-3b|unsloth/granite-4.1-3b-GGUF:UD-Q4_K_XL
gemma-3n-E4B-it|unsloth/gemma-3n-E4B-it-GGUF:UD-Q4_K_XL
Qwen3-1.7B|unsloth/Qwen3-1.7B-GGUF:UD-Q4_K_XL"

say "=== v5 re-run (GGUF, 5080): 6 models, $EXPECT notes, 1 proc, no MTP, UD-Q4_K_XL"
while IFS='|' read -r label repo; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label (banked)"; continue; fi
  [ -e "$pred.errored" ] && mv "$pred.errored" "$pred.errored.$(date -u +%Y%m%dT%H%M%SZ)"

  say "--- $label  $repo"
  t0=$(date +%s)
  # DRAFT is exported EMPTY on purpose; shard_run.sh omits -hfd when it is.
  GOLD="$GOLD" OUT="$OUT" LABEL="$label" REPO="$repo" DRAFT="" \
    CARD=5080 NPROC=1 BASE_PORT="$PORT" CACHE_RAM_MIB=1024 \
    bash harness/shard_run.sh
  rc=$?
  t1=$(date +%s)
  if [ $rc -ne 0 ]; then say "FAIL $label (rc=$rc) -- continuing to next model"; continue; fi

  python3 harness/score.py --gold "$GOLD" --pred "$pred" \
      --json-out "$OUT/$label.score.json" >/dev/null 2>&1
  python3 harness/score.py --gold "$GOLD" --pred "$pred" --pred-key pred_nofloor \
      --json-out "$OUT/$label.score.nofloor.json" >/dev/null 2>&1
  f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  nf=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.score.nofloor.json'))['strict']['f1'])" 2>/dev/null || echo "?")
  # Article 1 quotes the unfloored figure: the confidence floor read four models
  # as 0.000 that were not zero.
  say "OK   $label floored=$f1 nofloor=$nf wall=$(( (t1-t0)/60 ))m"
done <<< "$ARMS"
say "=== V5 GGUF RERUN COMPLETE ==="
