#!/bin/sh
# aimee-llm-entrypoint.sh: llama-server on loopback, mTLS on the network interface.
#
# THE SHAPE, and why it is this way:
#
#   network :8761  ->  stunnel (verifies the client certificate)  ->  127.0.0.1:8760  ->  llama-server
#
# llama-server never binds a network interface. That is not defence in depth so much
# as arithmetic: loopback inside this container's own network namespace is
# unreachable from any other container, so the terminator is the only path in. It
# also means llama-server's own warning that CORS is open and no API key is set is
# contained by construction rather than by configuration.
#
# stunnel does the verifying because llama-server cannot. The pinned b10218 has
# --api-key and --ssl-cert-file but no client-certificate or CA flag whatsoever.
#
# THIS CONTAINER DOES NOT MINT ITS OWN IDENTITY. The KB owns the CA
# (kb_pki_ca_load_or_create_custodied) and the KB is the client on this hop, so the
# KB issues this server's certificate. Deployment order guarantees the material
# exists before this starts: server, wizard, kb, then this. A missing identity is
# therefore a real error and is reported as one, not waited out silently.
set -eu

IDENTITY_DIR="${AIMEE_LLM_IDENTITY_DIR:-/var/lib/aimee-llm/tls}"
CA_FILE="${AIMEE_LLM_CA_FILE:-$IDENTITY_DIR/ca.pem}"
CERT_FILE="${AIMEE_LLM_CERT_FILE:-$IDENTITY_DIR/server.pem}"
KEY_FILE="${AIMEE_LLM_KEY_FILE:-$IDENTITY_DIR/server.key}"
: "${AIMEE_LLM_PORT:=8761}"
: "${AIMEE_LLM_LOOPBACK_PORT:=8760}"
: "${AIMEE_LLM_CTX:=8192}"

LLAMA=/opt/aimee/llama.cpp/llama-server
: "${LLAMA_CACHE:=/opt/aimee/llama.cpp/cache}"
export LLAMA_CACHE
TABLE=/usr/local/bin/synthesis-model-table.sh

log() { echo "aimee-llm: $*" >&2; }

# The model is addressed REPO-SHAPED even though it is baked, because that is the only
# form that engages MTP: -hfd names the same repo and llama.cpp loads mtp-<model>.gguf
# as the speculative draft. The file-path form (-md) does not engage it.
#
# --offline is what makes that safe. llama.cpp documents it as "forces use of cache,
# prevents network access", so resolution is satisfied entirely from the cache baked
# into this image. A user never downloads a model, and cannot.
if [ ! -x "$LLAMA" ] || [ ! -d "$LLAMA_CACHE" ]; then
    log "no llama-server or no baked cache at $LLAMA_CACHE. Both are built into this"
    log "image, so their absence means the image is broken rather than unconfigured."
    exit 1
fi

REPO="$(sh "$TABLE" repo "${AIMEE_SYNTHESIS_MODEL:?AIMEE_SYNTHESIS_MODEL is baked into this image}")"
QUANT="$(sh "$TABLE" quant "$AIMEE_SYNTHESIS_MODEL")"

# Fail closed, and say which piece is missing. Starting llama-server anyway and
# leaving the terminator down would put a working inference server behind nothing,
# on a port nobody is listening on: the deployment would look started and answer
# nothing, which is the failure mode this whole design exists to avoid.
missing=""
[ -r "$CA_FILE" ]   || missing="$missing $CA_FILE"
[ -r "$CERT_FILE" ] || missing="$missing $CERT_FILE"
[ -r "$KEY_FILE" ]  || missing="$missing $KEY_FILE"
if [ -n "$missing" ]; then
    log "refusing to start: mTLS identity missing:$missing"
    log "The kb issues this identity when synthesis is deployed. If this container"
    log "was started before the kb, start it again after the kb is connected."
    exit 1
fi

STUNNEL_CONF=/tmp/aimee-llm-stunnel.conf
cat > "$STUNNEL_CONF" <<EOF
foreground = yes
# stunnel's own log to stderr, so container logs carry handshake failures rather
# than swallowing them into a file nobody reads.
output = /dev/stderr
debug = 4
pid =

