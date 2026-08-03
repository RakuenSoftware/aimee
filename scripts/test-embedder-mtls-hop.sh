#!/usr/bin/env bash
# test-embedder-mtls-hop.sh: the embedder sidecar, on a real container, over real mTLS.
#
# WHY THIS EXISTS AS A SCRIPT. Its sibling for synthesis
# (scripts/test-synthesis-mtls-hop.sh) passed 9/9 against a deployment where synthesis
# could not work at all, because it drove `curl --cert --key` and so proved the SIDECAR
# accepts a correct client while saying nothing about whether the kb is one. Both facts
# need testing; this covers the sidecar half, and it covers it properly:
#
#   - the container REFUSES to start with no identity, rather than serving unterminated
#   - a client presenting a certificate reaches the embedder through the terminator
#   - an anonymous client is refused at the handshake
#   - and it actually EMBEDS, returning a vector of the width the registry declares
#
# That last one is the point. A terminator that answers /health proves the plumbing; a
# vector of the right width proves the thing behind it works.
#
# SELF-CONTAINED CA. It mints its own, rather than needing a kb, so this runs anywhere
# docker does. In production the kb owns the CA and issues this certificate, because the
# kb is the client on the hop -- that path is exercised by the deploy tests, not here.
#
# Usage:  scripts/test-embedder-mtls-hop.sh <embedder-image> [expected-dim]
#   e.g.  scripts/test-embedder-mtls-hop.sh aimee-embedder-a25m:local 384
set -uo pipefail

IMAGE="${1:-}"
WANT_DIM="${2:-}"
if [ -z "$IMAGE" ]; then
   echo "usage: $0 <embedder-image> [expected-dim]" >&2
   exit 2
fi
command -v docker >/dev/null || { echo "docker not found"; exit 2; }
command -v openssl >/dev/null || { echo "openssl not found"; exit 2; }

NET=embhop-net
NAME=aimee-embedder          # must match the certificate's SAN, and be resolvable
CLIENT=embhop-client
WORK="$(mktemp -d)"
pass=0
fail=0

cleanup() {
   docker rm -f "$NAME" "$CLIENT" >/dev/null 2>&1
   docker network rm "$NET" >/dev/null 2>&1
   rm -rf "$WORK"
}
trap cleanup EXIT

check() {
   if [ "$1" = "0" ]; then
      echo "  ok    $2"
      pass=$((pass + 1))
   else
      echo "  FAIL  $2${3:+: $3}"
      fail=$((fail + 1))
   fi
}

# --- material -------------------------------------------------------------------
# basicConstraints/keyUsage on the CA are NOT optional: without them OpenSSL refuses the
# chain with "unable to get local issuer certificate", which reads like a missing file.
echo "==> minting a CA, a server certificate for $NAME, and a client certificate"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=embhop-ca" \
   -addext "basicConstraints=critical,CA:TRUE" \
   -addext "keyUsage=critical,keyCertSign,cRLSign" \
   -keyout "$WORK/ca.key" -out "$WORK/ca.pem" 2>/dev/null

