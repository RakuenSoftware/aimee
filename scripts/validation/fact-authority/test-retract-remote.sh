#!/bin/bash
# Gap 1, the network case: the same owner bearer, presented from a NON-loopback
# peer, must not carry user authority. Loopback is what makes the bearer stand
# for aimee-server-on-this-host; from anywhere else it is just a shared token.
#
# Run from the Proxmox host (or any machine that is not the container), with the
# container's address and bearer passed in:
#   test-retract-remote.sh <container-ip> <bearer>
set -u
IP="$1"
B="$2"

echo "=== retract alice (Class A, user-stated) from a remote peer, claiming user authority ==="
curl -s -m 15 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
     -X POST --data '{"source":"alice","relation":"works_for","authority":"user"}' \
     "http://${IP}:8741/v1/actions/facts.retract"
echo
echo "=== control: retract bob (Class B, model-authored) from the same remote peer ==="
curl -s -m 15 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
     -X POST --data '{"source":"bob","relation":"works_for","authority":"user"}' \
     "http://${IP}:8741/v1/actions/facts.retract"
echo
