#!/bin/bash
# Does an ACCOUNT presented over TCP to aimee-server get USER authority?
#
# This is the last account form that was covered by assertion alone. A KB-issued
# identity token carries a subject; the server verifies it against the management
# trust chain, and server_http_apply_caller_context makes that subject the
# request's caller_subject -- the account every surface below ingress reads.
#
# Under the old transport table this caller was MODEL no matter who it was,
# because its bytes arrived over TCP. The account is the same account whichever
# socket carried it, so it must retract.
#
# HOW A CALLER PRESENTS ONE, which is not obvious: over TCP the server runs TWO
# independent checks on the same request. server_http_authorize demands a
# credential equal to the configured bearer, and server_http_resolve_write_tier
# reads the IDENTITY TOKEN out of `Authorization`. So the identity token goes in
# `Authorization` and the server bearer in `x-api-key`. Putting the identity
# token in Authorization alone gets a 401 from the bearer check before the
# account is ever resolved -- a refusal that proves nothing.
#
# Legs, each with the thing that makes it falsifiable:
#   1. bearer only, NO identity token   -> no account -> must not retract
#   2. identity token for the account   -> account -> must retract the Class-A row
#   3. a token minted for a DIFFERENT audience -> must be refused, so leg 2
#      cannot be read as "the server accepts any token"
#   4. Class-B control                  -> so leg 2 cannot be "it retracts for anyone"
#
# REQUIRES aimee.api.mtls = off, and that is not incidental.
# server_http_effective_conn_caps() gives a caller that presents no client
# certificate CAPS_READ_ONLY whenever mTLS is in optional mode, whatever its
# token says -- "optional-mode bearer fallback is deliberately weaker than a
# client cert". Only with mTLS off does the token's per-user tier reach the
# route gate. Run `set-mtls-mode.sh off` first; test-mtls-authority.sh wants
# `optional` and is the probe about certificates.
#
# Usage: test-account-tcp-authority.sh <SERVER_BEARER>
# Run AS ROOT in the container, after provision-mgmt-trust.sh.
set -u
export LC_ALL=C
RIG=/usr/local/bin/write-tier-enforce-live
TOKEN_KEY=/root/mgmt-token.key
P=/root/psql.sh
SRV=http://127.0.0.1:8740
SUB="${SUB:-alice}"
rc=0

BEARER="${1:-}"
[ -n "$BEARER" ] || { echo "FAIL: no server bearer -- every leg would stop at the bearer wall" >&2; exit 1; }
[ -x "$RIG" ] || { echo "FAIL: $RIG not installed" >&2; exit 1; }
[ -f /root/mgmt-trust-env.sh ] || { echo "FAIL: run provision-mgmt-trust.sh first" >&2; exit 1; }
. /root/mgmt-trust-env.sh

state() {
  $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='$1' and relation='works_for'" | tail -1
}

mint() { # $1 = subject, $2 = audience
  local jti="acct-$$-$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  "$RIG" mint --key "$TOKEN_KEY" --aud "$2" --team "$AIMEE_SERVER_TEAM_ID" \
              --sub "$1" --tier full --jti "$jti"
}

retract() { # $1 = source, $2 = identity token ("" for none)
  if [ -n "$2" ]; then
    curl -s -m 25 -H "Authorization: Bearer $2" -H "x-api-key: $BEARER" \
         -H 'content-type: application/json' -X POST \
         --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"user\"}" \
         "$SRV/v1/facts/retract"
  else
    curl -s -m 25 -H "x-api-key: $BEARER" \
         -H 'content-type: application/json' -X POST \
         --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"user\"}" \
         "$SRV/v1/facts/retract"
  fi
}

# Refuse to run under a posture where every write leg is read-only for a reason
# that has nothing to do with accounts -- that would "pass" leg 1 and fail leg 2
# while proving nothing either way.
mtls_mode="$(grep -E '^    mtls: ' /root/aimee.yaml | awk '{print $2}')"
if [ "${mtls_mode:-}" != "off" ]; then
  echo "FAIL: aimee.api.mtls is '${mtls_mode:-unset}', not 'off'." >&2
  echo "      In optional/required mode a caller with no client certificate is" >&2
  echo "      read-only regardless of its identity token, so this test cannot" >&2
  echo "      reach the authority decision. Run set-mtls-mode.sh off first." >&2
  exit 1
fi

bash /root/seed-facts.sh >/dev/null 2>&1
echo "seeded: alice=$(state alice) bob=$(state bob)"

echo
echo "=== 1. bearer only, NO identity token (no account) ==="
r1="$(retract alice "")"
echo "  response: $(printf '%s' "$r1" | tr '\n' ' ' | head -c 170)"
a1="$(state alice)"
echo "  alice: $a1"

echo
echo "=== 3. identity token minted for a DIFFERENT audience ==="
bad="$(mint "$SUB" "some-other-server")" || bad=""
r3="$(retract alice "$bad")"
echo "  response: $(printf '%s' "$r3" | tr '\n' ' ' | head -c 170)"
a3="$(state alice)"
echo "  alice: $a3"

echo
echo "=== 2. identity token for the account, correct audience ==="
tok="$(mint "$SUB" "$AIMEE_SERVER_ID")" || { echo "FAIL: could not mint" >&2; exit 1; }
r2="$(retract alice "$tok")"
echo "  response: $(printf '%s' "$r2" | tr '\n' ' ' | head -c 170)"
a2="$(state alice)"
echo "  alice: $a2"

echo
echo "=== 4. control: the same account against a model-authored fact ==="
tok2="$(mint "$SUB" "$AIMEE_SERVER_ID")"
r4="$(retract bob "$tok2")"
echo "  response: $(printf '%s' "$r4" | tr '\n' ' ' | head -c 170)"
b4="$(state bob)"
echo "  bob: $b4"

echo
case "$a1" in
  *current*) echo "PASS: a bearer with no account retracted nothing" ;;
  *)         echo "FAIL: a caller with no account retracted a Class-A fact"; rc=1 ;;
esac
case "$a3" in
  *current*) echo "PASS: a token for another audience was refused" ;;
  *)         echo "FAIL: a token minted for a different server was accepted"; rc=1 ;;
esac
case "$r2" in
  *authentication_error*|*permission_error*)
    echo "FAIL: the account leg never reached the authority decision -- refused at a"
    echo "      wall above it, so this run says nothing about accounts over TCP"
    rc=1 ;;
  *)
    case "$a2" in
      *gone*) echo "PASS: the account retracted a Class-A fact OVER TCP" ;;
      *)      echo "FAIL: the account did not get user authority over TCP"; rc=1 ;;
    esac ;;
esac
case "$b4" in
  *gone*) echo "PASS: the Class-B control retracted, so the endpoint is working" ;;
  *)      echo "FAIL: the control did not retract -- this run proves nothing"; rc=1 ;;
esac
exit $rc
