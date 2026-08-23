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
# A retired fact is lifecycle_state='invalidated' + invalidated_at; only a
# supersession sets superseded_at. Judging liveness by superseded_at/suppressed
# alone calls an invalidated row "current", which makes a successful retraction
# read as a blocked one.
  q "select count(*) from entity_edges where relation='$REL' and superseded_at='' and invalidated_at='' and suppressed=0"
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
# entity_edges carries MORE THAN ONE write guard -- entity_edges_semantic_guard
# and semantic_evidence_event_guard ("semantic assertion mutation committed
# without its evidence event"). Naming one leaves the other, so this suspends
# every user trigger on the table for the seed. They refuse raw INSERT/DELETE of a semantic edge
# outside an open fact_mutation commit, so the seed silently does nothing
# without this -- and the test then reports "nothing to retract".
q "ALTER TABLE entity_edges DISABLE TRIGGER USER" >/dev/null
q "delete from entity_edges where source='user' and relation='$REL'" >/dev/null
# entity_edges has no created_at/updated_at: the typed-fact layer stamps
# asserted_at (and superseded_at on correction). Naming the wrong columns fails
# the insert, and with psql errors suppressed that reads as "nothing to retract".
# authority_rank 30 (FACT_ACTOR_USER) is what db2_fact_mutation_invalidate
# gates on -- confidence_class 'A' alone leaves a row anyone may retract, so the
# test would protect nothing while looking correct.
q "insert into entity_edges (source,relation,target,edge_class,confidence_class,confidence,
     authority_rank,lifecycle_state,asserted_at,invalidated_at,superseded_at,suppressed)
   values ('user','$REL','52','semantic','A',1.0,30,'persistent','2026-01-01T00:00:00Z','','',0)" >/dev/null
q "ALTER TABLE entity_edges ENABLE TRIGGER USER" >/dev/null

before=$(live)
echo "Class A '$REL' edges live before: $before"
[ "${before:-0}" -ge 1 ] || { echo "FAIL: nothing to retract, test is vacuous" >&2; exit 1; }

BODY="{\"source\":\"user\",\"relation\":\"$REL\",\"authority\":\"user\"}"

echo
echo "--- NEGATIVE control: TCP bearer, body claims authority=user ---"
tcp=$(curl -s -m 30 -H "Authorization: Bearer $BEARER" -H 'content-type: application/json' \
        -X POST --data "$BODY" http://127.0.0.1:8740/v1/facts/retract)
echo "response: $tcp"

# WHICH WALL STOPPED IT, and why that question decides what this probe may claim.
#
# "It did not retract" is not evidence about authority unless the request
# actually REACHED the authority derivation. Over TCP there are two walls in
# front of it, and this probe used to print
# "PASS: TCP caller could not retract by assertion" no matter which one fired:
#
#   mtls    -- with aimee.api.mtls at optional or required, a caller presenting
#              no client certificate is capped (CAPS_READ_ONLY, or refused
#              outright) by server_http_effective_conn_caps BEFORE the route
#              gate. The mode is global, so this probe's result depended on
#              whichever posture a DIFFERENT probe happened to leave behind --
#              it failed on the first run after prepare-suite and passed on
#              every run after test-mtls-authority.sh had set it back to off.
#
#   write-tier -- with mtls off, the per-subject tier reaches the route gate. A
#              plain API bearer carries NO subject, so it can never hold a
#              write-tier grant, and the gate refuses it permanently.
#
# The second one is structural: this leg CANNOT reach the authority decision
# with a plain bearer, so a green line here never meant what it said. The claim
# is corrected rather than the wall removed -- gap 1 over TCP is proven by
# test-account-tcp-authority.sh, which presents a KB-issued identity token whose
# subject holds a write-tier grant, clears both walls, and then shows that a
# bearer with NO account retracts nothing while an account retracts a Class A
# fact.
case "$tcp" in
  *'client certificate is required'*|*'mtls'*)
    wall="mtls" ;;
  *authentication_error*)
    wall="auth" ;;
  *permission_error*|*'write-tier'*)
    wall="write-tier" ;;
  *)
    wall="none" ;;
esac
echo "TCP refusal wall: $wall"
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
  case "$wall" in
    none)
      # Reached the route and was still refused: the authority derivation is
      # what stopped it, which is the thing this probe set out to show.
      echo "PASS: the TCP caller reached the authority decision and was still"
      echo "      refused, so the body's authority claim was ignored ($before -> $mid)" ;;
    write-tier|mtls|auth)
      echo "PASS (NARROW): the TCP caller did not retract ($before -> $mid), but it"
      echo "      was stopped at the $wall wall IN FRONT OF the authority derivation,"
      echo "      so this leg shows defence in depth and NOT that authority is"
      echo "      derived from the connection. That claim belongs to"
      echo "      test-account-tcp-authority.sh, which clears both walls first." ;;
  esac
fi
if [ "${after:-0}" -lt "${mid:-0}" ]; then
  echo "PASS: UDS caller (a real person) did retract ($mid -> $after)"
else
  echo "FAIL: UDS caller could not retract either -- the negative control above proves nothing"
  rc=1
fi
exit $rc
