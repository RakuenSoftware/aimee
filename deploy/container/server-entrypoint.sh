#!/bin/sh
# Entrypoint for the aimee-server image with the co-located webchat browser UI.
#
# Runs as root so it can bootstrap the PAM login user and run aimee-webchat
# (which needs /etc/shadow access for pam_unix). aimee-server itself is dropped
# to the unprivileged "aimee" user. Lifecycle follows the SERVER: when it exits,
# webchat is torn down and the container exits with the server's status;
# SIGTERM/SIGINT are forwarded to both so `docker stop` is clean.
#
# POSIX sh (the image has no bash). Endpoints/DB come from the environment.
set -eu

AIMEE_HOME="${AIMEE_HOME:-/var/lib/aimee}"
SERVER_SOCK="${AIMEE_SERVER_SOCK:-/var/lib/aimee/aimee-server.sock}"
server_pid=""

# The server's worker threads need a 64 MB stack; the 8 MB container default
# overflows and SIGSEGVs on real queries. Raise the soft limit here (inherited
# by the runuser child); hard limit is unlimited on typical hosts. Best-effort.
ulimit -s 65536 2>/dev/null || true

# Seed the baked default config into AIMEE_HOME if absent. The server reads its
# /v1 listener settings (port + bearer) from $AIMEE_HOME/aimee.yaml; a
# bind-mounted (empty) volume would otherwise leave the listener unconfigured.
# Never clobber an operator's config. Done as root, then owned by aimee so it
# can read/rewrite it. On smoothfs tiers ownership is forced to 1000 regardless.
if [ ! -f "$AIMEE_HOME/aimee.yaml" ] && [ -f /opt/aimee/defaults/aimee.yaml ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/aimee.yaml "$AIMEE_HOME/aimee.yaml"
fi
# Seed the default delegate roster (definitions only; keys are client-held and
# pushed per session) so delegates / the roundtable work out of the box. Never
# clobber an operator's agents.json.
if [ ! -f "$AIMEE_HOME/agents.json" ] && [ -f /opt/aimee/defaults/agents.json ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/agents.json "$AIMEE_HOME/agents.json"
fi
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true
[ -f "$AIMEE_HOME/agents.json" ] && chown aimee:aimee "$AIMEE_HOME/agents.json" 2>/dev/null || true

. /usr/local/bin/webchat-lib.sh

log() { printf '[server-entrypoint] %s\n' "$*"; }

shutdown() {
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    webchat_stop
}
trap 'shutdown' TERM INT

# Browser UI (root, PAM). Supplementary — a webchat crash must not take the
# container down; the server is the contract.
webchat_start

log "starting aimee-server (socket=$SERVER_SOCK) as user aimee"
runuser -u aimee -- aimee-server --socket="$SERVER_SOCK" &
server_pid=$!

wait "$server_pid"
status=$?
log "aimee-server exited (status $status); shutting down webchat"
shutdown
exit "$status"
