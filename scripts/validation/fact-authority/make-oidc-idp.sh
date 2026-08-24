#!/bin/bash
# Stand up an OIDC issuer for the kb, so the OIDC account path can be exercised
# for real instead of asserted.
#
# No network IdP is needed: kb verifies an RS256 bearer against a JWKS FILE
# (AIMEE_KB_OIDC_JWKS_FILE), pinning alg=RS256, selecting the key by header
# `kid`, and checking iss / aud / sub / exp / iat (auth_oidc.c). So an issuer is
# a keypair, a JWKS document, and a signer -- all of which openssl can do.
#
# Two keys are minted. The second (rogue) never appears in the JWKS, and its
# token is the negative control: without it, "the OIDC caller got user
# authority" cannot be distinguished from "kb accepts any bearer shaped like a
# JWT", which would be a far worse bug than the one under test.
#
# Usage: make-oidc-idp.sh [SUBJECT]
# Run AS ROOT in the container.
set -u
export LC_ALL=C
DIR=/root/.config/aimee-oidc
ISS="https://idp.aimee-e2e.test"
AUD="aimee-kb"
SUB="${1:-alice@example.com}"
mkdir -p "$DIR"
cd "$DIR" || exit 1

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

# --- keys ---------------------------------------------------------------------
[ -f signer.pem ] || openssl genrsa -out signer.pem 2048 2>/dev/null
[ -f rogue.pem  ] || openssl genrsa -out rogue.pem  2048 2>/dev/null

# --- JWKS: only the signer's PUBLIC key ---------------------------------------
# n comes from the modulus in hex; e is 65537 for an openssl RSA key, which is
# AQAB. Both are base64url of the raw big-endian bytes, per RFC 7518.
MOD="$(openssl rsa -in signer.pem -noout -modulus 2>/dev/null | sed 's/^Modulus=//')"
# hex -> raw bytes -> base64url. Not `xxd`: it is not installed on a minimal
# container image, and its absence left `n` EMPTY, which produced a
# syntactically valid JWKS that could never verify anything.
N="$(printf '%s' "$MOD" | python3 -c '
import sys, base64, binascii
h = sys.stdin.read().strip()
sys.stdout.write(base64.urlsafe_b64encode(binascii.unhexlify(h)).decode().rstrip("="))
')"
[ -n "$N" ] || { echo "FAIL: could not derive the JWKS modulus" >&2; exit 1; }
KID="aimee-e2e-key-1"
cat > jwks.json <<EOF
{"keys":[{"kty":"RSA","use":"sig","alg":"RS256","kid":"$KID","n":"$N","e":"AQAB"}]}
EOF

# --- mint a token -------------------------------------------------------------
mint() { # $1 = signing key, $2 = kid, $3 = out file
  local now exp hdr pl signing sig
  now="$(date +%s)"
  exp="$(( now + 3600 ))"
  hdr="$(printf '{"alg":"RS256","typ":"JWT","kid":"%s"}' "$2" | b64url)"
  pl="$(printf '{"iss":"%s","aud":"%s","sub":"%s","iat":%s,"exp":%s}' \
        "$ISS" "$AUD" "$SUB" "$now" "$exp" | b64url)"
  signing="${hdr}.${pl}"
  sig="$(printf '%s' "$signing" | openssl dgst -sha256 -sign "$1" -binary | b64url)"
  printf '%s.%s' "$signing" "$sig" > "$3"
}

mint signer.pem "$KID"    token.jwt
# Same claims, same kid, signed by a key the JWKS does not contain.
mint rogue.pem  "$KID"    rogue.jwt

cat > env.sh <<EOF
export AIMEE_KB_OIDC_JWKS_FILE=$DIR/jwks.json
export AIMEE_KB_OIDC_ISSUER=$ISS
export AIMEE_KB_OIDC_AUDIENCE=$AUD
EOF

echo "issuer:    $ISS"
echo "audience:  $AUD"
echo "subject:   $SUB"
echo "kid:       $KID"
echo "jwks:      $DIR/jwks.json"
echo "token:     $DIR/token.jwt   (signed by the key IN the jwks)"
echo "rogue:     $DIR/rogue.jwt   (same claims, key NOT in the jwks)"
echo "env:       $DIR/env.sh      (source this before starting aimee-kb)"
echo
echo "--- token payload ---"
cut -d. -f2 token.jwt | tr '_-' '/+' | sed 's/$/==/' | openssl base64 -d -A 2>/dev/null; echo
