#!/bin/bash
# Everything the suite needs that a bare deployment does not provide.
#
# WHY THIS EXISTS. deploy-all.sh brings the daemons up; it does not enable the
# /v1 API, mint an mTLS identity, stand up an OIDC issuer, provision the
# management trust chain, or seed the facts the authority probes act on. Those
# were done by hand across a long session on one container, which is precisely
# why "it works" there was a claim about a hand-built box rather than about a
# clean install. Collecting them here makes a fresh bring-up reproducible and,
# more usefully, makes the ORDER explicit -- several of these depend on each
# other and on a daemon restart in between.
#
# Idempotent: safe to re-run on a box that already has some of it.
# Run AS ROOT in the container, after deploy-all.sh.
set -u
export LC_ALL=C
export AIMEE_HOME=/root AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock
A=/usr/local/bin/aimee

step() { printf '\n=== %s ===\n' "$1"; }

step "1. /v1 API bearer"
# The probes present this as x-api-key alongside an identity token, so without it
# every network leg stops at the bearer wall and proves nothing.
if [ ! -s /root/api-bearer.txt ]; then
  "$A" api enable 2>/dev/null | grep -oE '[0-9a-f]{64}' | head -1 > /root/api-bearer.txt
fi
B="$(cat /root/api-bearer.txt 2>/dev/null)"
[ -n "$B" ] || { echo "FAIL: could not obtain an API bearer" >&2; exit 1; }
echo "  bearer: ${B:0:12}... (${#B} chars)"

step "2. management trust chain (identity tokens over TCP)"
bash /root/provision-mgmt-trust.sh 2>&1 | grep -E "kid:|server id:|team id:" | sed 's/^/  /'

step "3. restart the server so the API listener and trust env apply"
bash /root/start-server.sh >/dev/null 2>&1
bash /root/imms.sh >/dev/null 2>&1
sleep 2

step "4. mTLS: server identity, rogue CA, and the client CA"
bash /root/make-mtls-certs.sh 2>&1 | tail -4 | sed 's/^/  /'
bash /root/configure-mtls.sh 2>&1 | grep -E "mtls_client_ca" | sed 's/^/  /'

step "5. restart so TLS comes up, then enrol the first user"
bash /root/set-mtls-mode.sh optional >/dev/null 2>&1
bash /root/start-server.sh >/dev/null 2>&1
bash /root/imms.sh >/dev/null 2>&1
sleep 3
bash /root/enroll-first-user.sh 2>&1 | tail -3 | sed 's/^/  /'

step "6. OIDC issuer (a keypair, a JWKS document and a signer)"
bash /root/make-oidc-idp.sh 2>&1 | grep -E "issuer:|subject:|jwks:" | sed 's/^/  /'
bash /root/start-kb.sh >/dev/null 2>&1
bash /root/smm.sh >/dev/null 2>&1
bash /root/install-postgres-module.sh >/dev/null 2>&1
sleep 2

step "7. chat provider and capture proxy (the live-model probes)"
bash /root/icp.sh >/dev/null 2>&1
bash /root/slp.sh 2>&1 | grep -E "proxy /v1/models|endpoint" | sed 's/^/  /'

step "8. seed the facts the authority probes act on"
bash /root/seed-facts.sh 2>&1 | tail -3 | sed 's/^/  /'

step "ready"
echo "  run: bash /root/run-suite.sh \$(cat /root/api-bearer.txt)"
