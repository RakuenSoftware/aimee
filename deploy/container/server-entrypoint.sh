#!/bin/sh
# Entrypoint for the aimee-server image with the co-located webchat browser UI.
#
# Runs as root so it can bootstrap the PAM login user and run aimee-runtime-web
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
wfe_pid=""
export AIMEE_WFE_ENGINE="${AIMEE_WFE_ENGINE:-go}"
case "$AIMEE_WFE_ENGINE" in
    go) ;;
    *) printf '[server-entrypoint] fatal: WFE is Go-only; AIMEE_WFE_ENGINE must be go\n' >&2; exit 2 ;;
esac
export AIMEE_WFE_HTTP_SOCKET="${AIMEE_WFE_HTTP_SOCKET:-$AIMEE_HOME/aimee-wfe-http.sock}"
# Existing appliances may need to recover SQLite WAL state and refresh seeded
# workflow definitions before the C resource socket appears.  A real upgraded
# volume on the supported container path takes about 30 seconds, so the former
# 15-second default killed a healthy startup and left the persisted pid file
# behind.  Still fail early when the child exits, but allow bounded recovery.
WFE_SOCKET_WAIT_TENTHS="${AIMEE_WFE_SOCKET_WAIT_TENTHS:-1200}"

# The server's worker threads need a 64 MB stack; the 8 MB container default
# overflows and SIGSEGVs on real queries. Raise the soft limit here (inherited
# by the runuser child); hard limit is unlimited on typical hosts. Best-effort.
ulimit -s 65536 2>/dev/null || true

# Credential plaintext necessarily exists in process memory while a request is
# authenticated. Never persist that memory in a core image.
ulimit -c 0 2>/dev/null || true

# An explicit Docker command is not an aimee first boot, so it must run before
# image-only helpers are sourced. Never forward ambient credentials into that
# unrelated process; normal server startup below seals them into Vault first.
if [ "$#" -gt 0 ]; then
    for _secret_name in $(env | sed -n 's/=.*//p'); do
        case "$_secret_name" in
            AIMEE_DELEGATE_KEY_*|AIMEE_DB2_URL|AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE|*_TOKEN|*_SECRET|*_PASSWORD|*_PRIVATE_KEY|*_API_KEY|*_DSN)
                unset "$_secret_name"
                ;;
        esac
    done
    exec "$@"
fi

