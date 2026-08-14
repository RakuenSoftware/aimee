#!/bin/sh
# Entrypoint for the aimee-server image with the co-located webchat browser UI.
#
# Runs as root only for one-time legacy credential erasure and child supervision.
# Webchat retains root solely as the kernel-attested UDS identity trusted by the
# server; aimee-server runs as the unprivileged "aimee" user. Lifecycle follows
# the SERVER: when it exits,
# webchat is torn down and the container exits with the server's status;
# SIGTERM/SIGINT are forwarded to both so `docker stop` is clean.
#
# POSIX sh (the image has no bash). Endpoints/DB come from the environment.
set -eu

vault_bootstrapped=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --aimee-internal-vault-bootstrapped) vault_bootstrapped=1; shift ;;
        *) break ;;
    esac
done

AIMEE_HOME="${AIMEE_HOME:-/var/lib/aimee}"
export AIMEE_HOME
SERVER_SOCK="${AIMEE_SERVER_SOCK:-/var/lib/aimee/aimee-server.sock}"
server_pid=""
wfe_pid=""
module_pid=""

# The one-shot bootstrap begins with a narrowly scoped legacy-volume ownership
# repair, then drops privileges before it touches credentials. Disable core
# files in the supervising shell first so the privileged phase cannot persist
# inherited first-boot secrets either.
ulimit -c 0 2>/dev/null || true

# Consume deployment credentials before invoking any unrelated child process.
# Kubernetes/Docker environment injection is accepted only as first-boot
# transport: the short-lived server seals it into Vault, then this PID removes
# every credential-shaped variable before it starts tini, an explicit command,
# bootstrap helpers, webchat, the C server, or the Go WFE. The internal second
# pass is intentionally idempotent so an externally supplied sentinel cannot
# bypass ingestion.
if [ -n "${AIMEE_DELEGATE_SECRETS_FILE:-}" ]; then
    printf '[server-entrypoint] fatal: AIMEE_DELEGATE_SECRETS_FILE is unsupported; use first-boot AIMEE_DELEGATE_KEY_<AGENT> variables\n' >&2
    exit 2
fi
if [ -n "${AIMEE_WEBCHAT_TLS_KEY:-}" ]; then
    printf '[server-entrypoint] fatal: AIMEE_WEBCHAT_TLS_KEY files are forbidden; TLS private keys must be imported into Vault\n' >&2
    exit 2
fi
if [ -n "${AIMEE_SERVER_MGMT_TLS_KEY:-}" ] || [ -n "${AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY:-}" ]; then
    printf '[server-entrypoint] fatal: management private-key files are forbidden; inject AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY and AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY as first-boot Vault inputs\n' >&2
    exit 2
fi
if ! aimee-server --bootstrap-vault-env --drop-user aimee; then
    printf '[server-entrypoint] fatal: Vault bootstrap failed; refusing to start child processes\n' >&2
    exit 1
fi
_secret_names=$(runuser -u aimee -- aimee-server --list-credential-env-names)
had_credential_env=0
for _secret_name in $_secret_names; do
    eval "_secret_was_set=\${${_secret_name}+x}"
    [ "$_secret_was_set" = x ] && had_credential_env=1
    unset "$_secret_name"
done
unset _secret_was_set

# Legacy credential files are a migration source, never runtime storage. Seal
# and erase them even for a Docker command override; an internal restart marker
# supplied by an external caller must not be able to bypass this boundary.
runtime_web_lib=/usr/local/bin/runtime-web-lib.sh
if [ ! -r "$runtime_web_lib" ]; then
    entrypoint_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
    runtime_web_lib="$entrypoint_dir/runtime-web-lib.sh"
fi
[ -r "$runtime_web_lib" ] || {
    printf '[server-entrypoint] fatal: runtime-web credential migration helper is unavailable\n' >&2
    exit 2
}
. "$runtime_web_lib"
webchat_migrate_legacy_credentials