mint() { # mint <name> <cn> [san]
   openssl req -newkey rsa:2048 -nodes -subj "/CN=$2" \
      -keyout "$WORK/$1.key" -out "$WORK/$1.csr" 2>/dev/null
   local ext="$WORK/$1.ext"
   : > "$ext"
   [ -n "${3:-}" ] && printf 'subjectAltName=DNS:%s\n' "$3" > "$ext"
   openssl x509 -req -in "$WORK/$1.csr" -CA "$WORK/ca.pem" -CAkey "$WORK/ca.key" \
      -CAcreateserial -days 2 -out "$WORK/$1.pem" \
      ${3:+-extfile "$ext"} 2>/dev/null
}
mint server "$NAME" "$NAME"
mint client "embhop-kb"
chmod 0644 "$WORK"/*.pem "$WORK"/*.key

docker network rm "$NET" >/dev/null 2>&1
docker network create "$NET" >/dev/null

# --- 1. no identity -> refuse ---------------------------------------------------
# A sidecar that starts anyway would serve the embedder unterminated, or leave the
# terminator down on a port the kb is about to call.
echo "==> with no identity mounted"
docker run --rm --name "$NAME" --network "$NET" "$IMAGE" >"$WORK/noid.log" 2>&1
rc=$?
[ "$rc" -ne 0 ] && grep -qi "identity missing" "$WORK/noid.log"
check $? "refuses to start with no mTLS identity" "exit=$rc $(tail -1 "$WORK/noid.log")"

# --- 2. with identity -> serves --------------------------------------------------
echo "==> with the identity mounted"
docker run -d --name "$NAME" --network "$NET" \
   -v "$WORK/ca.pem:/var/lib/aimee-embedder/tls/ca.pem:ro" \
   -v "$WORK/server.pem:/var/lib/aimee-embedder/tls/server.pem:ro" \
   -v "$WORK/server.key:/var/lib/aimee-embedder/tls/server.key:ro" \
   "$IMAGE" >/dev/null

up=1
for _ in $(seq 1 90); do
   [ "$(docker inspect -f '{{.State.Health.Status}}' "$NAME" 2>/dev/null)" = "healthy" ] && { up=0; break; }
   [ "$(docker inspect -f '{{.State.Status}}' "$NAME" 2>/dev/null)" = "exited" ] && break
   sleep 5
done
check "$up" "sidecar reports healthy" "$(docker logs "$NAME" 2>&1 | tail -2 | tr '\n' ' ')"

# A separate container as the client: the kb reaches this over the network, so testing
# from inside the sidecar would prove something else entirely.
docker run -d --name "$CLIENT" --network "$NET" \
   -v "$WORK:/tls:ro" --entrypoint sleep debian:trixie-slim 3600 >/dev/null
docker exec "$CLIENT" sh -c 'apt-get update -qq && apt-get install -y -qq curl >/dev/null 2>&1'

CURL_MTLS="curl -sS -m 30 --cacert /tls/ca.pem --cert /tls/client.pem --key /tls/client.key"

code=$(docker exec "$CLIENT" sh -c "$CURL_MTLS -o /dev/null -w '%{http_code}' https://$NAME:8762/health" 2>/dev/null)
check "$([ "$code" = 200 ] && echo 0 || echo 1)" "client certificate accepted (200)" "got $code"

# --- 3. anonymous -> refused ----------------------------------------------------
# verifyChain alone still REQUIRES a certificate; this is what keeps that honest.
out=$(docker exec "$CLIENT" sh -c "curl -sS -m 30 --cacert /tls/ca.pem https://$NAME:8762/health" 2>&1)
echo "$out" | grep -qiE "certificate required|alert (handshake failure|certificate)"
check $? "anonymous client refused at the handshake" "$(echo "$out" | tail -1)"

# --- 4. it actually embeds ------------------------------------------------------
# The assertion that matters. /health only proves the terminator and the loader; this
# proves the vector space is being served, at the width the registry declares.
echo "==> embedding through the hop"
health=$(docker exec "$CLIENT" sh -c "$CURL_MTLS https://$NAME:8762/health" 2>/dev/null)
served_dim=$(printf '%s' "$health" | sed -n 's/.*"dim"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p')
vec=$(docker exec "$CLIENT" sh -c \
   "$CURL_MTLS -X POST --data-binary 'the reconcile service matches ledger rows to bank statements' \
    https://$NAME:8762/embed" 2>/dev/null)
got_dim=$(printf '%s' "$vec" | tr -cd ',' | wc -c)
got_dim=$((got_dim + 1))
[ -n "$served_dim" ] && [ "$got_dim" = "$served_dim" ]
check $? "embed returns a vector of the served width" "health says $served_dim, got $got_dim"

if [ -n "$WANT_DIM" ]; then
   [ "$served_dim" = "$WANT_DIM" ]
   check $? "served width is the expected $WANT_DIM" "got $served_dim"
fi

# A serving_id is what the kb records against its corpus to detect a pooling or prefix
# change that leaves the width and the model name alone, so it must be present.
printf '%s' "$health" | grep -q '"serving_id"'
check $? "health carries a serving_id for the kb to record"

echo
echo "==> Summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
echo "embedder mTLS hop: ok"
