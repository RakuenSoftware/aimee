#!/bin/bash
# Gap 1 across transports: authority must come from the connection, not the body.
#
# Every earlier retraction probe went over the loopback UDS, where the caller IS
# a kernel-attested person and the answer is "retract" either way -- so it could
# not tell a working derivation apart from the old body-trusting one. This runs
# the SAME request over both transports:
#
#   TCP  (127.0.0.1:8740, plaintext) -> ATTEST_TCP_BEARER   -> not a person -> MODEL
#   UDS  (/root/aimee-http.sock)     -> ATTEST_UDS_PEERCRED -> a person     -> USER
#
# Both send "authority":"user" in the body. If the body still decided, TCP would
# retract too. TCP runs FIRST: the UDS leg destroys the fact it targets, so the
# negative control has to happen while the fact is still there.
#
# The TCP leg MUST carry a valid bearer. Without one the request is refused at
# the auth wall and never reaches the authority derivation at all -- the test
# then "passes" while proving only that authentication exists. Mint the bearer
# with `aimee api enable` over the UDS and pass it as $1 or AIMEE_BEARER.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
SOCK=/root/aimee-http.sock
BEARER="${1:-${AIMEE_BEARER:-}}"
[ -n "$BEARER" ] || { echo "FAIL: no bearer -- the TCP leg would be refused at the auth wall and prove nothing" >&2; exit 1; }
q() { PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared -Atc "$1" 2>/dev/null; }

REL=age
live() {
  q "select count(*) from entity_edges where relation='$REL' and superseded_at='' and suppressed=0"
}

# A Class A row on a functional relation is the thing worth protecting; seed one
# so the test does not depend on what earlier runs left behind.
#
# Delete before insert, rather than insert-if-absent: a previous run's retraction
# leaves the row in place (suppressed / superseded, so NOT live), and
# idx_ee_unique_triple is on the triple regardless of state -- so re-inserting the
# same source/relation/target fails the unique index while the "is it live?"
# guard says the seed is needed. The test then reports "nothing to retract" on
# every run after the first.
q "delete from entity_edges where source='user' and relation='$REL'" >/dev/null
# entity_edges has no created_at/updated_at: the typed-fact layer stamps
# asserted_at (and superseded_at on correction). Naming the wrong columns fails
# the insert, and with psql errors suppressed that reads as "nothing to retract".
q "insert into entity_edges (source,relation,target,edge_class,confidence_class,confidence,asserted_at,superseded_at,suppressed)
   values ('user','$REL','52','semantic','A',1.0,'2026-01-01T00:00:00Z','',0)" >/dev/null

before=$(live)
echo "Class A '$REL' edges live before: $before"
[ "${before:-0}" -ge 1 ] || { echo "FAIL: nothing to retract, test is vacuous" >&2; exit 1; }

BODY="{\"source\":\"user\",\"relation\":\"$REL\",\"authority\":\"user\"}"

echo
echo "--- NEGATIVE control: TCP bearer, body claims authority=user ---"
tcp=$(curl -s -m 30 -H "Authorization: Bearer $BEARER" -H 'content-type: application/json' \
        -X POST --data "$BODY" http://127.0.0.1:8740/v1/facts/retract)
echo "response: $tcp"
case "$tcp" in
  *authentication_error*) echo "FAIL: refused at the auth wall -- this leg proves nothing about authority" >&2; exit 1 ;;
esac
mid=$(live)
echo "live after TCP: $mid"

echo
echo "--- POSITIVE control: UDS peercred, same body ---"
uds=$(curl -s -m 30 --unix-socket "$SOCK" -H 'content-type: application/json' \
        -X POST --data "$BODY" http://localhost/v1/facts/retract)
echo "response: $uds"
after=$(live)
echo "live after UDS: $after"

echo
rc=0
if [ "$mid" != "$before" ]; then
  echo "FAIL: the TCP caller retracted a Class A fact by asserting authority in the body"
  rc=1
else
  echo "PASS: TCP caller could not retract by assertion ($before -> $mid)"
fi
if [ "${after:-0}" -lt "${mid:-0}" ]; then
  echo "PASS: UDS caller (a real person) did retract ($mid -> $after)"
else
  echo "FAIL: UDS caller could not retract either -- the negative control above proves nothing"
  rc=1
fi
exit $rc