# Operator control over which optional modules attach to the bus. Resolved the
# same way as the runtime-web helper: installed path first, then alongside this
# script for a source checkout.
optional_modules_lib=/usr/local/bin/optional-modules-lib.sh
if [ ! -r "$optional_modules_lib" ]; then
    entrypoint_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
    optional_modules_lib="$entrypoint_dir/optional-modules-lib.sh"
fi
[ -r "$optional_modules_lib" ] || {
    printf '[server-entrypoint] fatal: optional-module helper is unavailable\n' >&2
    exit 2
}
. "$optional_modules_lib"

# An explicit Docker command is unrelated to normal server startup. It still
# follows Vault ingestion above and receives a credential-free environment.
if [ "$#" -gt 0 ]; then
    exec "$@"
fi
if [ "$vault_bootstrapped" -eq 0 ] || [ "$had_credential_env" -eq 1 ]; then
    # Migrate any legacy browser credential files only after first-boot env
    # values have been sealed and scrubbed. The sealed pair is first-boot
    # transport: webchat_provision_bootstrap_account reads it back out and
    # provisions a real PAM account, which is the only thing authentication
    # consults thereafter. Force a clean process image whenever this invocation
    # inherited credentials, even if an external caller supplied the internal
    # marker.
    webchat_prepare
    exec /usr/bin/tini -- aimee-server-entrypoint --aimee-internal-vault-bootstrapped
fi

export AIMEE_WFE_ENGINE="${AIMEE_WFE_ENGINE:-go}"
case "$AIMEE_WFE_ENGINE" in
    go) ;;
    *) printf '[server-entrypoint] fatal: WFE is Go-only; AIMEE_WFE_ENGINE must be go\n' >&2; exit 2 ;;
esac
export AIMEE_WFE_HTTP_SOCKET="${AIMEE_WFE_HTTP_SOCKET:-$AIMEE_HOME/aimee-wfe-http.sock}"
export AIMEE_MODULE_BUS_SOCKET="${AIMEE_MODULE_BUS_SOCKET:-$AIMEE_HOME/server-module-bus.sock}"
MODULE_MANIFEST="${AIMEE_MODULE_MANIFEST:-/opt/aimee/module-grants/server.modules}"
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

# Seed the baked default config into AIMEE_HOME if absent. It contains only the
# public /v1 listener policy; API and TLS private credentials live in Vault. A
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

# Admission policy must exist before the C host opens its local bus. Seed only
# missing grants so an operator may tighten a persisted policy without the next
# image start overwriting it.
mkdir -p "$AIMEE_HOME/modules.d/server"
# Overridable ONLY so the seeding rules can be tested against a fixture
# directory; production always uses the image path.
AIMEE_MODULE_GRANT_SRC="${AIMEE_MODULE_GRANT_SRC:-/opt/aimee/module-grants/server}"

# >>> module-grant-seeding (extracted by src/tests/test_module_grants.sh)
# Telling a STALE IMAGE DEFAULT apart from an OPERATOR'S POLICY.
#
# Seeding deliberately never overwrites a persisted grant, so a module that
# gains a stage cannot serve it until someone edits the file by hand — and the
# failure is silent at the bus (the kind is simply refused). Blindly adopting
# the shipped stage list would fix that by trampling deliberate policy, which is
# a privilege expansion and strictly worse.
#
# The two cases are distinguishable if we record what the image wrote at seed
# time: a file still byte-identical to what we seeded was never touched by an
# operator, so replacing it restores an image default rather than overriding a
# decision. Anything else keeps the warning and waits for a human.
#
# For pre-record installations, exact historical image defaults are also
# recognisable: the module name, old stage list, and every non-stage policy line
# must match a known transition. A nearby operator edit still fails that match
# and remains untouched.
# Recorded under .seeded/<name>, the same convention seed_managed_defaults uses
# above. The policy loader selects entries by a ".grant" suffix, so this
# subdirectory is skipped rather than parsed -- worth stating, because the loader
# rejects the WHOLE directory on one bad entry.
grant_seed_record() { printf '%s/.seeded/%s\n' "$(dirname "$1")" "$(basename "$1")"; }

