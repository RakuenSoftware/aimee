#!/bin/bash
# Gap 1 through the REAL topology: aimee-server -> kb, over the two transports a
# caller can actually reach the server on.
#
#   UDS  /root/aimee-http.sock  SO_PEERCRED attests the local uid -> a person
#   TCP  127.0.0.1:8740 + bearer  a shared token -> a service or an agent
#
# The model never gets its own connection: its tool calls arrive over whichever
# transport the agent process is using, which is the bearer. So the TCP row is
# the one that matters for "can an ordinary model-authored request delete a fact
# the user stated".
#
# Each transport is run against BOTH a Class-A row (the escalation attempt) and a
# Class-B row (the control), so a refusal can be told apart from a broken route.
# Run AS ROOT in the container.
set -u
SB="$(cat /root/server-bearer.txt)"
P=/root/psql.sh

# A retired fact is lifecycle_state='invalidated' + invalidated_at; only a
# supersession sets superseded_at. Judging liveness by superseded_at/suppressed
# alone calls an invalidated row "current", which makes a successful retraction
# read as a blocked one.
state() { $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='$1' and relation='works_for'" | tail -1; }

uds()  { curl -s -m 20 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
              -X POST --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"user\"}" \
              http://localhost/v1/facts/retract; }

tcp()  { curl -s -m 20 -H "Authorization: Bearer ${SB}" -H 'content-type: application/json' \
              -X POST --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"user\"}" \
              http://127.0.0.1:8740/v1/facts/retract; }

bash /root/seed-facts.sh >/dev/null 2>&1
echo "seeded:  alice=$(state alice)   bob=$(state bob)"
echo
echo "=== bearer only, NO account (what an agent presents) ==="
echo "  alice (Class A): $(tcp alice)   -> $(state alice)"
echo "  bob   (Class B): $(tcp bob)   -> $(state bob)"

bash /root/seed-facts.sh >/dev/null 2>&1
echo
echo "=== an authenticated account (PAM host account here) ==="
echo "  alice (Class A): $(uds alice)   -> $(state alice)"
echo "  bob   (Class B): $(uds bob)   -> $(state bob)"
