#!/bin/bash
# Gap 1 at the kb, positive half: does a caller the kb recognises AS a person
# actually get user authority -- and does that authority come from the
# connection rather than from the body?
#
# This probe reaches the kb's plain listener over loopback with the OWNER
# bearer. kb_memory_request_authority() maps that to FACT_AUTHORITY_USER
# deliberately: an owner bearer presented on loopback is the operator at the
# machine, and that was the chosen policy. So retracting the Class-A row here is
# CORRECT, not an escalation.
#
# An earlier version of this script called that leg "the attack" and expected it
# to be refused. That was wrong once the policy was settled, and it mattered:
# labelled that way, the run reads as a security failure every time the system
# behaves properly.
#
# The escalation case is a caller who is NOT a person -- an agent over TCP --
# and it lives in test-server-retract.sh, which must see the same body refused.
# Neither script is worth much alone: this one shows the derivation grants
# authority to a person, that one shows it withholds authority from everything
# else.
#
#   alice (Class A, authority_rank 30) -- retractable only by a user authority
#   bob   (Class B, authority_rank 10) -- retractable by a model authority too,
#                                         so "nothing happened" cannot be
#                                         mistaken for a broken endpoint
# Run AS ROOT in the container.
set -u
B="$(cat /root/kb-bearer.txt)"
P=/root/psql.sh

# A retired fact is lifecycle_state='invalidated' + invalidated_at; only a
# supersession sets superseded_at. Judging liveness by superseded_at/suppressed
# alone calls an invalidated row "current", which makes a successful retraction
# read as a blocked one.
state() { $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='$1' and relation='works_for'" | tail -1; }

retract() { # $1=source  $2=authority-in-body
  curl -s -m 15 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"$2\"}" \
       http://127.0.0.1:8741/v1/actions/facts.retract
}

echo "=== before ==="
echo "  alice (Class A, user-stated): $(state alice)"
echo "  bob   (Class B, model):       $(state bob)"

echo
echo "=== a caller the kb recognises as a person retracts the Class-A fact ==="
echo "  response: $(retract alice user)"
a_state="$(state alice)"
echo "  alice is now: $a_state"

echo
echo "=== control: the same caller against a model-authored fact ==="
echo "  response: $(retract bob user)"
b_state="$(state bob)"
echo "  bob is now: $b_state"

echo
rc=0
case "$a_state" in
  *gone*) echo "PASS: an attested person retracted the Class-A fact" ;;
  *)      echo "FAIL: a person could not retract a Class-A fact -- the derivation is withholding authority from a real user"; rc=1 ;;
esac
case "$b_state" in
  *gone*) echo "PASS: the control retracted too, so the endpoint is reachable and working" ;;
  *)      echo "FAIL: the control did not retract -- this run proves nothing either way"; rc=1 ;;
esac
echo
echo "NOTE: the refusal half of gap 1 is test-server-retract.sh (agent over TCP)."
exit $rc
