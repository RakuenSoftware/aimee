#!/bin/sh
# aimee-embedder-entrypoint.sh: the embedder on loopback, mTLS on the network interface.
#
# THE SHAPE, and it is deliberately the same as aimee-llm's:
#
#   network :8762  ->  stunnel (verifies the client certificate)  ->  127.0.0.1:8760
#
# embedder-server.py never binds a network interface. Loopback inside this container's
# own namespace is unreachable from any other container, so the terminator is the only
# path in -- and the server has no authentication of its own, so that is not defence in
# depth, it is the whole of it.
#
# THIS CONTAINER DOES NOT MINT ITS OWN IDENTITY. The kb owns the CA and the kb is the
# client on this hop, so the kb issues this server's certificate. Deployment order
# guarantees the material exists before this starts: server, wizard, kb, then the
# sidecars. A missing identity is a real error and is reported as one rather than waited
# out -- a half-started sidecar is worse than a stopped one, because the kb then either
# reaches an unterminated port or hangs against a terminator with nothing behind it, and
# both look like "embedding is slow".
set -eu

IDENTITY_DIR="${AIMEE_EMBEDDER_IDENTITY_DIR:-/var/lib/aimee-embedder/tls}"
CA_FILE="${AIMEE_EMBEDDER_CA_FILE:-$IDENTITY_DIR/ca.pem}"
CERT_FILE="${AIMEE_EMBEDDER_CERT_FILE:-$IDENTITY_DIR/server.pem}"
KEY_FILE="${AIMEE_EMBEDDER_KEY_FILE:-$IDENTITY_DIR/server.key}"
: "${AIMEE_EMBEDDER_PORT:=8762}"
: "${AIMEE_EMBEDDER_LOOPBACK_PORT:=8760}"

PY="${EMBEDDER_VENV:-/opt/aimee/embedder-venv}/bin/python"
SERVER=/opt/aimee/scripts/embedder-server.py

log() { echo "aimee-embedder: $*" >&2; }

if [ ! -s "$SERVER" ]; then
    log "no server at $SERVER; this image is broken rather than unconfigured"
    exit 1
fi

# The registry in this image holds exactly one embedder, so embedder-server.py's
# single-entry default resolves it and EMBEDDER_MODEL is optional. Set it anyway when
# the build recorded one, so the log names the model rather than leaving the reader to
# infer it from the tag.
if [ -z "${EMBEDDER_MODEL:-}" ] && [ -n "${AIMEE_EMBEDDER:-}" ]; then
    EMBEDDER_MODEL="$AIMEE_EMBEDDER"
    export EMBEDDER_MODEL
fi

# Fail closed, and say which piece is missing. Starting the server with no terminator
# would leave a working embedder on a port nobody can reach; starting the terminator
# with no server would accept connections and answer nothing.
missing=""
[ -r "$CA_FILE" ]   || missing="$missing $CA_FILE"
[ -r "$CERT_FILE" ] || missing="$missing $CERT_FILE"
[ -r "$KEY_FILE" ]  || missing="$missing $KEY_FILE"
if [ -n "$missing" ]; then
    log "refusing to start: mTLS identity missing:$missing"
    log "The kb issues this identity when an embedder sidecar is deployed. If this"
    log "container started before the kb, start it again once the kb is connected."
    exit 1
fi

STUNNEL_CONF=/tmp/aimee-embedder-stunnel.conf
cat > "$STUNNEL_CONF" <<EOF
foreground = yes
# stunnel's own log to stderr, so container logs carry handshake failures rather than
# swallowing them into a file nobody reads.
output = /dev/stderr
debug = 4
pid =

[embedder]
accept = 0.0.0.0:$AIMEE_EMBEDDER_PORT
connect = 127.0.0.1:$AIMEE_EMBEDDER_LOOPBACK_PORT
cert = $CERT_FILE
key = $KEY_FILE
CAfile = $CA_FILE
# verifyChain, and deliberately NOT verifyPeer. They are not two strengths of one
# check: verifyChain verifies the client certificate against CAfile, which is what this
# hop wants, while verifyPeer matches it against a local REPOSITORY of known peer
# certificates -- pinning -- and setting both rejects a correctly issued client with
#   CERT: Certificate not found in local repository
# which is what the synthesis hop did until it met a real kb. verifyChain alone still
# REQUIRES a certificate: an anonymous client is refused with
# "tlsv13 alert certificate required".
verifyChain = yes
sslVersionMin = TLSv1.3
EOF

cleanup() {
    [ -n "${srv_pid:-}"     ] && kill "$srv_pid"     2>/dev/null || true
    [ -n "${stunnel_pid:-}" ] && kill "$stunnel_pid" 2>/dev/null || true
}
trap cleanup TERM INT

log "starting embedder (${EMBEDDER_MODEL:-registry default}) on 127.0.0.1:$AIMEE_EMBEDDER_LOOPBACK_PORT"
EMBEDDER_PORT="$AIMEE_EMBEDDER_LOOPBACK_PORT" "$PY" "$SERVER" >&2 &
srv_pid=$!

log "starting mTLS terminator on :$AIMEE_EMBEDDER_PORT (client certificate required)"
stunnel "$STUNNEL_CONF" >&2 &
stunnel_pid=$!

# Either process dying takes the container with it, so the healthcheck and any
# depends_on gate see a stopped container rather than a half-up one.
while :; do
    if ! kill -0 "$srv_pid" 2>/dev/null; then
        log "embedder-server exited; stopping"
        cleanup
        wait "$srv_pid" 2>/dev/null || true
        exit 1
    fi
    if ! kill -0 "$stunnel_pid" 2>/dev/null; then
        log "stunnel exited; stopping (the embedder is only reachable through it)"
        cleanup
        wait "$stunnel_pid" 2>/dev/null || true
        exit 1
    fi
    sleep 5
done
