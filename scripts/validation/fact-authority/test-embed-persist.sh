#!/bin/bash
# Does a stored memory actually get embedded and persisted?
#
# THE ANSWER IS YES, and getting there took two wrong turns worth recording.
#
# First I measured a store on the long-running container, saw memory_embeddings
# unchanged after 40 seconds, and left it as "an open observation". Then I read
# the path -- memory_run_maintenance() calls embed_unembedded_l2() only when it
# promoted something -- and concluded embedding must follow promotion rather than
# the store. Both were wrong, and the second was wrong in the more dangerous way:
# a tidy explanation that fitted the evidence I had.
#
# On a FRESH container a single store takes memory_embeddings from 16 to 38: one
# `memory` row plus its unit rows, exactly as memory_core_scope_embed.c says ("one
# store produced 1 'memory' row and 12 'unit' rows"). The store path embeds
# synchronously.
#
# What was actually happening on the old box: I had earlier fed that embedder
# malformed requests, it answered 400, and those 400s tripped the embedding
# circuit breaker ("embedding dependency unavailable; retry after N ms"). The
# breaker was still open, so embeds were refused -- and a refusal there looks
# exactly like "the store does not embed". The fresh container had no such
# history.
#
# So: measure on a box whose breaker has not been tripped, and keep a control
# that ties the growth to the configured endpoint.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
PORT=8799
DIM=384
rc=0

rows() {
  PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared \
    -Atc "select count(*) from memory_embeddings" 2>/dev/null | tail -1
}
kinds() {
  PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared \
    -Atc "select record_type || '=' || count(*)::text from memory_embeddings group by record_type order by 1" \
    2>/dev/null | tr '\n' ' '
}
store() {
  curl -s -m 25 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
       -X POST --data "{\"key\":\"$1\",\"content\":\"$2\",\"tier\":\"L2\",\"kind\":\"fact\"}" \
       http://localhost/v1/memory/store
}

pkill -f stub-embedder.py 2>/dev/null
sleep 1
setsid nohup python3 /root/stub-embedder.py "$PORT" "$DIM" >/root/stub-embedder.log 2>&1 </dev/null &
sleep 2
curl -s -m 8 "http://127.0.0.1:$PORT/health" | grep -q serving_id || {
  echo "FAIL: stub embedder not answering; nothing below would mean anything" >&2; exit 1; }

# Restarted with the embedder already reachable, which also clears any breaker
# state from a previous run -- the exact condition that produced the original
# wrong conclusion.
export EMBEDDER_URL="http://127.0.0.1:$PORT"
bash /root/start-kb.sh >/dev/null 2>&1
bash /root/smm.sh >/dev/null 2>&1
bash /root/install-postgres-module.sh >/dev/null 2>&1
sleep 4

# UNIQUE PER RUN, and this is the whole reason the probe was re-runnable-broken.
#
# It used to store the fixed key `persist-probe-up` with fixed content. That
# works exactly once. On the second run the same key and the same text are
# already stored, the store is a no-op, no new memory row appears, and therefore
# no new embedding rows appear -- so the probe reported "nothing persisted with
# the embedder reachable" on a system where embedding was working perfectly.
#
# That is a false FAILURE, and it is the more expensive kind: it sent me looking
# for a defect in the embed path, past a kb log full of "embedding HTTP request
# failed" lines that were in fact the leg-2 control doing its job.
TAG="$$-$(date +%s)"
KEY_UP="persist-probe-up-$TAG"
KEY_DOWN="persist-probe-down-$TAG"

echo "=== 1. store with the embedder reachable ==="
echo "  run tag: $TAG (unique, so a re-run is a real store and not a no-op)"
before="$(rows)"
echo "  rows before: ${before:-0}   [$(kinds)]"
store "$KEY_UP" "Persistence probe $TAG: the deployment runbook owner is Priya." >/dev/null
sleep 8
after="$(rows)"
echo "  rows after:  ${after:-0}   [$(kinds)]"

echo
echo "=== 2. control: embedder DOWN, store again ==="
pkill -f stub-embedder.py 2>/dev/null
sleep 1
store "$KEY_DOWN" "A second note $TAG stored while no embedder is reachable." >/dev/null
sleep 8
down="$(rows)"
echo "  rows after:  ${down:-0}"

echo
if [ "${after:-0}" -gt "${before:-0}" ]; then
  echo "PASS: a store embedded and PERSISTED ($((after - before)) rows: memory + units)"
else
  echo "FAIL: nothing persisted with the embedder reachable"
  tail -3 /root/stub-embedder.log 2>/dev/null | sed 's/^/      /'
  grep -aiE "embedding dependency" /root/kb.log 2>/dev/null | tail -2 | sed 's/^/      /'
  rc=1
fi
if [ "${down:-0}" -eq "${after:-0}" ]; then
  echo "PASS: none appeared with the embedder down, so leg 1 was the endpoint"
else
  echo "NOTE: rows grew with no embedder (${after:-0} -> ${down:-0}); leg 1 is not"
  echo "      solely attributable to the configured endpoint."
  rc=1
fi
exit $rc