# Seed the baked default config into AIMEE_HOME if absent. The server reads its
# /v1 listener settings (port + bearer) from $AIMEE_HOME/aimee.yaml; a
# bind-mounted (empty) volume would otherwise leave the listener unconfigured.
# Never clobber an operator's config. Done as root, then owned by aimee so it
# can read/rewrite it. On smoothfs tiers ownership is forced to 1000 regardless.
if [ ! -f "$AIMEE_HOME/aimee.yaml" ] && [ -f /opt/aimee/defaults/aimee.yaml ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/aimee.yaml "$AIMEE_HOME/aimee.yaml"
fi
# agents.json intentionally starts absent. The onboarding wizard requires the
# operator to create the first agent; agent.add then creates the durable roster
# containing only that selected agent. Never invent provider entries on boot.
# Seed default dev-lifecycle workflows so autonomous development (default-on) can
# resolve "build" out of the box. Shipped defaults are hash-tracked under
# .seeded/<name>: a fresh install is seeded and its hash recorded; on later
# starts an UNMODIFIED managed default (on-disk hash still equals the recorded
# seed hash) is refreshed when the image ships a newer one. An operator-edited
# default (hash diverged) or one of unknown provenance (no seed record and not
# already equal to the shipped default) is never clobbered. A shipped default
# removed by a newer image is retired only when its recorded hash proves the
# operator never edited it.
# seed_managed_defaults <source-dir> <glob-suffix> <dest-dir>
seed_managed_defaults() {
    seed_src="$1"
    seed_ext="$2"
    seed_dst="$3"
    [ -d "$seed_src" ] || return 0
    mkdir -p "$seed_dst/.seeded"
    for shipped_file in "$seed_src"/*"$seed_ext"; do
        [ -e "$shipped_file" ] || continue
        base=$(basename "$shipped_file")
        dst="$seed_dst/$base"
        rec="$seed_dst/.seeded/$base"
        if ! command -v sha256sum >/dev/null 2>&1; then
            # No hasher available: fall back to conservative never-clobber seed.
            [ -f "$dst" ] || cp "$shipped_file" "$dst"
            continue
        fi
        shipped=$(sha256sum "$shipped_file" | cut -d' ' -f1)
        if [ ! -f "$dst" ]; then
            cp "$shipped_file" "$dst" && printf '%s\n' "$shipped" > "$rec"
        elif [ -f "$rec" ]; then
            disk=$(sha256sum "$dst" | cut -d' ' -f1)
            if [ "$disk" = "$(cat "$rec")" ] && [ "$disk" != "$shipped" ]; then
                cp "$shipped_file" "$dst" && printf '%s\n' "$shipped" > "$rec"
            fi
        elif [ "$(sha256sum "$dst" | cut -d' ' -f1)" = "$shipped" ]; then
            # No record yet, but already identical to the shipped default: adopt
            # it as managed so a future image can refresh it.
            printf '%s\n' "$shipped" > "$rec"
        fi
    done
    if command -v sha256sum >/dev/null 2>&1; then
        for rec in "$seed_dst/.seeded/"*"$seed_ext"; do
            [ -f "$rec" ] || continue
            base=$(basename "$rec")
            [ -f "$seed_src/$base" ] && continue
            dst="$seed_dst/$base"
            if [ ! -f "$dst" ]; then
                rm -f -- "$rec"
                continue
            fi
            disk=$(sha256sum "$dst" | cut -d' ' -f1)
            if [ "$disk" = "$(cat "$rec")" ]; then
                rm -f -- "$dst" "$rec"
            fi
        done
    fi
}
seed_managed_defaults /opt/aimee/defaults/workflows .yaml "$AIMEE_HOME/workflows"
# The roundtable presets those workflows name. Seeded on the same terms: a gate
# cannot resolve a panel until its preset exists on disk.
seed_managed_defaults /opt/aimee/defaults/roundtables .json "$AIMEE_HOME/roundtables"
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true
[ -f "$AIMEE_HOME/agents.json" ] && chown aimee:aimee "$AIMEE_HOME/agents.json" 2>/dev/null || true
# Seeded as root; the Go workflow engine creates its registry lock and immutable
# definition snapshots here while running as the unprivileged aimee user.
[ -d "$AIMEE_HOME/workflows" ] && chown -R aimee:aimee "$AIMEE_HOME/workflows" 2>/dev/null || true
# Seeded as root; the server and its preset-editing API run as aimee.
[ -d "$AIMEE_HOME/roundtables" ] && chown -R aimee:aimee "$AIMEE_HOME/roundtables" 2>/dev/null || true

# Vendor OAuth CLIs require a HOME-like directory while completing their device
# flow. Keep that short-lived transport on /run (container tmpfs), never on the
# persistent AIMEE_HOME volume. The server seals the result in Vault and removes
# the transport file before reporting authentication complete.
AIMEE_OAUTH_RUNTIME_DIR="${AIMEE_OAUTH_RUNTIME_DIR:-/run/aimee/oauth-login}"
case "$AIMEE_OAUTH_RUNTIME_DIR" in
    /*) ;;
    *) printf '[server-entrypoint] fatal: AIMEE_OAUTH_RUNTIME_DIR must be absolute\n' >&2; exit 2 ;;
esac
mkdir -p "$AIMEE_OAUTH_RUNTIME_DIR"
chown aimee:aimee "$AIMEE_OAUTH_RUNTIME_DIR"
chmod 0700 "$AIMEE_OAUTH_RUNTIME_DIR"
export AIMEE_OAUTH_RUNTIME_DIR

# The non-secret OAuth CLI installation lives under AIMEE_HOME. Historical
# images also wrote credentials beneath .codex/.claude; leave those directories
# readable by the unprivileged server so its one-time migration can seal and
# delete them. New login credentials are written only to AIMEE_OAUTH_RUNTIME_DIR.
# Best-effort + only touches directories that already exist.
for cli_dir in .codex .claude .config .npm-global; do
    [ -e "$AIMEE_HOME/$cli_dir" ] && chown -R aimee:aimee "$AIMEE_HOME/$cli_dir" 2>/dev/null || true
done

. /usr/local/bin/runtime-web-lib.sh
. /usr/local/bin/plane-supervisor.sh

# Consume deployment credentials exactly once. Kubernetes/Docker may supply a
# Secret as environment at first boot; the helper seals it into Vault, and this
# parent removes it before any unrelated bootstrap helper, webchat, the C server,
# or the Go WFE is launched.
if [ -n "${AIMEE_DELEGATE_SECRETS_FILE:-}" ]; then
    printf '[server-entrypoint] fatal: AIMEE_DELEGATE_SECRETS_FILE is unsupported; use first-boot AIMEE_DELEGATE_KEY_<AGENT> variables\n' >&2
    exit 2
fi
runuser -u aimee -- aimee-server --bootstrap-vault-env
for _secret_name in $(env | sed -n 's/=.*//p'); do
    case "$_secret_name" in
        AIMEE_DELEGATE_KEY_*|AIMEE_DB2_URL|AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE|*_TOKEN|*_SECRET|*_PASSWORD|*_PRIVATE_KEY|*_API_KEY|*_DSN)
            unset "$_secret_name"
            ;;
    esac
done
webchat_prepare

log() { printf '[server-entrypoint] %s\n' "$*"; }

# Compose the one line an operator reads when the container comes down. Kept
# pure (args in, string out, no globals) so it can be tested without a container.
#
# Two failures made a routine `docker stop` look like a crash:
#   - the plane name was hardcoded to aimee-server, so a Go WFE exit was
#     reported against the C server and the search started in the wrong process
#   - runuser turns a caught SIGTERM into a plain exit 1 with the signal
#     discarded, so "exited (status 1)" was indistinguishable from a real
#     failure, and pointed at a core dump that is never written for exit(1)
plane_exit_message() {
    _pem_first=$1
    _pem_status=$2
    _pem_terminating=$3
    case $_pem_first in
        wfe) _pem_plane=aimee-wfe ;;
        *) _pem_plane=aimee-server ;;
    esac
    if [ "$_pem_terminating" = 1 ]; then
        printf '%s stopped on termination signal (status %s); shutting down webchat' \
            "$_pem_plane" "$_pem_status"
    else
        printf '%s exited (status %s); shutting down webchat' "$_pem_plane" "$_pem_status"
    fi
}

