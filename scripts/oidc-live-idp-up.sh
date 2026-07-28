#!/bin/bash
# oidc-live-idp-up.sh — stand up a real OIDC provider for run-oidc-login-live.sh.
#
# Keycloak, on https/443, with a realm whose client requires PKCE S256. Shared by the
# local rig and the CI job so both test against the same provider configuration —
# a CI-only variant would be a second thing to keep in step.
#
# WHY 443 AND WHY TLS. kb_oidc_token_url_split refuses a non-https token endpoint and
# refuses an explicit port even :443, because the egress client pins it. And the client
# trusts ONLY an administrator-managed system CA bundle — it deliberately ignores
# SSL_CERT_FILE — so this generates a CA and prints where it is; the caller installs it
# into the trust store of wherever the driver runs. That is what a deployment with an
# internal IdP does.
#
# Usage: oidc-live-idp-up.sh [work-dir]
# Prints CA_FILE=<path> on success. Idempotent: replaces any previous container/realm.
set -euo pipefail
export LC_ALL=C

IDP_HOST=idp.aimee.test
REALM=aimee
CLIENT=aimee-kb
CLIENT_SECRET=kb-test-secret
LOGIN_USER=alice
LOGIN_PASS=alice-pw
REDIRECT=https://kb.aimee.test/v1/identity/login/callback
IMAGE=quay.io/keycloak/keycloak:26.0

W=${1:-/tmp/aimee-oidc-idp}
rm -rf "$W"; mkdir -p "$W"; cd "$W"

step() { printf '\n== %s\n' "$*" >&2; }
K() { curl -sk --resolve "$IDP_HOST:443:127.0.0.1" "$@"; }

step "A local CA and a server certificate for $IDP_HOST"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 2 \
  -subj "/CN=aimee-test-idp-ca" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout idp.key -out idp.csr \
  -subj "/CN=$IDP_HOST" >/dev/null 2>&1
printf 'subjectAltName = DNS:%s\nextendedKeyUsage = serverAuth\n' "$IDP_HOST" > ext.cnf
openssl x509 -req -in idp.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out idp.crt -days 2 -extfile ext.cnf >/dev/null 2>&1
chmod 644 idp.key idp.crt

step "Keycloak on https/443"
docker rm -f aimee-oidc-idp >/dev/null 2>&1 || true
for attempt in 1 2 3 4 5; do
  docker pull "$IMAGE" >/dev/null 2>&1 && break
  echo "docker pull $IMAGE failed (attempt $attempt/5); backing off" >&2
  sleep $((attempt * 10))
done
# Keycloak runs as uid 1000 and cannot bind 443 itself, so it listens on 8443 in its
# own namespace and docker publishes host 443 -> 8443. What has to be true is that kb
# reaches it on 443; who binds it inside the container is irrelevant.
docker run -d --name aimee-oidc-idp -p 443:8443 \
  -e KC_BOOTSTRAP_ADMIN_USERNAME=admin -e KC_BOOTSTRAP_ADMIN_PASSWORD=admin \
  -e "KC_HOSTNAME=https://$IDP_HOST" -e KC_HTTPS_PORT=8443 \
  -e KC_HTTPS_CERTIFICATE_FILE=/tls/idp.crt -e KC_HTTPS_CERTIFICATE_KEY_FILE=/tls/idp.key \
  -v "$W:/tls:ro" "$IMAGE" start --optimized=false >/dev/null

ready=0
for _ in $(seq 1 120); do
  if K "https://$IDP_HOST/realms/master/.well-known/openid-configuration" >/dev/null 2>&1; then
    ready=1; break; fi
  sleep 2
done
if [ "$ready" != "1" ]; then
  echo "oidc-live-idp-up: Keycloak did not become ready" >&2
  docker logs aimee-oidc-idp 2>&1 | tail -20 >&2
  exit 3
fi

step "Realm, a confidential client with PKCE S256 REQUIRED, and a user"
TOK=$(K -d client_id=admin-cli -d username=admin -d password=admin -d grant_type=password \
      "https://$IDP_HOST/realms/master/protocol/openid-connect/token" \
      | python3 -c 'import sys,json;print(json.load(sys.stdin)["access_token"])')
A=(-H "Authorization: Bearer $TOK" -H "Content-Type: application/json")

K "${A[@]}" -X DELETE "https://$IDP_HOST/admin/realms/$REALM" >/dev/null 2>&1 || true
K "${A[@]}" -X POST "https://$IDP_HOST/admin/realms" \
  -d "{\"realm\":\"$REALM\",\"enabled\":true}" >/dev/null

# PKCE S256 is REQUIRED, not merely permitted, so the IdP genuinely enforces the
# verifier — which is the only way a wrong verifier can be shown to be refused.
K "${A[@]}" -X POST "https://$IDP_HOST/admin/realms/$REALM/clients" -d "{
  \"clientId\":\"$CLIENT\",\"enabled\":true,\"protocol\":\"openid-connect\",
  \"publicClient\":false,\"secret\":\"$CLIENT_SECRET\",
  \"standardFlowEnabled\":true,\"directAccessGrantsEnabled\":false,
  \"redirectUris\":[\"$REDIRECT\"],
  \"attributes\":{\"pkce.code.challenge.method\":\"S256\"}}" >/dev/null

# A COMPLETE profile. Without an email and name Keycloak interposes its VERIFY_PROFILE
# required action between the login form and the redirect, and no code is issued —
# a fixture problem that looks exactly like a broken relying party.
K "${A[@]}" -X POST "https://$IDP_HOST/admin/realms/$REALM/users" -d "{
  \"username\":\"$LOGIN_USER\",\"enabled\":true,
  \"email\":\"$LOGIN_USER@aimee.test\",\"emailVerified\":true,
  \"firstName\":\"Alice\",\"lastName\":\"Tester\",\"requiredActions\":[],
  \"credentials\":[{\"type\":\"password\",\"value\":\"$LOGIN_PASS\",\"temporary\":false}]}" >/dev/null

CID=$(K "${A[@]}" "https://$IDP_HOST/admin/realms/$REALM/clients?clientId=$CLIENT" \
      | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["id"])')
pkce=$(K "${A[@]}" "https://$IDP_HOST/admin/realms/$REALM/clients/$CID" \
       | python3 -c 'import sys,json;print(json.load(sys.stdin)["attributes"].get("pkce.code.challenge.method"))')
[ "$pkce" = "S256" ] || { echo "oidc-live-idp-up: PKCE S256 not set on the client" >&2; exit 3; }
echo "realm=$REALM client=$CLIENT user=$LOGIN_USER pkce=$pkce" >&2

echo "CA_FILE=$W/ca.crt"
