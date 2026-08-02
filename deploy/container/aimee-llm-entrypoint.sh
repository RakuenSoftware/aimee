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

MODEL_DIR=/opt/aimee/llama.cpp/model
MODEL="$MODEL_DIR/synthesis.gguf"
LLAMA=/opt/aimee/llama.cpp/llama-server

log() { echo "aimee-llm: $*" >&2; }

if [ ! -s "$MODEL" ]; then
    log "no model at $MODEL. This image is built with one baked in, so an empty"
    log "path means the image is broken rather than unconfigured."
    exit 1
fi

if [ -z "${AIMEE_SYNTHESIS_MODEL:-}" ] && [ -r "$MODEL_DIR/MODEL_ID" ]; then
    AIMEE_SYNTHESIS_MODEL="$(cat "$MODEL_DIR/MODEL_ID")"
    export AIMEE_SYNTHESIS_MODEL
fi

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
# 4 = "verify peer against the CA, and REQUIRE a certificate". Levels below this
# accept an anonymous client, which would make the terminator ornamental.
verifyChain = yes
verifyPeer = yes
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
LLAMA_ARG_MMPROJ_AUTO=false \
    "$LLAMA" -m "$MODEL" --host 127.0.0.1 --port "$AIMEE_LLM_LOOPBACK_PORT" \
             -c "$AIMEE_LLM_CTX" --no-webui --no-mmproj >&2 &
llama_pid=$!

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