shutdown() {
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    [ -n "$wfe_pid" ] && kill "$wfe_pid" 2>/dev/null || true
	_wait=0
	while { [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; } || { [ -n "$wfe_pid" ] && kill -0 "$wfe_pid" 2>/dev/null; }; do
		[ "$_wait" -ge 50 ] && break
		_wait=$((_wait + 1)); sleep 0.1
	done
	[ -n "$server_pid" ] && kill -KILL "$server_pid" 2>/dev/null || true
	[ -n "$wfe_pid" ] && kill -KILL "$wfe_pid" 2>/dev/null || true
    webchat_stop
}
# A plane that is asked to stop reports the same exit 1 as a plane that broke:
# runuser catches the signal, prints "Session terminated, killing shell...", and
# exits 1 with the signal discarded. Without this flag the final log calls an
# ordinary `docker stop` an "exited (status 1)" failure, which reads as a crash
# and sends whoever is on call hunting a core dump that was never written.
# Record that WE were signalled, so the exit line can say so.
terminating=0
on_signal() {
    terminating=1
    log "termination signal received; stopping both planes"
    shutdown
}
trap 'on_signal' TERM INT

# Browser UI (root, PAM). Its login provisioning already ran before the
# credential scrub; launch now with a clean environment.
webchat_start

# Pre-warm the server-hosted OAuth CLIs (claude/codex) in the BACKGROUND so the
# first `aimee agent setup *-oauth` is instant instead of waiting on (or timing
# out against) a cold `npm i -g` — the failure mode on a freshly-deployed,
# empty-home container. Idempotent (probe-first) + best-effort: it never blocks
# or fails the server start, and runs as the same 'aimee' user that owns the
# install prefix ($AIMEE_HOME/.npm-global). The lazy install on first setup still
# covers it if this hasn't finished yet.
# Delegate sandbox: when the host Docker socket is bind-mounted in (so aimee-server
# can spawn per-delegate containers), grant the unprivileged 'aimee' user the
# socket's group. runuser re-initialises supplementary groups from /etc/group via
# initgroups(), so a container `--group-add <gid>` is dropped for the server child
# unless 'aimee' is actually a member in /etc/group. Without this the docker backend
# is INERT and delegates silently run on the HOST (see the delegate-sandbox posture
# log). Root here; the runuser calls below then pick the group up.
for _dsock in /var/run/docker.sock /run/docker.sock; do
    [ -S "$_dsock" ] || continue
    _dgid=$(stat -c %g "$_dsock" 2>/dev/null) || continue
    { [ -n "$_dgid" ] && [ "$_dgid" != 0 ]; } || continue
    _dgrp=$(getent group "$_dgid" | cut -d: -f1)
    if [ -z "$_dgrp" ]; then
        _dgrp=dockerhost
        groupadd -g "$_dgid" "$_dgrp" 2>/dev/null || true
    fi
    if ! id -nG aimee 2>/dev/null | tr ' ' '\n' | grep -qx "$_dgrp"; then
        usermod -aG "$_dgrp" aimee 2>/dev/null || true
    fi
    log "delegate sandbox: granted 'aimee' the docker socket group ($_dgrp/$_dgid)"
    break
done

log "starting aimee-server (socket=$SERVER_SOCK) as user aimee"
rm -f "$AIMEE_HOME/aimee-http.sock" "$AIMEE_WFE_HTTP_SOCKET"
runuser -u aimee -- sh -c 'set -eu; ulimit -c 0 2>/dev/null || true; exec aimee-server --socket="$1"' sh "$SERVER_SOCK" &
server_pid=$!

if [ "$AIMEE_WFE_ENGINE" = go ]; then
    if [ ! -x /usr/local/bin/aimee-wfe ]; then
        log "fatal: AIMEE_WFE_ENGINE=go but /usr/local/bin/aimee-wfe is unavailable"
        shutdown
        exit 1
    fi
    # The C process is a temporary stateless agent resource plane. Wait for its
    # HTTP socket, then put all WFE state/admission/execution on the Go socket.
    _wait=0
    while [ ! -S "$AIMEE_HOME/aimee-http.sock" ] && [ "$_wait" -lt "$WFE_SOCKET_WAIT_TENTHS" ]; do
        kill -0 "$server_pid" 2>/dev/null || break
        _wait=$((_wait + 1))
        sleep 0.1
    done
    if ! kill -0 "$server_pid" 2>/dev/null || [ ! -S "$AIMEE_HOME/aimee-http.sock" ]; then
        log "fatal: C agent resource plane did not become ready"
        shutdown
        exit 1
    fi
    log "starting Go WFE control plane (socket=$AIMEE_WFE_HTTP_SOCKET)"
    runuser -u aimee -- sh -c 'set -eu; ulimit -c 0 2>/dev/null || true; exec aimee-wfe --home "$1" --socket "$2" --config "$3" --workflow-dir "$4" --agent-service-socket "$5"' sh \
        "$AIMEE_HOME" "$AIMEE_WFE_HTTP_SOCKET" "$AIMEE_HOME/aimee.yaml" \
        "$AIMEE_HOME/workflows" "$AIMEE_HOME/aimee-http.sock" &
    wfe_pid=$!

    # Start this only after the resource plane owns the current pid file.  On a
    # restart, a stale persisted pid can be reused by the first child.  The
    # prewarm command is also named `aimee-server`, so launching it first made
    # the real server mistake that helper for an already-running server.
    log "pre-warming server-hosted OAuth CLIs (background)"
    runuser -u aimee -- aimee-server --prewarm-cli-oauth >/dev/null 2>&1 &
fi

if [ -n "$wfe_pid" ]; then
    status=0
    aimee_supervise_plane_unit "$server_pid" "$wfe_pid" || status=$?
    first=$AIMEE_FIRST_EXIT
    log "$first plane exited; terminating its peer so the container restarts as one unit"
    shutdown
else
    if wait "$server_pid"; then status=0; else status=$?; fi
fi
log "$(plane_exit_message "${first:-}" "$status" "$terminating")"
shutdown
exit "$status"
