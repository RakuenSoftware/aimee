#!/bin/bash
# Gap 1, end to end: can a caller retract a user-stated Class-A fact by simply
# declaring itself the user?
#
# The kb's plain listener is reached over loopback with the owner bearer, which
# is what aimee-server presents. Two probes:
#   alice (Class A) — the escalation attempt; must survive a model authority
#   bob   (Class B) — the control; a model authority may retract it, so a
#                     "nothing was retracted" result cannot be mistaken for the
#                     endpoint being broken or unreachable.
# Run AS ROOT in the container.
set -u
B="$(cat /root/kb-bearer.txt)"
P=/root/psql.sh

state() { $P "select confidence_class || ' ' || case when superseded_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='$1' and relation='works_for'" | tail -1; }

retract() { # $1=source  $2=authority-in-body
  curl -s -m 15 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"$2\"}" \
       http://127.0.0.1:8741/v1/actions/facts.retract
}

echo "=== before ==="
echo "  alice (Class A, user-stated): $(state alice)"
echo "  bob   (Class B, model):       $(state bob)"

echo
echo "=== the attack: a caller declares itself the user and retracts alice ==="
echo "  response: $(retract alice user)"
echo "  alice is now: $(state alice)"

echo
echo "=== the control: same caller, same authority, against a model-authored fact ==="
echo "  response: $(retract bob user)"
echo "  bob is now: $(state bob)"