[synthesis]
accept = 0.0.0.0:$AIMEE_LLM_PORT
connect = 127.0.0.1:$AIMEE_LLM_LOOPBACK_PORT
cert = $CERT_FILE
key = $KEY_FILE
CAfile = $CA_FILE
# verifyChain, and deliberately NOT verifyPeer.
#
# These are not two strengths of the same check. verifyChain verifies the client
# certificate against the CA in CAfile, which is what this hop wants: the kb presents
# an identity our CA issued. verifyPeer matches the presented certificate against a
# local REPOSITORY of known peer certificates -- pinning, not CA verification -- and
# setting both rejects a perfectly valid client with
#   CERT: Certificate not found in local repository
#   Rejected by CERT at depth=0: CN=aimee-kb-synthesis
# which is what this configuration did until it was run against a real kb.
#
# verifyChain alone still REQUIRES a certificate: an anonymous client is refused with
# "tlsv13 alert certificate required", so dropping verifyPeer does not weaken it.
verifyChain = yes
sslVersionMin = TLSv1.3
EOF

cleanup() {
    [ -n "${llama_pid:-}"   ] && kill "$llama_pid"   2>/dev/null || true
    [ -n "${stunnel_pid:-}" ] && kill "$stunnel_pid" 2>/dev/null || true
}
trap cleanup TERM INT

log "starting synthesis (${AIMEE_SYNTHESIS_MODEL:-unknown}) on 127.0.0.1:$AIMEE_LLM_LOOPBACK_PORT"
# --no-mmproj: every benchmark run passed it and the shipped service never did, so
# production loaded a vision projector for a text-only task that cannot use it. Only
# the text GGUF is baked here, so there is nothing to load -- the flag keeps it that
# way if a future image bakes one.
#
# -hfd names the SAME repo as -hf, which is what makes llama.cpp load
# mtp-<model>.gguf as its speculative draft. Both resolve from the baked cache because
# of --offline; neither reaches the network.
LLAMA_ARG_MMPROJ_AUTO=false \
    "$LLAMA" -hf "$REPO:$QUANT" -hfd "$REPO" --offline \
             --host 127.0.0.1 --port "$AIMEE_LLM_LOOPBACK_PORT" \
             -c "$AIMEE_LLM_CTX" --no-webui --no-mmproj >&2 &
llama_pid=$!

# ASSERT MTP ACTUALLY ENGAGED, before anything is served.
#
# This is the one failure in this container with no symptom. A missing identity refuses
# to start; a dead llama-server exits; MTP that quietly did not engage serves correct
# answers 1.6-1.8x slower, and nothing anywhere says so. The only way to notice is to
# measure tokens/sec against a baseline nobody has.
#
# /slots reports .speculative per slot, which is llama.cpp's own answer to "is the
# draft model in play". Refusing to serve without it is consistent with the rest of
# this entrypoint: a sidecar that cannot do its job says so at deploy.
spec=""
for _ in $(seq 1 120); do
    if curl -sf -m 4 "http://127.0.0.1:$AIMEE_LLM_LOOPBACK_PORT/health" >/dev/null 2>&1; then
        spec="$(curl -s -m 4 "http://127.0.0.1:$AIMEE_LLM_LOOPBACK_PORT/slots" 2>/dev/null \
                | tr ',' '\n' | grep -m1 '"speculative"' | grep -c 'true' || true)"
        break
    fi
    kill -0 "$llama_pid" 2>/dev/null || break
    sleep 5
done

if [ "${spec:-0}" != "1" ]; then
    log "refusing to serve: multi-token prediction did not engage."
    log "This image bakes mtp-${AIMEE_SYNTHESIS_MODEL}.gguf and passes -hfd so"
    log "llama.cpp loads it as a speculative draft. Without it synthesis is correct"
    log "and roughly 1.6-1.8x slower, with nothing else to indicate why -- so this"
    log "fails at deploy rather than silently costing throughput forever."
    "$LLAMA" --version >&2 2>&1 || true
    cleanup
    exit 1
fi
log "multi-token prediction engaged (slots report speculative=true)"

log "starting mTLS terminator on :$AIMEE_LLM_PORT (client certificate required)"
stunnel "$STUNNEL_CONF" >&2 &
stunnel_pid=$!

# Either process dying takes the container with it. A half-up sidecar is worse than
# a down one: the kb would either reach an unterminated port or hang against a
# terminator with nothing behind it, and both look like "synthesis is slow".
while :; do
    if ! kill -0 "$llama_pid" 2>/dev/null; then
        log "llama-server exited; stopping"
        cleanup
        wait "$llama_pid" 2>/dev/null || true
        exit 1
    fi
    if ! kill -0 "$stunnel_pid" 2>/dev/null; then
        log "stunnel exited; stopping (llama-server is only reachable through it)"
        cleanup
        wait "$stunnel_pid" 2>/dev/null || true
        exit 1
    fi
    sleep 5
done
