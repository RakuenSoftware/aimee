#!/bin/bash
# Does the GGUF path cost gemma-4 anything? E2B, both runtimes, matched settings.
#
# gemma-4 E2B/E4B carry Per-Layer Embeddings — `hidden_size_per_layer_input=256`
# and `vocab_size_per_layer_input=262144` are in the published config, verified
# rather than taken from a proposal. llama.cpp is reported not to inject the
# per-layer residual (ggml-org/llama.cpp#22243), which would make every gemma-4
# number in this benchmark a measurement of the GGUF path rather than the model.
#
# That worry is already contradicted by evidence in this repo: E4B's transformers
# run (results/gpu) and its llama.cpp run (results/llamacpp) are byte-identical
# on all 70 notes, and the llama.cpp server log carries 70 completed generations,
# so it is a real run and not a copied file. Greedy decoding over short, heavily
# constrained output converged exactly across Q8_0 and bf16.
#
# E4B is the wrong model to settle it on, though. It is the strong one: its
# per-token margins are wide, so an argmax survives both quantisation noise and a
# missing residual. E2B is half the size and scores 0.13 F1 lower, which means
# narrower margins and a far better chance of exposing a real difference. If E2B
# also lands byte-identical, the GGUF path is not costing gemma-4 anything on
# this task and the shipped recommendation stands on model numbers.
#
# Four arms, so runtime and thinking are each isolated rather than confounded:
#
#   arm 1  transformers  thinking OFF |  runtime delta, thinking held off
#   arm 2  llama.cpp     thinking OFF |
#   arm 3  transformers  thinking ON  |  runtime delta, thinking held on
#   arm 4  llama.cpp     thinking ON  |  <- the shipped configuration
#
# Reading 1v2 and 3v4 gives the runtime effect; 1v3 and 2v4 give the thinking
# effect. Nothing is compared against the older results/gpu E2B, which predates
# the current prompt and would confound runtime with prompt version.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/ple-control
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8094}
MAXTOK=8192   # matches the llama.cpp ladder, not run_hf.py's 512 default, so a
              # truncation in either arm is the model's and not this script's

score() {  # <label>
  local l=$1
  $PY harness/score.py --gold data/gold.jsonl --pred "$OUT/$l.pred.jsonl" \
      --json-out "$OUT/$l.score.json" >/dev/null 2>&1
  if [ -s "$OUT/$l.score.json" ]; then echo "OK   $l"
  else echo "FAIL $l -> scorer refused"; rm -f "$OUT/$l.pred.jsonl"; fi
}

# --- arm 1: transformers, bf16, PLE applied by the reference implementation ---
L=E2B.transformers
if [ -s "$OUT/$L.pred.jsonl" ]; then echo "SKIP $L"; else
  echo "=== $L (transformers, bf16) ==="
  if $PY harness/run_hf.py --model google/gemma-4-E2B-it --gold data/gold.jsonl \
       --max-new-tokens $MAXTOK --dtype bfloat16 --device cuda \
       --out "$OUT/$L.pred.jsonl" >"$OUT/$L.run.log" 2>&1; then
    score "$L"
  else
    echo "FAIL $L -> runner error: $(tail -3 "$OUT/$L.run.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$OUT/$L.pred.jsonl"
  fi
fi

# --- arm 2: llama.cpp, Q8_0, thinking OFF — matches arm 1 ---
L=E2B.llamacpp
if [ -s "$OUT/$L.pred.jsonl" ]; then echo "SKIP $L"; else
  echo "=== $L (llama.cpp, Q8_0) ==="
  LOG="$OUT/$L.server.log"
  $SERVER -hf ggml-org/gemma-4-E2B-it-GGUF:Q8_0 --port "$PORT" -c 8192 \
      --no-webui --no-mmproj -ngl 99 >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 240); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 10
  done
  if [ "$ready" = 1 ]; then
    if $PY harness/run_llamacpp.py --model "$L" --gold data/gold.jsonl \
         --no-thinking --max-tokens $MAXTOK \
         --out "$OUT/$L.pred.jsonl" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      score "$L"
    else
      echo "FAIL $L -> runner error"; rm -f "$OUT/$L.pred.jsonl"
    fi
  else
    echo "FAIL $L -> server never healthy: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-160)"
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; sleep 5
fi

# --- arm 3: transformers, bf16, thinking ON — the shipped configuration ---
# run_hf.py could not send this until now: it passed enable_thinking only for
# Qwen3, and gemma-4's template defaults it to false, so every gemma number in
# results/gpu is thinking-OFF by default rather than by choice. Production does
# not suppress thinking (kb_curator_provider.c:198), so arms 3 and 4 are the
# configuration we actually ship and arms 1 and 2 are the control.
L=E2B.transformers.thinking
if [ -s "$OUT/$L.pred.jsonl" ]; then echo "SKIP $L"; else
  echo "=== $L (transformers, bf16, thinking ON) ==="
  if $PY harness/run_hf.py --model google/gemma-4-E2B-it --gold data/gold.jsonl \
       --thinking --max-new-tokens $MAXTOK --dtype bfloat16 --device cuda \
       --out "$OUT/$L.pred.jsonl" >"$OUT/$L.run.log" 2>&1; then
    score "$L"
  else
    echo "FAIL $L -> runner error: $(tail -3 "$OUT/$L.run.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$OUT/$L.pred.jsonl"
  fi
fi

# --- arm 4: llama.cpp, Q8_0, thinking ON — same config, other runtime ---
L=E2B.llamacpp.thinking
if [ -s "$OUT/$L.pred.jsonl" ]; then echo "SKIP $L"; else
  echo "=== $L (llama.cpp, Q8_0, thinking ON) ==="
  LOG="$OUT/$L.server.log"
  $SERVER -hf ggml-org/gemma-4-E2B-it-GGUF:Q8_0 --port "$PORT" -c 8192 \
      --no-webui --no-mmproj -ngl 99 >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 240); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 10
  done
  if [ "$ready" = 1 ]; then
    if $PY harness/run_llamacpp.py --model "$L" --gold data/gold.jsonl \
         --thinking --max-tokens $MAXTOK \
         --out "$OUT/$L.pred.jsonl" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      score "$L"
    else
      echo "FAIL $L -> runner error"; rm -f "$OUT/$L.pred.jsonl"
    fi
  else
    echo "FAIL $L -> server never healthy: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-160)"
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; sleep 5
fi

# --- the comparison the lane exists for ---
$PY - <<'EOF'
import json, os
d = "results/ple-control"
def rows(p):
    return {json.loads(l)["id"]: json.loads(l) for l in open(p)} if os.path.exists(p) else None
a = rows(f"{d}/E2B.transformers.pred.jsonl")
b = rows(f"{d}/E2B.llamacpp.pred.jsonl")
if not a or not b:
    print("COMPARE: skipped, need both arms")
else:
    ids = sorted(set(a) & set(b))
    same = [i for i in ids if a[i]["raw"].strip() == b[i]["raw"].strip()]
    print(f"COMPARE: {len(same)}/{len(ids)} notes byte-identical across runtimes")
    for i in ids:
        if i not in same:
            print(f"  DIFF {i}\n    transformers: {a[i]['raw'][:160]!r}\n    llama.cpp   : {b[i]['raw'][:160]!r}")
EOF
printf '{"lane":"ple-control","arms":["transformers/bf16","llama.cpp/Q8_0"],"model":"google/gemma-4-E2B-it","thinking":false,"max_tokens":8192}\n' \
  > "$OUT/PROVENANCE.json"
echo "SWEEP_PLE_CONTROL_DONE"
