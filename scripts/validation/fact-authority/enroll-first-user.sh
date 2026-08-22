#!/bin/bash
# Drive the REAL first-user enrollment, instead of seeding the grant row.
#
# THE FLOW, as the product defines it (remote_client_grant.h: "the setup wizard
# owns the first interactive identity. Its bearer is only an enrollment
# credential; the standing write grant is attached to the mTLS certificate
# produced by that enrollment"):
#
#   1. POST /v1/deploy/apply as a webchat user
#        -> server_http_first_user_bootstrap() claims the first user and returns
#           an ENROLLMENT-ONLY bearer (tier is not yet active)
#   2. generate a keypair + CSR on the client side
#   3. POST /v1/cert/sign with that CSR, presenting the enrollment bearer
#        -> pki_sign_csr() issues the cert AND
#           server_http_first_user_bind_cert() binds its serial to the grant,
#           activating `full` only now, after possession of the client-generated
#           private key has been proved by the CSR
#
# Step 1 needs two things that are not defaults:
#   - AIMEE_DEPLOY_ENABLED, checked by deploy_route_guard
#   - a `webuser:` principal, which vault_principal_resolve grants ONLY for an
#     X-Aimee-Webuser header arriving over the root-owned UDS (peer uid 0). A
#     header asserted over TCP is a spoof and is refused a principal entirely.
#
# The deploy half genuinely tries to run `docker compose up -d` afterwards. That
# fails here and is expected to: the enrollment is claimed BEFORE the deploy
# starts, deliberately, so a stack can never come up without a usable remote
# owner. What matters for enrollment is the bearer in the response.
#
# Usage: enroll-first-user.sh [CN] [WEBUSER]
# Run AS ROOT in the container.
set -u
export LC_ALL=C
SOCK=/root/aimee-http.sock
TLS=/root/.config/aimee-tls
CN="${1:-thin-client-a}"
WEBUSER="${2:-alice}"
mkdir -p "$TLS"

uds() { # $1 = method, $2 = path, $3 = body, $4.. = extra headers
  local m="$1" p="$2" b="$3"; shift 3
  curl -s -m 30 --unix-socket "$SOCK" -X "$m" \
       -H 'content-type: application/json' "$@" \
       ${b:+--data "$b"} "http://localhost$p"
}

echo "=== 1. claim the first user as a webchat user ==="
deploy="$(uds POST /v1/deploy/apply '{}' -H "X-Aimee-Webuser: $WEBUSER")"
echo "  response: $(printf '%s' "$deploy" | tr '\n' ' ' | head -c 240)"

BEARER="$(printf '%s' "$deploy" | python3 -c '
import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
e = d.get("enrollment") or {}
sys.stdout.write(e.get("bearer_token") or e.get("bearer") or "")
')"
if [ -z "$BEARER" ]; then
  echo "FAIL: no enrollment bearer in the response." >&2
  echo "      Without it the next step cannot be the real flow, and falling back" >&2
  echo "      to writing the grant row directly is the shortcut this replaces." >&2
  exit 1
fi
echo "  enrollment bearer: ${BEARER:0:12}... (${#BEARER} chars)"

echo
echo "=== 2. client-side keypair + CSR ==="
openssl req -newkey rsa:2048 -nodes -keyout "$TLS/client.key" -out "$TLS/client.csr" \
  -subj "/CN=$CN" 2>/dev/null
chmod 600 "$TLS/client.key"
CSR="$(cat "$TLS/client.csr")"
echo "  CSR for CN=$CN generated (private key never leaves the client)"

echo
echo "=== 3. sign the CSR with the enrollment bearer ==="
BODY="$(python3 -c '
import json,sys
print(json.dumps({"cn": sys.argv[1], "csr": open(sys.argv[2]).read(), "days": 365}))
' "$CN" "$TLS/client.csr")"
signed="$(curl -s -m 30 --unix-socket "$SOCK" -X POST \
          -H 'content-type: application/json' \
          -H "Authorization: Bearer $BEARER" \
          --data "$BODY" "http://localhost/v1/cert/sign")"
echo "  response: $(printf '%s' "$signed" | tr '\n' ' ' | head -c 200)"

printf '%s' "$signed" | python3 -c '
import json,sys
d = json.load(sys.stdin)
cert = d.get("cert")
if not cert:
    sys.stderr.write("FAIL: no certificate in the response\n"); sys.exit(1)
open("/root/.config/aimee-tls/client.crt","w").write(cert)
serial = d.get("serial") or ""
open("/root/.config/aimee-tls/client.serial","w").write(serial)
print("  issued serial", serial)
' || exit 1

echo
echo "=== the grant the flow produced (NOT written by hand) ==="
SERIAL="$(cat "$TLS/client.serial")"
sqlite3 -header -column /root/aimee.db \
  "SELECT principal,tier,cert_serial FROM remote_client_grants WHERE cert_serial='$SERIAL'"
n="$(sqlite3 /root/aimee.db "SELECT COUNT(*) FROM remote_client_grants WHERE cert_serial='$SERIAL' AND tier='full'")"
[ "${n:-0}" = "1" ] || { echo "FAIL: enrollment did not activate a full-tier grant for this serial" >&2; exit 1; }
echo "enrolled by the real flow: tier=full bound to $SERIAL"
