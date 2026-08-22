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
PERSISTENCE, which is reported rather than asserted, because what was found is
# more interesting than a pass or a fail.
#
# Vectors DO persist -- `memory_embeddings` holds rows of halfvec(384), and
# kb_meta records `schema_embedder_serving_id = aimee-e2e-stub-v1-dim384`, i.e.
# THIS stub produced them. So the write path works and the vector space agrees.
#
# But a NEW store does not add a row: measured at 5, 10, 15, 20, 30 and 40
# seconds after storing, the count did not move, while the endpoint took 15
# successful embed calls in the same window. The existing rows appear to come
# from a bulk path that ran when the embedder first became available (db2_init
# records the serving identity and dim at that point), not from the store.
#
# That is left as an open observation, not a verdict. It may be correct
# behaviour -- a queue this probe does not wait long enough for, or a sync
# deliberately not on the store hot path -- and calling it a defect on this
# evidence would be a guess. The counts are printed so the next person starts
# from the measurement rather than from my inference.
#
# (An earlier version counted `memory_vectors`, which does not exist. The query
# errored, the count read as zero, and I wrote "persistence is not claimed" on
# the strength of a typo. The name is PGVEC_MEMORY_TABLE in pgvec_transport.h;
# checking the wrong table is indistinguishable from the feature being broken.)
#
# WHAT IT DOES NOT ESTABLISH: retrieval QUALITY. The stub's vectors are
# deterministic but carry no meaning, so any judgement about ranking or recall
# relevance from this run would be worthless.
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
# PGVEC_MEMORY_TABLE (pgvec_transport.h). NOT "memory_vectors", which does not
# exist -- see the header comment.
vec_rows() {
  PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared \
    -Atc "select count(*) from memory_embeddings" 2>/dev/null | tail -1
}

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
rows_before="$(vec_rows)"
echo "  memory_embeddings rows before: ${rows_before:-0}"
store embed-probe-up "The release captain for the deployment runbook is Dana." >/dev/null
sleep 8
rows_after="$(vec_rows)"
echo "  memory_embeddings rows after:  ${rows_after:-0}"
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
# Reported, not asserted -- see the header. The store path did not add a row in
# this window, and whether it should is not settled by this evidence.
echo "NOTE: memory_embeddings ${rows_before:-0} -> ${rows_after:-0} across this store."
if [ "${rows_after:-0}" -eq "${rows_before:-0}" ]; then
  echo "      Unchanged. The rows that exist were produced by this stub (kb_meta"
  echo "      schema_embedder_serving_id), so the write path and vector space are"
  echo "      fine; what is unclear is whether a store is meant to sync one."
fi
if [ "$(( after_down - before_down ))" -eq 0 ]; then
  echo "PASS: nothing reached it once down, so leg 2's calls were real"
else
  echo "NOTE: requests continued after it was stopped, so leg 2 is not solely"
  echo "      attributable to the store."
  rc=1
fi
exit $rc
