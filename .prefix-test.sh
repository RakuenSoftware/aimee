#!/bin/sh
# Prove the PREFIXES are actually applied, not merely declared.
#
# Dimension is not enough. Wrong pooling or dropped prefixes still yield
# correctly-shaped, correctly-normalised vectors — that is exactly how nomic once
# served at 0.5823 NDCG@10 instead of the 0.6075 it was selected on, with nothing
# erroring. The observable difference is behavioural:
#
#   nomic  declares query "search_query: " / document "search_document: "
#          -> the same text embedded as query vs document MUST differ
#   bekko  declares empty prefixes
#          -> the same text as query vs document MUST be identical
#
# So this asserts the prefix machinery changes the vector for the model that has
# prefixes, and does not for the model that does not.
set -u
img=$1; emb=$2; expect=$3   # expect: differ | same

out=$(docker run --rm --entrypoint sh "$img" -c "
    EMBEDDER_MODEL=$emb EMBEDDER_PORT=8760 \
      /opt/aimee/embedder-venv/bin/python /opt/aimee/scripts/embedder-server.py >/tmp/e.log 2>&1 &
    for i in \$(seq 1 90); do
      curl -fsS -m 2 http://127.0.0.1:8760/health >/dev/null 2>&1 && break; sleep 2
    done
    curl -fsS -m 30 http://127.0.0.1:8760/health 2>/dev/null > /tmp/h.json
    for t in query document; do
      curl -fsS -m 60 \"http://127.0.0.1:8760/embed?input_type=\$t\" \
        -H 'content-type: application/json' -d '{\"text\":\"retrieval augmented generation\"}' \
        2>/dev/null > /tmp/\$t.json
    done
    python3 - <<'PY'
import json
def vec(p):
    v = json.load(open(p))
    for k in ('embedding','vector','vectors','embeddings','data'):
        if isinstance(v, dict) and k in v: v = v[k]
    while isinstance(v, list) and v and isinstance(v[0], list): v = v[0]
    return v
q, d = vec('/tmp/query.json'), vec('/tmp/document.json')
same = (q == d)
h = json.load(open('/tmp/h.json'))
print('POOLING=%s' % h.get('pooling', '?'))
print('SERVING_ID=%s' % (h.get('serving_id') or '')[:24])
print('VERDICT=%s' % ('same' if same else 'differ'))
PY
" 2>&1 | grep -aE 'POOLING=|SERVING_ID=|VERDICT=')

echo "$out"
got=$(echo "$out" | grep VERDICT= | cut -d= -f2)
if [ "$got" = "$expect" ]; then
  echo "PREFIX-PASS $img ($emb): query vs document -> $got (expected $expect)"
else
  echo "PREFIX-FAIL $img ($emb): query vs document -> ${got:-none} (expected $expect)"
  exit 1
fi
