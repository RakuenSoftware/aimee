#!/bin/sh
# Build and BOOT every aimee-kb image variant, on a real Docker host.
#
# CI proves the images build. It never runs one, so everything that only fails at
# start-up — the embedder actually loading, llama-server resolving its libraries,
# the baked GGUF being a model this llama.cpp can read — is unverified until here.
#
# Docker's data-root sits on the 4TB pool, so every variant is kept for the compose
# smoke test that follows. The shared base layers cache, so only the first build
# pays for the C and pgvectorscale compiles; the rest pay for their embedder bake
# and their model.
set -u
cd "$(dirname "$0")" || exit 1

pass=0; fail=0
say() { echo "[$(date +%H:%M:%S)] $*"; }
ok()   { pass=$((pass+1)); say "  PASS  $*"; }
bad()  { fail=$((fail+1)); say "  FAIL  $*"; }

# variant | embedder | with_llama | synth_model | expected_dim
VARIANTS='
aimee-kb|bekko-a25m|0||384
aimee-kb-nomic|nomic-embed-text-v2-moe|0||768
aimee-kb-llm-e2b|bekko-a25m|1|gemma-4-E2B-it|384
aimee-kb-nomic-llm-e4b|nomic-embed-text-v2-moe|1|gemma-4-E4B-it|768
aimee-kb-llm-e4b|bekko-a25m|1|gemma-4-E4B-it|384
aimee-kb-nomic-llm-e2b|nomic-embed-text-v2-moe|1|gemma-4-E2B-it|768
'

for row in $VARIANTS; do
  [ -z "$row" ] && continue
  name=$(echo "$row" | cut -d'|' -f1)
  emb=$(echo "$row"  | cut -d'|' -f2)
  llm=$(echo "$row"  | cut -d'|' -f3)
  syn=$(echo "$row"  | cut -d'|' -f4)
  dim=$(echo "$row"  | cut -d'|' -f5)

  say "=== $name (embedder=$emb llama=$llm model=${syn:-none}) ==="
  df -h / | tail -1

  if ! docker build --network=host \
      --build-arg AIMEE_EMBEDDER="$emb" \
      --build-arg AIMEE_WITH_LLAMACPP="$llm" \
      --build-arg AIMEE_SYNTHESIS_MODEL="$syn" \
      --build-arg AIMEE_MODEL_MIRROR="${AIMEE_MODEL_MIRROR:-}" \
      -t "$name:test" . > "build-$name.log" 2>&1; then
    bad "$name: BUILD — $(grep -aoE 'ERROR: .{0,90}' "build-$name.log" | tail -1)"
    continue
  fi
  ok "$name: builds"

  # --- the image records what it is -----------------------------------------
  got_llm=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$name:test" \
            | grep '^AIMEE_WITH_LLAMACPP=' | cut -d= -f2)
  got_syn=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$name:test" \
            | grep '^AIMEE_SYNTHESIS_MODEL=' | cut -d= -f2)
  [ "$got_llm" = "$llm" ] && ok "$name: AIMEE_WITH_LLAMACPP=$got_llm" \
                          || bad "$name: AIMEE_WITH_LLAMACPP=$got_llm want $llm"
  [ "$got_syn" = "$syn" ] && ok "$name: AIMEE_SYNTHESIS_MODEL='${got_syn}'" \
                          || bad "$name: AIMEE_SYNTHESIS_MODEL='$got_syn' want '$syn'"

  # --- the embedder is the one this tag promises, and it EMBEDS --------------
  # Serves on loopback inside the container; ask it for a vector and check width.
  emb_out=$(docker run --rm --entrypoint sh "$name:test" -c "
      EMBEDDER_MODEL=$emb EMBEDDER_PORT=8760 \
        /opt/aimee/embedder-venv/bin/python /opt/aimee/scripts/embedder-server.py >/tmp/e.log 2>&1 &
      for i in \$(seq 1 90); do
        curl -fsS -m 2 http://127.0.0.1:8760/health >/dev/null 2>&1 && break; sleep 2
      done
      curl -fsS -m 60 'http://127.0.0.1:8760/embed?input_type=query' \
        -H 'content-type: application/json' -d '{\"text\":\"hello\"}' 2>/dev/null \
        | python3 -c \"
import json,sys
try:
    v = json.load(sys.stdin)
except Exception:
    print('DIM=nojson'); raise SystemExit
for k in ('embedding','vector','vectors','embeddings','data'):
    if isinstance(v, dict) and k in v: v = v[k]
while isinstance(v, list) and v and isinstance(v[0], list): v = v[0]
print('DIM=%d' % len(v) if isinstance(v, list) else 'DIM=novec')
\" 2>/dev/null || { echo DIM=embfail; tail -4 /tmp/e.log; }
  " 2>&1 | grep -a 'DIM=' | tail -1)
  # Match the whole marker line: the container also prints loader warnings, and an
  # earlier version of this check grepped for a bare number and matched those.
  if [ "$emb_out" = "DIM=$dim" ]; then
    ok "$name: embedder serves ${dim}-dim vectors"
  else
    bad "$name: embedder dim -> '${emb_out:-no-DIM-line}' want DIM=$dim"
  fi

  # --- llama.cpp + the baked model actually load and generate ----------------
  if [ "$llm" = "1" ]; then
    if docker run --rm --entrypoint /opt/aimee/llama.cpp/llama-server "$name:test" --version >/dev/null 2>&1; then
      ok "$name: llama-server runs (shared objects resolve)"
    else
      bad "$name: llama-server --version failed"
    fi

    sz=$(docker run --rm --entrypoint sh "$name:test" -c 'stat -c %s /opt/aimee/llama.cpp/model/synthesis.gguf 2>/dev/null || echo 0')
    if [ "$sz" -gt 1000000000 ]; then
      ok "$name: baked model present ($((sz/1000000000)) GB)"
    else
      bad "$name: baked model missing/short ($sz bytes)"
    fi

    # The real question: can THIS llama.cpp load THIS gguf and answer?
    gen=$(docker run --rm --entrypoint sh "$name:test" -c "
        LLAMA_ARG_MMPROJ_AUTO=false /opt/aimee/llama.cpp/llama-server \
          -m /opt/aimee/llama.cpp/model/synthesis.gguf --host 127.0.0.1 --port 8761 \
          -c 512 --no-webui --no-mmproj > /tmp/l.log 2>&1 &
        for i in \$(seq 1 150); do
          curl -fsS -m 2 http://127.0.0.1:8761/health >/dev/null 2>&1 && break; sleep 2
        done
        curl -fsS -m 120 http://127.0.0.1:8761/v1/chat/completions \
          -H 'content-type: application/json' \
          -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Reply with the single word: ok\"}],\"max_tokens\":8}' \
          2>/dev/null | head -c 400 || { echo GENFAIL; tail -12 /tmp/l.log; }
    " 2>&1 | tail -6)
    if echo "$gen" | grep -q '"choices"'; then
      ok "$name: llama-server LOADED the model and completed a request"
    else
      bad "$name: generation -> $(echo "$gen" | tr '\n' ' ' | head -c 200)"
    fi
  fi

  # Images are KEPT: docker's data-root is on the big pool now, and the compose
  # smoke test wants them afterwards.
  say "  (kept; $(df -h /var/lib/docker-pool | tail -1 | awk '{print $4}') free on the pool)"
done

say "==== matrix: $pass passed, $fail failed ===="
[ "$fail" -eq 0 ]
