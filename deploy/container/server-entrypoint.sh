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
# Seed default dev-lifecycle workflows so autonomous development (default-on) can
# resolve "build" out of the box. Never clobber operator-authored workflows.
if [ -d /opt/aimee/defaults/workflows ]; then
    mkdir -p "$AIMEE_HOME/workflows"
    for wf in /opt/aimee/defaults/workflows/*.yaml; do
        [ -e "$wf" ] || continue
        dst="$AIMEE_HOME/workflows/$(basename "$wf")"
        [ -f "$dst" ] || cp "$wf" "$dst"
    done
fi
chown aimee:aimee "$AIMEE_HOME" "${AIMEE_WORKSPACES_DIR:-/var/lib/aimee-workspaces}" 2>/dev/null || true
[ -f "$AIMEE_HOME/aimee.yaml" ] && chown aimee:aimee "$AIMEE_HOME/aimee.yaml" 2>/dev/null || true
[ -f "$AIMEE_HOME/agents.json" ] && chown aimee:aimee "$AIMEE_HOME/agents.json" 2>/dev/null || true

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
log "pre-warming server-hosted OAuth CLIs (background)"
runuser -u aimee -- aimee-server --prewarm-cli-oauth >/dev/null 2>&1 &

log "starting aimee-server (socket=$SERVER_SOCK) as user aimee"
runuser -u aimee -- aimee-server --socket="$SERVER_SOCK" &
server_pid=$!

wait "$server_pid"
status=$?
log "aimee-server exited (status $status); shutting down webchat"
shutdown
exit "$status"
