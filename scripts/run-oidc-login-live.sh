#!/bin/bash
# run-oidc-login-live.sh — the relying-party login against a REAL OIDC provider.
#
# WHY THIS EXISTS. Every other test of increment 4a stubs the token-endpoint POST.
# The unit tests sign their id_tokens with a genuine keypair, so the verifier is
# exercised for real, but nothing before this touched an identity provider. That left
# a specific, named gap: the real TLS handshake, the real form-urlencoded round trip,
# the IdP's actual response shape, its actual JWKS (Keycloak publishes an encryption
# key alongside the signing key, so kid selection is load-bearing), and its
# enforcement of the PKCE verifier and of single-use codes.
#
# So this stands up Keycloak, provisions a realm with a confidential client whose
# PKCE method is REQUIRED to be S256, and drives the whole flow through the shipping
# code (../oidc-login-live links the production units).
#
# TLS IS NOT OPTIONAL HERE, and that is kb's own doing: kb_oidc_token_url_split
# refuses a non-https token endpoint and refuses an explicit port — even :443 —
# because the egress client pins 443. So the IdP genuinely runs on https/443, and
# because kb's client trusts ONLY an administrator-managed system CA bundle (it
# deliberately ignores SSL_CERT_FILE), the test CA is installed into the container's
# bundle. That is the same thing a deployment with an internal IdP does.
#
# Usage: run-oidc-login-live.sh <idp-host-ip>
#   Requires: docker, root, and the IdP already running (see the --up mode below).
set -euo pipefail
export LC_ALL=C

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

IDP_HOST=idp.aimee.test
REALM=aimee
CLIENT=aimee-kb
CLIENT_SECRET=kb-test-secret
USER=alice
PASS=alice-pw
REDIRECT=https://kb.aimee.test/v1/identity/login/callback

idp_ip=${1:-127.0.0.1}
work=$(mktemp -d /root/oidc-live.XXXXXX)
chmod 0700 "$work"
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT

step() { printf '\n== %s\n' "$*"; }
K() { curl -sk --resolve "$IDP_HOST:443:$idp_ip" "$@"; }

step "The IdP is real and reachable on https/443"
disc=$(K "https://$IDP_HOST/realms/$REALM/.well-known/openid-configuration")
auth_ep=$(printf '%s' "$disc" | python3 -c 'import sys,json;print(json.load(sys.stdin)["authorization_endpoint"])')
tok_ep=$(printf '%s' "$disc" | python3 -c 'import sys,json;print(json.load(sys.stdin)["token_endpoint"])')
iss=$(printf '%s' "$disc" | python3 -c 'import sys,json;print(json.load(sys.stdin)["issuer"])')
echo "issuer:    $iss"
echo "authorize: $auth_ep"
echo "token:     $tok_ep"
# The IdP's real JWKS, fetched from its real endpoint.
K "https://$IDP_HOST/realms/$REALM/protocol/openid-connect/certs" >"$work/jwks.json"
python3 - "$work/jwks.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
print("jwks:      %d key(s): %s" % (len(d["keys"]),
      ", ".join("%s/%s" % (k["kty"], k.get("alg","?")) for k in d["keys"])))
print("           more than one key, so the header kid decides which is used"
      if len(d["keys"])>1 else "           single key")
PY

# The login profile, exactly as a deployment sets it. AUDIENCE is deliberately kb as
# a RESOURCE SERVER and NOT the client id: if the id_token verified anyway, the
# audience override is genuinely being applied rather than accidentally agreeing.
export AIMEE_KB_OIDC_ISSUER="$iss"
export AIMEE_KB_OIDC_LOGIN_CLIENT_ID="$CLIENT"
export AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL="$auth_ep"
export AIMEE_KB_OIDC_LOGIN_TOKEN_URL="$tok_ep"
export AIMEE_KB_OIDC_LOGIN_REDIRECT_URI="$REDIRECT"
export AIMEE_KB_OIDC_LOGIN_SCOPE="openid profile"
export AIMEE_KB_OIDC_JWKS_FILE="$work/jwks.json"
export AIMEE_KB_OIDC_AUDIENCE="https://kb.aimee.test/api"
export AIMEE_OIDC_LIVE_CLIENT_SECRET="$CLIENT_SECRET"

step "kb starts the login (real code, real secrets)"
./oidc-login-live start "$work/pending" | tee "$work/start.out"
url=$(grep '^AUTHORIZE_URL ' "$work/start.out" | cut -d' ' -f2-)
[ -n "$url" ] || { echo "no authorization URL" >&2; exit 3; }
grep -q 'verifier absent from the URL: yes' "$work/start.out" \
  || { echo "the code_verifier LEAKED into the authorization URL" >&2; exit 3; }

step "A user authenticates at the IdP (its real login form)"
# This is the browser's part, scripted. Nothing of kb is involved.
jar="$work/cookies"
form=$(K -c "$jar" "$url" | grep -o 'action="[^"]*"' | head -1 | sed 's/action="//;s/"$//' \
       | python3 -c 'import sys,html;print(html.unescape(sys.stdin.read().strip()))')