grant_record_seed() {
    command -v sha256sum >/dev/null 2>&1 || return 0
    _rec=$(grant_seed_record "$1")
    mkdir -p "$(dirname "$_rec")" 2>/dev/null || return 0
    sha256sum "$1" | cut -d' ' -f1 > "$_rec" 2>/dev/null || return 0
    chmod 0600 "$_rec" 2>/dev/null || true
}

grant_untouched_since_seed() {
    command -v sha256sum >/dev/null 2>&1 || return 1
    _rec=$(grant_seed_record "$1")
    [ -f "$_rec" ] || return 1
    [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$(cat "$_rec" 2>/dev/null)" ]
}

grant_known_historical_default() { # <persisted> <shipped>
    _persisted=$1
    _shipped=$2
    # These modules originally shipped with one stage and later gained a second.
    # Match the entire remaining policy so an operator change to identity,
    # executable, or any other capability is never mistaken for an old image
    # default.
    _historical="$(basename "$_persisted"):$(grep '^serve=' "$_persisted" 2>/dev/null || true)"
    case "$_historical" in
        git.grant:serve=7425|skills.grant:serve=7681|roundtable.grant:serve=9473|benchmarks.grant:serve=10497) ;;
        *) return 1 ;;
    esac
    [ "$(sed '/^serve=/d' "$_persisted")" = "$(sed '/^serve=/d' "$_shipped")" ]
}

