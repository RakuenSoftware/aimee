#!/bin/bash
# Does an OIDC caller get USER authority from its ACCOUNT?
#
# kb_memory_request_authority() reads the request's authenticated principal.
# KB_PRIN_OIDC is an account, so an OIDC bearer must be able to retract a
# user-stated Class-A fact -- and it must be the ACCOUNT that decides, not the
# socket the request arrived on. Every leg here is plain TCP to the kb's
# listener, so transport is held constant throughout.
#
# Four legs, because any one alone is unfalsifiable:
#
#   1. rogue JWT (same claims, same kid, signed by a key NOT in the JWKS)
#      -> MUST be refused. Without this, leg 3 could mean "kb accepts anything
#         shaped like a JWT", which is worse than the bug under test.
#   2. no credential at all         -> no account -> refused
#   3. the real OIDC bearer         -> account -> retracts the Class-A fact
#   4. Class-B control              -> so leg 3 cannot be "it retracts for anyone"
#
# Usage: test-oidc-authority.sh
# Run AS ROOT in the container.
set -u
export LC_ALL=C
DIR=/root/.config/aimee-oidc
KB=http://127.0.0.1:8741
P=/root/psql.sh
rc=0

state() {
  $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='$1' and relation='works_for'" | tail -1
}

retract() { # $1 = source, $2 = authorization header value ("" for none)
  if [ -n "$2" ]; then
    curl -s -m 20 -H "Authorization: Bearer $2" -H 'content-type: application/json' \
         -X POST --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"user\"}" \
         "$KB/v1/actions/facts.retract"
  else
    curl -s -m 20 -H 'content-type: application/json' \
         -X POST --data "{\"source\":\"$1\",\"relation\":\"works_for\",\"authority\":\"user\"}" \
         "$KB/v1/actions/facts.retract"
  fi
}

# Mint FRESH tokens rather than reuse whatever is on disk. kb_oidc applies a
# hard token-age ceiling (AIMEE_KB_OIDC_MAX_TOKEN_AGE, default 900s) on `iat`
# independently of `exp`, so a token minted at setup time is refused once the
# run happens more than fifteen minutes later -- and the refusal is the same
# "unauthorized" a forged token gets, so the whole suite reads as a broken
# account path when nothing is broken.
if ! bash /root/make-oidc-idp.sh >/dev/null 2>&1; then
  echo "FAIL: could not mint fresh OIDC tokens" >&2; exit 1
fi
for f in token.jwt rogue.jwt; do
  [ -s "$DIR/$f" ] || { echo "FAIL: $DIR/$f missing -- run make-oidc-idp.sh first" >&2; exit 1; }
done
TOKEN="$(cat "$DIR/token.jwt")"
ROGUE="$(cat "$DIR/rogue.jwt")"

echo "=== 0. does the kb VERIFY this bearer? ==="
# Not /v1/identity/auth-mode: that reports the LOGIN mode, which needs a full
# authorization-code profile (client_id, endpoints) and answers "pam" on a kb
# configured only to VERIFY OIDC bearers. Bearer verification and interactive
# login are separate concerns, and only the first one is under test here.
# A read that requires authentication is the honest probe.
probe="$(curl -s -m 15 -H "Authorization: Bearer $TOKEN" -H 'content-type: application/json' \
         -X POST --data '{}' "$KB/v1/actions/relations.schema_list")"
echo "  probe: $(printf '%s' "$probe" | tr '\n' ' ' | head -c 140)"
case "$probe" in
  *unauthorized*|*authentication_error*)
    echo "FAIL: the kb does not accept this OIDC bearer at all -- every leg below" >&2
    echo "      would 'pass' by being refused, proving nothing about authority" >&2
    exit 1 ;;
esac
echo "  the OIDC bearer authenticates"

bash /root/seed-facts.sh >/dev/null 2>&1
echo "  seeded: alice=$(state alice) bob=$(state bob)"

echo
echo "=== 1. NEGATIVE CONTROL: JWT signed by a key absent from the JWKS ==="
r1="$(retract alice "$ROGUE")"
echo "  response: $(printf '%s' "$r1" | tr '\n' ' ' | head -c 160)"
a1="$(state alice)"
echo "  alice: $a1"

echo
echo "=== 2. no credential at all ==="
r2="$(retract alice "")"
echo "  response: $(printf '%s' "$r2" | tr '\n' ' ' | head -c 160)"
a2="$(state alice)"
echo "  alice: $a2"

echo
echo "=== 3. the real OIDC bearer ==="
r3="$(retract alice "$TOKEN")"
echo "  response: $(printf '%s' "$r3" | tr '\n' ' ' | head -c 160)"
a3="$(state alice)"
echo "  alice: $a3"

echo
echo "=== 4. control: the same bearer against a model-authored fact ==="
r4="$(retract bob "$TOKEN")"
echo "  response: $(printf '%s' "$r4" | tr '\n' ' ' | head -c 160)"
b4="$(state bob)"
echo "  bob: $b4"

echo
case "$a1" in
  *current*) echo "PASS: the forged JWT changed nothing -- signatures are genuinely verified" ;;
  *)         echo "FAIL: a JWT signed by an unknown key retracted a fact"; rc=1 ;;
esac
case "$a2" in
  *current*) echo "PASS: an unauthenticated caller changed nothing" ;;
  *)         echo "FAIL: an unauthenticated caller retracted a fact"; rc=1 ;;
esac
case "$r3" in
  *unauthorized*|*authentication_error*|*permission_error*)
    echo "FAIL: the OIDC leg never reached the authority decision (refused at an auth wall),"
    echo "      so this run says nothing about whether an OIDC subject is an account"
    rc=1 ;;
  *)
    case "$a3" in
      *gone*) echo "PASS: the OIDC caller retracted a Class-A fact -- the subject is an account" ;;
      *)      echo "FAIL: the OIDC caller could not retract; its account is not granting USER"; rc=1 ;;
    esac ;;
esac
case "$b4" in
  *gone*) echo "PASS: the Class-B control retracted, so the endpoint is working" ;;
  *)      echo "FAIL: the control did not retract -- this run proves nothing"; rc=1 ;;
esac
exit $rc
