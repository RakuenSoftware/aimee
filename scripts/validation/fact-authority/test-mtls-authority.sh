#!/bin/bash
# Does an mTLS caller get USER authority from its ACCOUNT?
#
# This is the case the old transport table got wrong. A verified client
# certificate resolves to "cert:<CN>" -- a named, enrolled machine, which is
# NARROWER than a person (one person may connect several thin clients to one
# server) -- and it was classed with shared bearers as "not a person".
#
# Four legs, because any one alone is unfalsifiable:
#
#   1. rogue cert (untrusted CA)  -> handshake MUST fail. Without this, leg 3
#                                    could mean "any certificate is accepted",
#                                    which would be worse than the bug.
#   2. bearer only over TLS       -> no account -> MODEL -> retracts nothing
#   3. trusted client cert        -> account cert:thin-client-a -> USER -> retracts
#   4. re-seed and confirm 3 was  -> a Class-B control, so leg 3 cannot be
#      not a fluke                   "the endpoint retracts for anyone"
#
# Legs 2 and 3 differ ONLY in whether a client certificate is presented. Same
# port, same TLS, same body. If the transport still decided, they would agree.
# Usage: test-mtls-authority.sh [BEARER]   (else /root/api-bearer.txt)
# Run AS ROOT in the container.
set -u
export LC_ALL=C
TLS=/root/.config/aimee-tls
# aimee generates a SELF-SIGNED server identity (CN=<hostname>, SAN covering
# localhost/127.0.0.1), so the cert is its own CA for --cacert purposes. Not -k:
# skipping verification here would also hide a genuinely broken TLS setup.
SRV_CA=/root/tls/server.crt
PORT=8743
B="${1:-$(cat /root/api-bearer.txt 2>/dev/null || echo)}"
[ -n "$B" ] || { echo "FAIL: no API bearer -- every TLS leg would be refused at the auth wall and prove nothing" >&2; exit 1; }
P=/root/psql.sh
rc=0

state() {
  $P "select confidence_class || ' ' || case when superseded_at='' and invalidated_at='' and suppressed=0 then 'current' else 'gone' end from entity_edges where source='$1' and relation='works_for'" | tail -1
}

body() { printf '{"source":"%s","relation":"works_for","authority":"user"}' "$1"; }

curl_tls() { # $@ = extra curl args
  curl -s -m 25 --cacert "$SRV_CA" \
       -H "Authorization: Bearer $B" -H 'content-type: application/json' \
       "$@"
}

echo "=== 0. is TLS actually up on $PORT? ==="
if ! curl -s -m 10 --cacert "$SRV_CA" "https://127.0.0.1:$PORT/v1/health" >/dev/null 2>&1; then
  echo "FAIL: nothing serving TLS on $PORT -- every leg below would 'pass' by failing" >&2
  exit 1
fi
echo "  TLS listener answering"

bash /root/seed-facts.sh >/dev/null 2>&1
echo "  seeded: alice=$(state alice) bob=$(state bob)"

echo
echo "=== 1. NEGATIVE CONTROL: certificate from an untrusted CA ==="
rogue_out="$(curl_tls --cert "$TLS/rogue.crt" --key "$TLS/rogue.key" \
             -X POST --data "$(body alice)" \
             "https://127.0.0.1:$PORT/v1/facts/retract" 2>&1; echo "rc=$?")"
echo "  result: $(printf '%s' "$rogue_out" | tr '\n' ' ' | head -c 160)"
case "$rogue_out" in
  *rc=0*) echo "  FAIL: an untrusted client certificate completed the handshake"; rc=1 ;;
  *)      echo "  PASS: refused -- the client CA is genuinely being verified" ;;
esac
echo "  alice: $(state alice)"

echo
echo "=== 2. TLS + bearer, NO client certificate (no account) ==="
no_cert="$(curl_tls -X POST --data "$(body alice)" \
           "https://127.0.0.1:$PORT/v1/facts/retract")"
echo "  response: $(printf '%s' "$no_cert" | tr '\n' ' ' | head -c 200)"
a_after_bearer="$(state alice)"
echo "  alice: $a_after_bearer"

echo
echo "=== 3. TLS + the SAME bearer + a trusted client certificate ==="
with_cert="$(curl_tls --cert "$TLS/client.crt" --key "$TLS/client.key" \
             -X POST --data "$(body alice)" \
             "https://127.0.0.1:$PORT/v1/facts/retract")"
echo "  response: $(printf '%s' "$with_cert" | tr '\n' ' ' | head -c 200)"
a_after_cert="$(state alice)"
echo "  alice: $a_after_cert"

echo
echo "=== 4. control: the same certificate against a model-authored fact ==="
ctl="$(curl_tls --cert "$TLS/client.crt" --key "$TLS/client.key" \
       -X POST --data "$(body bob)" \
       "https://127.0.0.1:$PORT/v1/facts/retract")"
echo "  response: $(printf '%s' "$ctl" | tr '\n' ' ' | head -c 200)"
b_after="$(state bob)"
echo "  bob: $b_after"

echo
case "$with_cert" in
  *permission_error*|*authentication_error*)
    echo "FAIL: the certificate leg never reached the authority decision."
    echo "      It was refused at an auth or capability wall, so this run says"
    echo "      nothing about whether cert:<CN> counts as an account."
    rc=1 ;;
  *)
    case "$a_after_cert" in
      *gone*) echo "PASS: the mTLS caller retracted a Class-A fact -- cert:<CN> is an account" ;;
      *)      echo "FAIL: the mTLS caller could not retract; its account is not granting USER"; rc=1 ;;
    esac ;;
esac
case "$a_after_bearer" in
  *current*) echo "PASS: the same request WITHOUT a certificate did not retract" ;;
  *)         echo "FAIL: the bearer-only leg retracted too -- the certificate is not what decided"; rc=1 ;;
esac
case "$b_after" in
  *gone*) echo "PASS: the Class-B control retracted, so the endpoint is working" ;;
  *)      echo "FAIL: the control did not retract -- this run proves nothing"; rc=1 ;;
esac
exit $rc