for module_grant in "$AIMEE_MODULE_GRANT_SRC"/*.grant; do
    [ -f "$module_grant" ] || continue
    grant_target="$AIMEE_HOME/modules.d/server/$(basename "$module_grant")"
    if [ ! -e "$grant_target" ]; then
        cp "$module_grant" "$grant_target"
        grant_record_seed "$grant_target"
        continue
    fi
    # A module that gains a stage in a new image cannot serve it under a grant
    # persisted before that stage existed, and the failure is silent: the daemon
    # simply reports the module as not serving that kind. Copying over the
    # operator's file would defeat the whole point of seeding only what is
    # missing, so say so instead and let them decide.
    # No record yet but already byte-identical to what this image ships: adopt it
    # as managed so a LATER image can refresh it. Existing installs come under
    # management this way instead of staying unmanageable forever.
    if [ ! -f "$(grant_seed_record "$grant_target")" ] && cmp -s "$module_grant" "$grant_target"; then
        grant_record_seed "$grant_target"
    fi
    shipped_serve=$(grep '^serve=' "$module_grant" 2>/dev/null || true)
    persisted_serve=$(grep '^serve=' "$grant_target" 2>/dev/null || true)
    if [ "$shipped_serve" != "$persisted_serve" ]; then
        # log() is not defined this early in the script, so match its format.
        if grant_untouched_since_seed "$grant_target" ||
           grant_known_historical_default "$grant_target" "$module_grant"; then
            # Still exactly what this installation seeded, so the difference is
            # image drift and adopting it overrides nobody.
            printf '[server-entrypoint] %s grant is a known unmodified image default and this image ships %s; adopting it\n' \
                "$(basename "$module_grant" .grant)" "${shipped_serve:-<none>}" >&2
            cp "$module_grant" "$grant_target"
            grant_record_seed "$grant_target"
        else
            printf '[server-entrypoint] warning: %s grant differs from this image\n' \
                "$(basename "$module_grant" .grant)" >&2
            printf '[server-entrypoint]   persisted: %s\n' "${persisted_serve:-<none>}" >&2
            printf '[server-entrypoint]   shipped:   %s\n' "${shipped_serve:-<none>}" >&2
            printf '[server-entrypoint]   this file was edited after seeding, so it is treated as operator policy\n' >&2
            printf '[server-entrypoint]   stages only in the shipped grant are refused until %s is updated\n' \
                "$grant_target" >&2
        fi
    fi
done

# Reconcile grants whose pinned executable this image does not ship.
#
# The loader realpath()s executable= and rejects the ENTIRE policy directory if
# any single entry is unresolvable, so one stale grant is not a degraded module —
# it is a daemon that will not boot. That is exactly what an upgrade produces:
# seeding never overwrites a persisted grant, so a module that MOVED (workflows
# is hosted by aimee-wfe now, not spawned as a multicall binary) or was REMOVED
# leaves behind a grant pinning a path that no longer exists.
#
# A pinned path is an image fact, not an operator policy choice, so repairing it
# does not override anyone's intent — whereas leaving it bricks the server. Where
# the image still ships a grant for that module, adopt it; where it does not, the
# module is gone and so is its grant. Both are logged, because silently rewriting
# admission policy would be worse than the failure.
for grant_target in "$AIMEE_HOME"/modules.d/server/*.grant; do
    [ -f "$grant_target" ] || continue
    pinned=$(sed -n 's/^executable=//p' "$grant_target" | head -1)
    [ -n "$pinned" ] && [ ! -x "$pinned" ] || continue
    shipped="$AIMEE_MODULE_GRANT_SRC/$(basename "$grant_target")"
    if [ -f "$shipped" ]; then
        printf '[server-entrypoint] %s grant pins %s, which this image does not ship; adopting the shipped grant
'             "$(basename "$grant_target" .grant)" "$pinned" >&2
        cp "$shipped" "$grant_target"
        # Now byte-identical to the image default again, so a later stage change
        # is recognisable as drift rather than as an operator edit.
        grant_record_seed "$grant_target"
    else
        printf '[server-entrypoint] %s grant pins %s and this image ships no such module; removing the stale grant
'             "$(basename "$grant_target" .grant)" "$pinned" >&2
        rm -f "$grant_target" "$(grant_seed_record "$grant_target")"
    fi
done
# <<< module-grant-seeding
# The root entrypoint creates modules.d before dropping to the aimee user.  The
# daemon must be able to traverse that 0700 parent in order to load the strict
# grant policy; owning only its server child leaves the parent root-only and
# makes every otherwise-valid grant look like an invalid policy.
chown aimee:aimee "$AIMEE_HOME/modules.d" 2>/dev/null || true
chown -R aimee:aimee "$AIMEE_HOME/modules.d/server" 2>/dev/null || true
chmod 0700 "$AIMEE_HOME/modules.d" "$AIMEE_HOME/modules.d/server" 2>/dev/null || true
chmod 0600 "$AIMEE_HOME/modules.d/server/"*.grant 2>/dev/null || true

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

. /usr/local/bin/plane-supervisor.sh

webchat_prepare

# Diagnostics go to stderr. Two callers capture a helper's stdout as a VALUE
# (`MODULE_MANIFEST="$(apply_optional_modules ...)"`), and those helpers report
# through this function, so a log line on stdout is captured as part of the value.
# When it was stdout, gating any optional module handed the module supervisor
# "<diagnostic>\n<path>" as its manifest; the supervisor found no such file, called
# it fatal, and exited — stopping EVERY module, which is the opposite of what
# enabling one asks for. optional-modules-lib.sh documents this contract
# ("Diagnostics go to stderr via log()") and module-supervisor.sh already honours
# it. Docker captures both streams, so operators see these lines either way.
log() { printf '[server-entrypoint] %s\n' "$*" >&2; }

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
	[ -n "$module_pid" ] && kill "$module_pid" 2>/dev/null || true
	_wait=0
	while { [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; } || { [ -n "$wfe_pid" ] && kill -0 "$wfe_pid" 2>/dev/null; } || { [ -n "$module_pid" ] && kill -0 "$module_pid" 2>/dev/null; }; do
		[ "$_wait" -ge 50 ] && break
		_wait=$((_wait + 1)); sleep 0.1
	done
	[ -n "$server_pid" ] && kill -KILL "$server_pid" 2>/dev/null || true
	[ -n "$wfe_pid" ] && kill -KILL "$wfe_pid" 2>/dev/null || true
	[ -n "$module_pid" ] && kill -KILL "$module_pid" 2>/dev/null || true
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

# Browser UI is the kernel-attested root UDS peer and obtains only fixed Vault
# records through short-lived helpers dropped to the Vault owner. Its inherited
# environment is credential-free.
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

# Delegate sandbox host-path translation.
#
# aimee-server drives a SIBLING docker daemon through the socket above, so a bind
# SOURCE like /var/lib/aimee/<workspace> — which exists in THIS container — does
# not exist on the daemon's host. Docker then creates it empty rather than
# failing, and the delegate gets a sandbox whose workspace mount is an empty
# directory: its worktree is simply not there, so every file tool answers
# "cannot open" for files that plainly exist.
#
# docker_translate_host_source() already handles this, but only when
# AIMEE_SANDBOX_HOST_MOUNTS names the mapping. The plugin deploys set it from
# their own bind mounts; the COMPOSE deploys never did, which is why the managed
# topology could not run a tool-using delegate at all.
#
# Derive it from this container's own mounts instead of hardcoding a path: the
# volume host paths depend on docker's data-root and the compose project name,
# neither of which belongs in an image. Skip entries whose source equals its
# destination (a plain host bind such as the docker socket needs no translation).
# Any failure leaves the variable unset, which is exactly the previous behaviour.
if [ -z "${AIMEE_SANDBOX_HOST_MOUNTS:-}" ] && [ -S "${_dsock:-/var/run/docker.sock}" ]; then
    _self=$(hostname 2>/dev/null || true)
    if [ -n "$_self" ]; then
        _map=$(docker inspect "$_self" \
                   --format '{{range .Mounts}}{{.Destination}}={{.Source}}{{println}}{{end}}' \
                   2>/dev/null |
               awk -F= 'NF == 2 && $1 != $2 && $1 ~ /^\// && $2 ~ /^\// {
                            printf "%s%s=%s", sep, $1, $2; sep = ","
                        }')
        if [ -n "$_map" ]; then
            export AIMEE_SANDBOX_HOST_MOUNTS="$_map"
            log "delegate sandbox: derived host-path map from own mounts ($_map)"
        else
            log "delegate sandbox: could not derive a host-path map; delegate workspace mounts may be empty if this daemon is a sibling"
        fi
    fi
    unset _self _map 2>/dev/null || true
fi

# Derive the managed compose `.env` from config, every start.
#
# The managed deployment's identity -- which kb image variant, which embedder --
# used to live ONLY in the running container's Config.Env, put there by whichever
# shell first ran compose. A reboot is safe (restart=unless-stopped restarts the
# same container object with its env intact); a RECREATE is not, and a recreate is
# what every image upgrade does. Recreating with a different caller environment
# silently reinterpolates AIMEE_KB_VARIANT to nothing, which resolves the kb image
# to the EMBEDDERLESS aimee-kb -- a working deployment losing its embedder with no
# error anywhere.
#
# Compose reads `.env` from the project directory on its own, so writing it here
# makes every later `docker compose up -d` correct without the caller supplying
# anything: swapping an image becomes a restart rather than a reconfiguration.
#
# WRITTEN FRESH RATHER THAN PERSISTED. /opt/aimee/deploy is image content, not a
# mount, so this file cannot survive to contradict a config changed while the
# container was down. Config is the single source of truth; this is only its
# projection. A failure here is not fatal -- the server's own deploy path builds
# its child environment directly and still works -- so warn and carry on rather
# than refuse to start a server over a file only compose reads.
DEPLOY_ENV_DIR="${AIMEE_DEPLOY_COMPOSE_DIR:-/opt/aimee/deploy}"
if [ -d "$DEPLOY_ENV_DIR" ]; then
    if aimee-server --emit-deploy-env >"$DEPLOY_ENV_DIR/.env.tmp" 2>/dev/null; then
        chmod 0600 "$DEPLOY_ENV_DIR/.env.tmp" 2>/dev/null || true
        mv -f "$DEPLOY_ENV_DIR/.env.tmp" "$DEPLOY_ENV_DIR/.env"
        log "wrote managed compose env ($DEPLOY_ENV_DIR/.env) from config"
    else
        rm -f "$DEPLOY_ENV_DIR/.env.tmp" 2>/dev/null || true
        log "WARNING: could not derive $DEPLOY_ENV_DIR/.env; a manual 'docker compose up -d' may recreate the kb with the wrong image variant"
    fi
fi

log "starting aimee-server (socket=$SERVER_SOCK) as user aimee"
rm -f "$AIMEE_HOME/aimee-http.sock" "$AIMEE_WFE_HTTP_SOCKET"
runuser -u aimee -- sh -c 'set -eu; ulimit -c 0 2>/dev/null || true; exec aimee-server --socket="$1"' sh "$SERVER_SOCK" &
server_pid=$!
# The shipped manifest is decided when the image is built and cannot know what
# this operator wants running, so apply the operator's AIMEE_MODULE_<ID> choices
# over it. This replaces a hard-coded roundtable-only branch that could enable a
# module but never disable one; AIMEE_MODULE_ROUNDTABLE keeps working exactly as
# before, and every other optional module now has the same control.
#
# roundtable remains the case that matters most: the daemon has no other
# implementation of roundtable.review since the proxy was deleted, so with the
# module absent the review route reports the module as not attached however the
# operator configured the feature.
MODULE_MANIFEST="$(apply_optional_modules server "$MODULE_MANIFEST" "$AIMEE_HOME")"
runuser -u aimee -- env AIMEE_HOME="$AIMEE_HOME" \
    module-supervisor.sh server "$AIMEE_MODULE_BUS_SOCKET" "$MODULE_MANIFEST" &
module_pid=$!

if [ "$AIMEE_WFE_ENGINE" = go ]; then
    if [ ! -x /usr/local/bin/aimee-wfe ]; then
        log "fatal: AIMEE_WFE_ENGINE=go but /usr/local/bin/aimee-wfe is unavailable"
        shutdown
        exit 1
    fi
    # The C daemon remains the host for the module bus, mTLS/MCP and external
    # HTTP API. This particular Unix resource-plane socket is passed to the Go
    # WFE only for credentialed forge operations; delegate execution uses the
    # Go delegates process over the module bus.
    _wait=0
    while [ ! -S "$AIMEE_HOME/aimee-http.sock" ] && [ "$_wait" -lt "$WFE_SOCKET_WAIT_TENTHS" ]; do
        kill -0 "$server_pid" 2>/dev/null || break
        _wait=$((_wait + 1))
        sleep 0.1
    done
    if ! kill -0 "$server_pid" 2>/dev/null || [ ! -S "$AIMEE_HOME/aimee-http.sock" ]; then
        # Say which of the two it was and how long we waited. These fail for very
        # different reasons -- a dead process means the server exited (its own log
        # says why), while a live process with no socket means startup is blocked
        # before it listens, typically on a dependency such as an unresponsive kb.
        # The bare message sent me looking at the wrong one for some time.
        if kill -0 "$server_pid" 2>/dev/null; then
            log "fatal: aimee-server is running but never created $AIMEE_HOME/aimee-http.sock after $((_wait / 10))s"
            log "  startup is blocked before the listener; check $AIMEE_HOME/server.log for the last"
            log "  step reached, and whether a dependency (e.g. aimee-kb) is reachable"
        else
            log "fatal: aimee-server exited during startup after $((_wait / 10))s; see $AIMEE_HOME/server.log"
        fi
        shutdown
        exit 1
    fi
    log "starting Go WFE control plane (socket=$AIMEE_WFE_HTTP_SOCKET)"
    # The WFE is the workflows bus principal: it serves the advance decision and
    # the control stage the C resource plane calls. The module supervisor does
    # not spawn a workflows process (the contract marks it hosted_by=wfe), because
    # the bus denies a live duplicate of a principal. Pass the bus socket
    # explicitly rather than relying on runuser's environment handling.
    runuser -u aimee -- sh -c 'set -eu; ulimit -c 0 2>/dev/null || true; export AIMEE_MODULE_BUS_SOCKET="$6"; exec aimee-wfe --home "$1" --socket "$2" --config "$3" --workflow-dir "$4" --forge-service-socket "$5"' sh \
        "$AIMEE_HOME" "$AIMEE_WFE_HTTP_SOCKET" "$AIMEE_HOME/aimee.yaml" \
        "$AIMEE_HOME/workflows" "$AIMEE_HOME/aimee-http.sock" \
        "$AIMEE_MODULE_BUS_SOCKET" &
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
