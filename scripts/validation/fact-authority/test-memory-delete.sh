#!/bin/bash
# memory.delete: does the row get DESTROYED, or retired?
#
# CAP_MEMORY_ADMIN decides whether the caller may delete at all. It does not
# decide whether the caller is a person, and only a person's delete destroys —
# the capability sits inside CAPS_AUTHENTICATED, so a bearer clears it.
# Destroying a row and its provenance is irreversible and the audit event carries
# only the id.
#
# NOTE ON COVERAGE. Only the attested (UDS) half is observable here. Over TCP the
# request is refused several layers ABOVE the authority decision, by the per-user
# write-tier grant gate, and reaching it needs a KB-signed identity token this
# container has no tenancy to issue. So the TCP row below shows the OUTER gate
# refusing, not the retire path — the retire path is covered exhaustively by the
# unit test (test_mcp_memory_gate.c, all six attestation values, with a negative
# control), and it becomes load-bearing here the moment an operator issues a
# grant. Do not read the TCP line as evidence of a retire.
# Run AS ROOT in the container.
set -u
SB="$(cat /root/server-bearer.txt)"
P=/root/psql.sh

mk() { # $1 = key -> prints the new memory id
  curl -s -m 20 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
       -X POST --data "{\"key\":\"$1\",\"content\":\"delete probe\",\"tier\":\"L2\",\"kind\":\"fact\"}" \
       http://localhost/v1/memory/store | sed 's/.*"id":\([0-9]*\).*/\1/'
}

rows() { $P "select count(*) from memories where id=$1"; }

echo "=== TCP + bearer, no write-tier grant (the default deployment) ==="
ID="$(mk e2e-del-tcp)"
echo "  stored id=${ID}, rows=$(rows "$ID")"
resp="$(curl -s -m 20 -H "Authorization: Bearer ${SB}" -H 'content-type: application/json' \
     -X POST --data "{\"id\":${ID}}" http://127.0.0.1:8740/v1/memory/delete)"
case "$resp" in
  *permission_error*) echo "  refused at the write-tier gate, ABOVE the authority decision" ;;
  *) echo "  response: $resp" ;;
esac
echo "  rows remaining: $(rows "$ID")   <- untouched (never reached the delete)"

echo
echo "=== UDS, kernel-attested local uid (a person at the terminal) ==="
ID2="$(mk e2e-del-uds)"
echo "  stored id=${ID2}, rows=$(rows "$ID2")"
echo "  response: $(curl -s -m 20 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
     -X POST --data "{\"id\":${ID2}}" http://localhost/v1/memory/delete)"
echo "  rows remaining: $(rows "$ID2")   <- destroyed, as an attested person should"
