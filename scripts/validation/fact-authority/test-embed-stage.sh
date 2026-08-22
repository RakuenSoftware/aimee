#!/bin/bash
# EMBED (5891): does the kb actually CALL a configured embedder?
#
# This stage was never exercised on this branch. `aimee status` reported
# "BLOCKED: no embedder configured -- memory and KB search cannot embed"
# throughout, and the llama-server serving Qwen answers 501 for /v1/embeddings,
# so the call path had never once been run.
#
# WHAT THIS ESTABLISHES, and only this: with an embedder configured and
# reachable, the kb resolves it, posts text, and receives vectors. The stub's own
# request log is the evidence -- ground truth for "was this endpoint called",
# which no amount of reading the C could settle.
#
# WHAT IT DOES NOT ESTABLISH: that vectors are persisted, indexed, or used in
# recall. Memory embeddings are not written synchronously by the store path, and
# this run produced no rows I could attribute with confidence, so nothing is
# claimed about persistence. Retrieval QUALITY is doubly out of reach: the stub's
# vectors are deterministic but carry no meaning.
#
# Two request shapes, which cost a round trip to find and are worth stating:
#     POST /embed_batch   body: a JSON array of strings
#     POST /embed         body: THE RAW TEXT, not JSON
# memory_core_scope_embed.c: "the polarity rides in the query string because the
# body is the raw text itself". A stub assuming JSON on both answers 400 to every
# single-text call, and those 400s trip the embedding circuit breaker
# ("embedding dependency unavailable; retry after N ms"), which then keeps
# refusing -- so a later, correct attempt looks like a failure of something else.
#
# Run AS ROOT in the container.
set -u
export LC_ALL=C
SOCK=/root/aimee-http.sock
PORT=8799
DIM=384
rc=0

store() {
  curl -s -m 25 --unix-socket "$SOCK" -H 'content-type: application/json' -X POST \
       --data "{\"key\":\"$1\",\"content\":\"$2\",\"tier\":\"L2\",\"kind\":\"fact\"}" \
       http://localhost/v1/memory/store
}
embed_calls() { grep -ac 'POST /embed' /root/stub-embedder.log 2>/dev/null || echo 0; }

pkill -f stub-embedder.py 2>/dev/null
sleep 1
: > /root/stub-embedder.log

echo "=== 1. embedder UP, and the kb pointed at it ==="
# Started BEFORE the kb, deliberately. Bringing it up second means the kb's first
# attempts fail, the breaker opens, and the leg that matters is then refused for
# a reason that has nothing to do with the code under test.
setsid nohup python3 /root/stub-embedder.py "$PORT" "$DIM" >/root/stub-embedder.log 2>&1 </dev/null &
sleep 2
health="$(curl -s -m 8 "http://127.0.0.1:$PORT/health")"
echo "  /health: $(printf '%s' "$health" | head -c 90)"
case "$health" in
  *serving_id*) ;;
  *) echo "FAIL: the stub is not answering; nothing below would mean anything" >&2; exit 1 ;;
esac

# EMBEDDER_URL outranks config precisely because it is how a running deployment
# is pointed at one (kb_curator_drain.c says so).
export EMBEDDER_URL="http://127.0.0.1:$PORT"
printf 'export EMBEDDER_URL=http://127.0.0.1:%s\n' "$PORT" > /root/embedder-env.sh
bash /root/start-kb.sh >/dev/null 2>&1
bash /root/smm.sh >/dev/null 2>&1
bash /root/install-postgres-module.sh >/dev/null 2>&1
sleep 4
: > /root/stub-embedder.log   # count only what the store below provokes

echo
echo "=== 2. store a memory ==="
store embed-probe-up "The release captain for the deployment runbook is Dana." >/dev/null
sleep 8
calls="$(embed_calls)"
ok="$(grep -a 'POST /embed' /root/stub-embedder.log 2>/dev/null | grep -c ' 200 ' || true)"
echo "  embed requests seen by the endpoint: ${calls:-0}  (200s: ${ok:-0})"

echo
echo "=== 3. control: embedder DOWN, store again ==="
# Without this, "the endpoint was called" could be a health probe or a leftover
# retry; the contrast is what ties the calls to the store.
pkill -f stub-embedder.py 2>/dev/null
sleep 1
before_down="$(embed_calls)"
store embed-probe-down "A second note stored while no embedder is reachable." >/dev/null
sleep 6
after_down="$(embed_calls)"
echo "  further requests while down: $(( after_down - before_down ))"

echo
if [ "${ok:-0}" -gt 0 ]; then
  echo "PASS: the kb called the configured embedder and it answered 200"
  echo "      (call path only -- persistence and recall are NOT claimed here)"
else
  echo "FAIL: the endpoint saw no successful embed request, so the stage did not run"
  tail -4 /root/stub-embedder.log 2>/dev/null | sed 's/^/      /'
  rc=1
fi
if [ "$(( after_down - before_down ))" -eq 0 ]; then
  echo "PASS: nothing reached it once down, so leg 2's calls were real"
else
  echo "NOTE: requests continued after it was stopped, so leg 2 is not solely"
  echo "      attributable to the store."
  rc=1
fi
exit $rc
