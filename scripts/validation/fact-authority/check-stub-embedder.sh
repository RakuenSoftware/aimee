#!/bin/bash
# Is the stub embedder actually answering the two shapes the kb calls?
#
# This exists because a failing EMBED probe was nearly blamed on the product.
# The kb logged "embedding HTTP request failed" while the stub's own access log
# showed `POST /embed 200`, which reads like the kb rejecting a good answer. A
# direct curl then returned an empty body -- but that measurement was taken
# after test-embed-persist.sh had already killed the stub in its leg-2 control,
# so it was measuring a closed port, not a bad reply.
#
# So: start the stub, prove BOTH endpoints, and print the byte counts. A probe
# that cannot tell "wrong reply" from "nothing listening" cannot diagnose
# anything, and both of those were reached in the space of one run.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
PORT="${1:-8799}"
DIM="${2:-384}"
rc=0

pkill -f stub-embedder.py 2>/dev/null
sleep 1
setsid nohup python3 /root/stub-embedder.py "$PORT" "$DIM" >/root/stub-embedder.log 2>&1 </dev/null &
sleep 3

echo "=== GET /health ==="
h="$(curl -s -m 8 "http://127.0.0.1:$PORT/health")"
echo "  $h"
case "$h" in
  *serving_id*) echo "  PASS: health names a serving_id" ;;
  *) echo "  FAIL: no serving_id -- the kb will not consider this an embedder"; rc=1 ;;
esac

echo
echo "=== POST /embed (RAW TEXT body, single vector back) ==="
# The shape that matters: memory_core_scope_embed.c posts the raw text and
# parses a bare JSON array of numbers. Anything else trips the breaker, and the
# breaker's message names the DEPENDENCY, not the payload -- so a malformed
# reply here looks exactly like an unreachable service.
b="$(curl -s -m 8 -X POST --data 'hello world' "http://127.0.0.1:$PORT/embed?input_type=document")"
n="$(printf '%s' "$b" | wc -c)"
echo "  bytes: $n"
echo "  head:  $(printf '%s' "$b" | head -c 70)"
dims="$(printf '%s' "$b" | python3 -c '
import json,sys
try:
    v = json.load(sys.stdin)
except Exception:
    print(-1); raise SystemExit
print(len(v) if isinstance(v, list) and all(isinstance(x,(int,float)) for x in v) else -1)')"
if [ "${dims:-0}" = "$DIM" ]; then
  echo "  PASS: a bare JSON array of $dims floats, which is what the kb parses"
else
  echo "  FAIL: not a bare JSON float array of $DIM (got '$dims')"
  rc=1
fi

echo
echo "=== POST /embed_batch (JSON array of strings, list of vectors back) ==="
b2="$(curl -s -m 8 -X POST -H 'content-type: application/json' \
      --data '["one","two"]' "http://127.0.0.1:$PORT/embed_batch?input_type=document")"
rows="$(printf '%s' "$b2" | python3 -c '
import json,sys
try:
    v = json.load(sys.stdin)
except Exception:
    print(-1); raise SystemExit
print(len(v) if isinstance(v, list) and all(isinstance(x, list) for x in v) else -1)')"
echo "  vectors returned: $rows"
if [ "${rows:-0}" = "2" ]; then
  echo "  PASS: one vector per input string"
else
  echo "  FAIL: expected 2 vectors, got '$rows'"
  rc=1
fi

echo
if [ "$rc" = "0" ]; then
  echo "PASS: the stub answers both shapes; an EMBED failure after this is the kb's"
else
  echo "FAIL: the stub is wrong, so any EMBED result would be about the stub"
  tail -5 /root/stub-embedder.log 2>/dev/null | sed 's/^/      /'
fi
exit $rc