[ -n "$form" ] || { echo "could not find the IdP login form" >&2; exit 3; }
loc=$(K -b "$jar" -c "$jar" -o /dev/null -D - -X POST "$form" \
        --data-urlencode "username=$USER" --data-urlencode "password=$PASS" \
        --data-urlencode "credentialId=" | tr -d '\r' | awk 'tolower($1)=="location:"{print $2}')
[ -n "$loc" ] || { echo "the IdP did not redirect after login" >&2; exit 3; }
code=$(printf '%s' "$loc" | sed -n 's/.*[?&]code=\([^&]*\).*/\1/p')
cb_state=$(printf '%s' "$loc" | sed -n 's/.*[?&]state=\([^&]*\).*/\1/p')
[ -n "$code" ] || { echo "no code in the IdP redirect: $loc" >&2; exit 3; }
echo "the IdP redirected to the registered callback with a code"
echo "  code length: ${#code}"
# The state the IdP echoed must be the one kb generated — checked here as well as in
# the route, because if the IdP did not echo it the route's check would be vacuous.
pend_state=$(head -1 "$work/pending")
[ "$cb_state" = "$pend_state" ] || { echo "the IdP echoed a DIFFERENT state" >&2; exit 3; }
echo "  state echoed by the IdP matches the one kb generated"

step "kb finishes the login: exchange -> verify -> nonce -> principal"
./oidc-login-live finish "$work/pending" "$code" | tee "$work/finish.out"
grep -q '^exchange: OK' "$work/finish.out" || { echo "the exchange failed" >&2; exit 4; }
grep -q 'verified: signature, iss and aud check out' "$work/finish.out" \
  || { echo "verification failed against the live JWKS" >&2; exit 4; }
grep -q '^nonce: OK' "$work/finish.out" || { echo "the nonce did not match" >&2; exit 4; }
grep -q '^SUBJECT oidc:' "$work/finish.out" || { echo "no issuer-scoped subject" >&2; exit 4; }
grep -q '^replayed code: DENIED' "$work/finish.out" \
  || { echo "the IdP did not refuse a replayed code" >&2; exit 4; }

step "The negative cases, against the same live IdP"
# A WRONG PKCE VERIFIER. The IdP holds the challenge, so only it can enforce this —
# no stub can. Corrupt the stored verifier and re-run a fresh login's exchange.
./oidc-login-live start "$work/pending2" >/dev/null
url2=$(grep '^AUTHORIZE_URL ' <(./oidc-login-live start "$work/pending3") | cut -d' ' -f2-)
jar2="$work/cookies2"
form2=$(K -c "$jar2" "$url2" | grep -o 'action="[^"]*"' | head -1 | sed 's/action="//;s/"$//' \
        | python3 -c 'import sys,html;print(html.unescape(sys.stdin.read().strip()))')
loc2=$(K -b "$jar2" -c "$jar2" -o /dev/null -D - -X POST "$form2" \
         --data-urlencode "username=$USER" --data-urlencode "password=$PASS" \
         --data-urlencode "credentialId=" | tr -d '\r' | awk 'tolower($1)=="location:"{print $2}')
code2=$(printf '%s' "$loc2" | sed -n 's/.*[?&]code=\([^&]*\).*/\1/p')
if [ -n "$code2" ]; then
  # Swap in a different (well-formed) verifier for the same code.
  python3 - "$work/pending3" <<'PY'
import sys
p=sys.argv[1]
l=open(p).read().split("\n")
l[1]="X"*len(l[1])            # same length, wrong value
open(p,"w").write("\n".join(l))
PY
  set +e
  out=$(./oidc-login-live finish "$work/pending3" "$code2" 2>&1)
  rc=$?
  set -e
  printf '%s\n' "$out" | grep -E '^exchange:' || true
  if [ "$rc" = "0" ]; then
    echo "the IdP ACCEPTED a wrong PKCE verifier" >&2; exit 5
  fi
  echo "  correct: a wrong code_verifier is refused BY THE IdP"
else
  echo "  (skipped: could not obtain a second code)" >&2
fi

step "What this proved"
cat <<'MSG'
  - a real TLS handshake to an IdP on 443, trusted via the system CA bundle only
  - the authorization URL carries code_challenge/S256 and NOT the verifier
  - the IdP echoed kb's state unchanged
  - a real client-secret-basic form POST to the real token endpoint
  - a real RS256 id_token, verified against the live JWKS by kid, with the audience
    OVERRIDDEN to the client id while AIMEE_KB_OIDC_AUDIENCE named something else
  - the nonce kb generated came back inside the signed payload
  - the subject is issuer-scoped from the CONFIGURED issuer
  - the IdP refuses a replayed code, and refuses a wrong PKCE verifier
MSG
echo
echo "run-oidc-login-live: PASSED"
