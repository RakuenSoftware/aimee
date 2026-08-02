#!/usr/bin/env bash
# test-synthesis-mtls-hop.sh: the kb -> aimee-llm hop, on two real containers.
#
# WHY THIS EXISTS AS A SCRIPT. Every other gate passed while this hop was broken.
# `make lint` (41 checks), the identity unit test (16 assertions), the client-side
# handshake test, and both image builds in CI were all green against a sidecar that
# rejected every request the kb made. Two defects, either of which would have shipped
# as "configured, deployed, does nothing":
#
#   1. The kb could not write its own identity directory. Docker initialises a named
#      volume from the image's content at the mount point, so an absent path yields a
#      root-owned volume and the kb runs as `aimee`.
#   2. stunnel had verifyPeer as well as verifyChain. verifyPeer PINS against a local
#      repository of peer certificates; it does not mean "verify harder". A correctly
#      issued client certificate was refused with "Certificate not found in local
#      repository".
#
# Nothing short of running the two containers against each other finds either. So
# this is the shape of that test, kept runnable.
#
# Usage:  scripts/test-synthesis-mtls-hop.sh <kb-image> <llm-image>
#   e.g.  scripts/test-synthesis-mtls-hop.sh aimee-kb-a25m:local aimee-llm-e2b:local
#
# Needs a Docker daemon. It is NOT wired into `make lint`: lint must run without one,
# and a check that silently skips is worse than one an operator runs deliberately.
set -uo pipefail

KB_IMAGE="${1:-}"
LLM_IMAGE="${2:-}"
if [ -z "$KB_IMAGE" ] || [ -z "$LLM_IMAGE" ]; then
   echo "usage: $0 <kb-image> <llm-image>" >&2
   exit 2
fi
command -v docker >/dev/null || { echo "docker not found"; exit 2; }

NET=synthhop-net
VOL=synthhop-tls
KB=synthhop-kb
LLM=aimee-llm   # MUST match the certificate CN the kb issues, and be resolvable by it
TLS=/var/lib/aimee/synthesis-tls
pass=0
fail=0

cleanup() {
   docker rm -f "$KB" "$LLM" >/dev/null 2>&1
   docker volume rm "$VOL" >/dev/null 2>&1
   docker network rm "$NET" >/dev/null 2>&1
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

cleanup
docker network create "$NET" >/dev/null
docker volume create "$VOL" >/dev/null

echo "==> kb mints the identities"
docker run -d --name "$KB" --network "$NET" \
   -e AIMEE_LLM_HOST="$LLM" -e EMBEDDER_MODEL=bekko-a25m \
   -v "$VOL:$TLS" "$KB_IMAGE" >/dev/null

health=""
for _ in $(seq 1 60); do
   health=$(docker inspect -f '{{.State.Health.Status}}' "$KB" 2>/dev/null)
   [ "$health" = "healthy" ] && break
   sleep 5
done
check "$([ "$health" = healthy ] && echo 0 || echo 1)" "the kb comes up healthy" "$health"

# The permission failure was a WARN, not a crash: the kb stayed healthy and the
# identities simply were not there. Assert the files, never the absence of an error.
for f in ca.pem server.pem server.key client.pem client.key; do
   docker exec "$KB" test -s "$TLS/$f" 2>/dev/null
   check $? "issued $f"
done

echo "==> the sidecar starts against that material"
docker run -d --name "$LLM" --network "$NET" -v "$VOL:/var/lib/aimee-llm/tls:ro" \
   "$LLM_IMAGE" >/dev/null
# Wait for HEALTHY, not for the "terminator listening" log line. Those are seconds
# apart -- the entrypoint starts llama-server in the background and stunnel right
# after, so the port accepts connections while the model is still loading and the hop
# returns 503. Waiting on the log line made this test fail against a working sidecar.
up=1
for _ in $(seq 1 60); do
   [ "$(docker inspect -f '{{.State.Health.Status}}' "$LLM" 2>/dev/null)" = "healthy" ] && { up=0; break; }
   [ "$(docker inspect -f '{{.State.Status}}' "$LLM" 2>/dev/null)" = "exited" ] && break
   sleep 5
done
check "$up" "the sidecar reports healthy" "$(docker logs "$LLM" 2>&1 | tail -2 | tr '\n' ' ')"

echo "==> the hop"
# WITH the kb's client certificate: must reach llama-server behind the terminator.
code=$(docker exec "$KB" curl -sS -m 30 -o /dev/null -w '%{http_code}' \
   --cacert "$TLS/ca.pem" --cert "$TLS/client.pem" --key "$TLS/client.key" \
   "https://$LLM:8761/v1/models" 2>/dev/null)
check "$([ "$code" = 200 ] && echo 0 || echo 1)" "client certificate accepted (200)" "got $code"

# WITHOUT one: must be refused at the handshake. This is the assertion that keeps the
# terminator honest -- verifyChain alone still requires a certificate.
out=$(docker exec "$KB" curl -sS -m 30 --cacert "$TLS/ca.pem" \
   "https://$LLM:8761/v1/models" 2>&1)
echo "$out" | grep -qi "certificate required"
check $? "anonymous client refused" "$(echo "$out" | tail -1)"

echo
echo "==> Summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
echo "synthesis mTLS hop: ok"
