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

# Preserve the container runtime's command-override contract.  The image has no
# default CMD, so arguments here are an operator-supplied command (for example,
# `aimee-server --version`) and must replace the managed server lifecycle below.
if [ "$#" -gt 0 ]; then
    exec "$@"
fi

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
WFE_SOCKET_WAIT_TENTHS="${AIMEE_WFE_SOCKET_WAIT_TENTHS:-150}"

# The server's worker threads need a 64 MB stack; the 8 MB container default
# overflows and SIGSEGVs on real queries. Raise the soft limit here (inherited
# by the runuser child); hard limit is unlimited on typical hosts. Best-effort.
ulimit -s 65536 2>/dev/null || true

# Preserve post-mortem evidence when the temporary C resource plane crashes.
# Required appliance profiles fail closed if either the resource limit or the
# storage policy cannot guarantee it; other runtimes stay warning-compatible.
. /usr/local/bin/core-storage.sh
if ! aimee_enable_core_dumps || ! aimee_prepare_core_storage; then
    exit 1
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
# Seed the default delegate roster (definitions only; keys are client-held and
# pushed per session) so delegates / the roundtable work out of the box. Never
# clobber an operator's agents.json.
if [ ! -f "$AIMEE_HOME/agents.json" ] && [ -f /opt/aimee/defaults/agents.json ]; then
    mkdir -p "$AIMEE_HOME"
    cp /opt/aimee/defaults/agents.json "$AIMEE_HOME/agents.json"
fi
# Seed default dev-lifecycle workflows so autonomous development (default-on) can
# resolve "build" out of the box. Shipped defaults are hash-tracked under
# .seeded/<name>: a fresh install is seeded and its hash recorded; on later
# starts an UNMODIFIED managed default (on-disk hash still equals the recorded
# seed hash) is refreshed when the image ships a newer one. An operator-edited
# default (hash diverged) or one of unknown provenance (no seed record and not
# already equal to the shipped default) is never clobbered.
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
}
seed_managed_defaults /opt/aimee/defaults/workflows .yaml "$AIMEE_HOME/workflows"
# The roundtable presets those workflows name. Seeded on the same terms: a gate
# cannot resolve a panel until its preset exists on disk.
seed_managed_defaults /opt/aimee/defaults/roundtables .json "$AIMEE_HOME/roundtables"
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true
[ -f "$AIMEE_HOME/agents.json" ] && chown aimee:aimee "$AIMEE_HOME/agents.json" 2>/dev/null || true
# Seeded as root; the server and its preset-editing API run as aimee.
[ -d "$AIMEE_HOME/roundtables" ] && chown -R aimee:aimee "$AIMEE_HOME/roundtables" 2>/dev/null || true

# The server-hosted OAuth CLIs (claude/codex) and their npm prefix live under the
# aimee home and MUST be writable by the unprivileged 'aimee' user that runs the
# login: codex writes auth.json into $CODEX_HOME ($AIMEE_HOME/.codex) and claude
# into $AIMEE_HOME/.claude on a successful device/browser login. If one of those
# dirs is root-owned (e.g. left behind by a root `docker exec ... codex login`),
# the CLI's token write fails and the login process exits WITHOUT persisting,
# surfacing to the operator only as "<vendor> login session ended without
# authenticating". Force them (and the npm install prefix) to the aimee user at
# boot so both `aimee agent setup codex-oauth` and `claude-oauth` can persist.
# Best-effort + only touches dirs that exist; the auth/config dirs are tiny and
# the npm prefix is chowned in place (cheap relative to the install it backs).
for cli_dir in .codex .claude .config .npm-global; do
    [ -e "$AIMEE_HOME/$cli_dir" ] && chown -R aimee:aimee "$AIMEE_HOME/$cli_dir" 2>/dev/null || true
done

. /usr/local/bin/runtime-web-lib.sh
. /usr/local/bin/plane-supervisor.sh

log() { printf '[server-entrypoint] %s\n' "$*"; }

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
trap 'shutdown' TERM INT

# Browser UI (root, PAM). Supplementary — a webchat crash must not take the
# container down; the server is the contract.
webchat_start

# Delegate-vault auto-provisioning. aimee-server seals operator-supplied delegate
# API keys into its server-principal vault at startup, so a fresh deploy's
# delegates/roundtables work with no manual `aimee vault set`. The source comes
# from the environment, which runuser preserves into the aimee-server child:
#   - AIMEE_DELEGATE_SECRETS_FILE=/run/secrets/aimee-delegates.json
#       a JSON object {"<agent>":"<api-key>", ...}. Mount it readable by the
#       container's "aimee" user (the server reads it after dropping privileges).
#   - AIMEE_DELEGATE_KEY_<AGENT>=<api-key>   (env-only convenience; agent name
#       lowercased, e.g. AIMEE_DELEGATE_KEY_MISTRAL).
# Non-destructive by default; set AIMEE_DELEGATE_SECRETS_OVERWRITE=1 to replace an
# existing vaulted key. Secrets are never written to AIMEE_HOME or logs.
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

log "pre-warming server-hosted OAuth CLIs (background)"
runuser -u aimee -- aimee-server --prewarm-cli-oauth >/dev/null 2>&1 &

log "starting aimee-server (socket=$SERVER_SOCK) as user aimee"
rm -f "$AIMEE_HOME/aimee-http.sock" "$AIMEE_WFE_HTTP_SOCKET"
# runuser/PAM resets selected resource limits, including RLIMIT_CORE, after the
# parent entrypoint configured them. Raise the soft limit again as the final
# unprivileged child operation so the actual server process inherits it.
runuser -u aimee -- sh -c 'set -eu; . /usr/local/bin/core-storage.sh; aimee_enable_core_dumps; aimee_verify_core_dump; exec aimee-server --socket="$1"' sh "$SERVER_SOCK" &
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
    runuser -u aimee -- sh -c 'set -eu; . /usr/local/bin/core-storage.sh; aimee_enable_core_dumps; exec aimee-wfe --home "$1" --socket "$2" --config "$3" --workflow-dir "$4" --agent-service-socket "$5"' sh \
        "$AIMEE_HOME" "$AIMEE_WFE_HTTP_SOCKET" "$AIMEE_HOME/aimee.yaml" \
        "$AIMEE_HOME/workflows" "$AIMEE_HOME/aimee-http.sock" &
    wfe_pid=$!
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
log "aimee-server exited (status $status); shutting down webchat"
shutdown
exit "$status"
