#!/bin/bash
# Gap 2's prerequisite: what a stored note records about WHOSE WORDS it is.
#
# memories.provenance_category is what the typed-fact drain reads later to decide
# whether facts mined from a note may enter at Class A. It used to default to
# 'user_stated' for every row, including notes the model wrote — so anything the
# agent chose to remember became a source of permanent facts outranking the
# user's own. It is now written explicitly on every insert, from the caller's
# authenticated identity, and the column default fails closed.
#
# Usage: test-provenance.sh <container-ip>
# Run AS ROOT in the container.
set -u
IP="${1:-}"
B="$(cat /root/kb-bearer.txt)"
P=/root/psql.sh

store() { # $1=host  $2=key  $3=authority-in-body
  curl -s -m 20 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data "{\"key\":\"$2\",\"content\":\"my email is theo@example.com\",\"tier\":\"L2\",\"kind\":\"fact\",\"authority\":\"$3\"}" \
       "http://$1:8741/v1/actions/memory.store" >/dev/null
}

prov() { $P "select provenance_category from memories where key='$1' order by id desc limit 1"; }

$P "delete from memories where key like 'e2e-prov-%'" >/dev/null

echo "=== what each caller's note records as its provenance ==="
store 127.0.0.1 e2e-prov-loopback-user user
echo "  loopback + owner bearer, asking \"user\":  $(prov e2e-prov-loopback-user)"

store 127.0.0.1 e2e-prov-loopback-plain ""
echo "  loopback, asking nothing:                 $(prov e2e-prov-loopback-plain)"

if [ -n "$IP" ]; then
  store "$IP" e2e-prov-remote-user user
  echo "  REMOTE peer, asking \"user\":               $(prov e2e-prov-remote-user)"
fi

echo
echo "=== and the column's own default, for a writer that never says ==="
$P "insert into memories (tier, kind, key, content) values ('L2','fact','e2e-prov-default','x')" >/dev/null
echo "  no provenance supplied at all:            $(prov e2e-prov-default)"
