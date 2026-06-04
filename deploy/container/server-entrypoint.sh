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

SERVER_SOCK="${AIMEE_SERVER_SOCK:-/var/lib/aimee/aimee-server.sock}"
server_pid=""

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
