#!/bin/sh
# aimee-kb container entrypoint.
#
# Wraps the aimee-kb binary to make the image robust to deployment
# environments whose volume semantics differ from Docker named volumes.
#
# 1. Stack rlimit. The kb's drain/ingest/watch/query worker threads need a
#    64 MB stack; the 8 MB container default overflows and SIGSEGVs (exit 139)
#    on real memory/kb queries. Compose sets `ulimits: stack: 67108864`, but
#    runtimes that don't (e.g. SmoothNAS plugins, plain `docker run`) inherit
#    the small default. The container's hard limit is unlimited, so raise the
#    soft limit here before exec.
#
# 2. Baked config seeding. The image keeps its default embedder/LLM config at
#    /opt/aimee/defaults/aimee.yaml. Seed that into $AIMEE_HOME/aimee.yaml when
#    a fresh named or bind-mounted volume is empty, so both volume types get the
#    same working defaults without clobbering an operator-provided config.
#
#    The config path is aimee_home()/aimee.yaml; with AIMEE_HOME set,
#    aimee_home() == $AIMEE_HOME verbatim (see src/aimee_home.c), so the file
#    the kb reads is $AIMEE_HOME/aimee.yaml -- NOT $AIMEE_HOME/.config/aimee/
#    (that path only applies when AIMEE_HOME is unset and $HOME/.config is used).
#
# 3. DB2. An unset AIMEE_DB2_URL means the operator configured no database, so
#    run the in-image PostgreSQL 18 + pgvector cluster. Any value in
#    AIMEE_DB2_URL selects an external server and nothing is started here.
# ---- bundled embedder -------------------------------------------------------
# The weights are baked into the image, but the model is NOT loaded unless it has been
# SELECTED. Half a gigabyte of resident model is not something to spend on a kb that was
# never told to embed with it, and on the wizard path the selection always exists before
# this container is deployed (`aimee config deploy-env` emits EMBEDDER_MODEL for the
# bundled embedder, or EMBEDDER_URL for an external one).
#
# Selection, in precedence order:
#   EMBEDDER_URL set  -> an external embedder; start nothing.
#   EMBEDDER_MODEL set      -> the bundled embedder; start it.
#   embedding_model in cfg  -> same, for a hand-run container.
#   none of the above       -> REFUSE TO START.
#
# When it DOES start, the loopback URL is exported as EMBEDDER_URL. That makes the
# bundled embedder just "an embedder at a URL" and reuses one precedence rule for both
# cases, instead of a second mechanism that can disagree with the first.
#
# Refusing is the point. There used to be a builtin lexical embedder behind this, so an
# unconfigured container came up healthy and answered every search with keyword matching
# — a deployment could run for weeks believing it had vector retrieval. It also claimed
# the corpus: db2 recorded the fallback as the vector space, so choosing a real embedder
# later was a space change the guard refused, and the kb never started again. A kb with
# no embedder cannot do the one thing it exists for, and saying so at startup is cheaper
# than discovering it from bad answers.
# Ask the pure-Go config process binary, never the file and never a native
# configuration implementation. Before the KB daemon bus exists, its narrow
# public-string bootstrap mode is the sole authoritative reader.
read_cfg_embedding_model() {
    /usr/local/libexec/aimee-modules/aimee-module-config --get embedder_model 2>/dev/null || true
}

# Is this container starting the KB SERVICE, or running a one-shot that exits?
#
# Only a serving container needs an embedder. Two kinds of invocation do not:
#
#   1. A bare subcommand first — `managed-server-identity install ...` is the managed
#      deploy's server-enrolment job, which runs this image against the kb's volume and
#      never serves a query. Requiring an embedder of it failed server identity
#      enrolment on every clean install: the kb came up and the server could not talk
#      to it.
#   2. An informational flag that aimee-kb answers at argv[1] and exits — `--help`,
#      `--version`, and the vault/config one-shots the entrypoint itself invokes.
#      Classifying every `-*` as serving refused `docker run <image> --help` on a fresh
#      install, which is the first thing someone types to check the image is alive, and
#      the moment they are least likely to have configured an embedder.
#
# The flag list mirrors aimee-kb's own argv[1] handling in kb_main.c. If a one-shot flag
# is added there it belongs here too; the cost of missing one is a refusal to print
# help, not a kb serving without an embedder.
kb_is_serving() {
    case "${1:-}" in
    --help | -h | --version | -v | --print-embedding-model | --bootstrap-vault-env | \
        --bootstrap-vault-stdin | --list-credential-env-names)
        return 1 ;;
    "" | -*) return 0 ;;
    *) return 1 ;;
    esac
}

start_embedder() {
    if ! kb_is_serving "$@"; then
        return 0
    fi
    if [ -n "${EMBEDDER_URL:-}" ]; then
        echo "aimee-kb: external embedder configured ($EMBEDDER_URL); bundled model not loaded" >&2
        return 0
    fi
    if [ -z "${EMBEDDER_MODEL:-}" ]; then
        EMBEDDER_MODEL="$(read_cfg_embedding_model)"
    fi
    if [ -z "$EMBEDDER_MODEL" ]; then
        echo "aimee-kb: no embedder selected, and there is no fallback. Retrieval needs one." >&2
        echo "aimee-kb:   pick a bundled model:  aimee config set embedder_model bekko-a25m" >&2
        echo "aimee-kb:   or point at your own:  EMBEDDER_URL=http://<host>:<port>" >&2
        echo "aimee-kb: then re-run Deploy. Refusing to start." >&2
        exit 1
    fi
    export EMBEDDER_MODEL

    venv="${EMBEDDER_VENV:-/opt/aimee/embedder-venv}"
    server=/opt/aimee/scripts/embedder-server.py
    if [ ! -x "$venv/bin/python" ] || [ ! -f "$server" ]; then
        echo "aimee-kb: '$EMBEDDER_MODEL' selected but this image has no bundled embedder." >&2
        echo "aimee-kb: the aimee-kb image carries no weights — use aimee-kb-a25m or" >&2
        echo "aimee-kb: aimee-kb-nomic, or set EMBEDDER_URL. Refusing to start." >&2
        exit 1
    fi
    : "${EMBEDDER_PORT:=8760}"
    export EMBEDDER_PORT
    # One precedence rule for both cases: the kb reaches the bundled embedder the same way
    # it would reach an external one.
    EMBEDDER_URL="http://127.0.0.1:$EMBEDDER_PORT"
    export EMBEDDER_URL
    echo "aimee-kb: starting bundled embedder ($EMBEDDER_MODEL) on :$EMBEDDER_PORT" >&2
    "$venv/bin/python" "$server" >&2 &
    embedder_pid=$!
}

# ---- synthesis --------------------------------------------------------------
# NOTHING TO START. Synthesis used to run inside this container from a GGUF baked
# into the *-llm image variants. It is its own image now (aimee-llm-e2b / -e4b),
# deployed beside this one and reached over mTLS, because llama.cpp and multi-
# gigabyte weights should not be rebuilt every time kb code changes.
#
# The kb therefore treats every provider the same way it has always treated a remote
# one: SYNTHESIS_ENDPOINT names it, or synthesis is off. Off is a supported state,
# not an error -- embedding, search, recall and indexing never call it.
#
# The mTLS material for the sidecar hop is issued by the kb at startup, not here;
# see kb_synthesis_identity.c.

# The PostgreSQL module is a separate process and deliberately cannot open the
# KB's credential Vault.  For the embedded cluster its DSN contains no secret:
# it is a local trust-authenticated Unix socket owned by this container.  Give
# that exact DSN to the module instead of leaving health and all DB1-backed
# operations permanently disconnected from the database the KB just started.
configure_embedded_store_module() {
    AIMEE_STORE_URL=$1
    AIMEE_STORE_MIGRATION_URL=$2
    export AIMEE_STORE_URL AIMEE_STORE_MIGRATION_URL
}

# Sourcing stops here: everything above is definitions, everything below starts a
# container. tests/test_kb_entrypoint.sh uses this to exercise the embedder gate without
# a PostgreSQL cluster, a Vault, or an image.
[ -n "${AIMEE_KB_ENTRYPOINT_SOURCE_ONLY:-}" ] && return 0

set -e

# Operator control over which optional modules attach to the kb bus. Installed
# path first, then alongside this script for a source checkout.
optional_modules_lib=/usr/local/bin/optional-modules-lib.sh
if [ ! -r "$optional_modules_lib" ]; then
    kb_entrypoint_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
    optional_modules_lib="$kb_entrypoint_dir/optional-modules-lib.sh"
fi
[ -r "$optional_modules_lib" ] || {
    printf '[aimee-kb-entrypoint] fatal: optional-module helper is unavailable\n' >&2
    exit 2
}
. "$optional_modules_lib"

# Kubernetes/Docker credential environment is first-boot transport only. Record
# the non-secret external-DB decision, seal every credential-shaped value into
# Vault, and scrub this PID's inherited copy before any unrelated child process.
vault_bootstrapped=0
external_db=0
case "${1:-}" in
    --aimee-internal-vault-bootstrapped-external-db)
        vault_bootstrapped=1
        external_db=1
        shift
        ;;
    --aimee-internal-vault-bootstrapped-embedded-db)
        vault_bootstrapped=1
        shift
        ;;
esac
: "${AIMEE_HOME:=/var/lib/aimee}"
export AIMEE_HOME
export AIMEE_EGRESS_CREDENTIAL_HELPER=/usr/local/bin/aimee-kb
[ -n "${AIMEE_DB2_URL:-}" ] && external_db=1
aimee-kb --bootstrap-vault-env
_secret_names=$(aimee-kb --list-credential-env-names)
had_credential_env=0
for _secret_name in $_secret_names; do
    eval "_secret_was_set=\${${_secret_name}+x}"
    [ "$_secret_was_set" = x ] && had_credential_env=1
    unset "$_secret_name"
done
unset _secret_was_set
# Container metadata is deliberately credential-free after the disposable
# bootstrap. Resolve the DB topology from Vault without printing the URL. The
# fixed probe distinguishes the entrypoint's own embedded socket DSN from an
# operator-supplied external connection string.
if aimee-kb --vault-db2-external; then
    external_db=1
else
    external_db=0
fi
if [ "$vault_bootstrapped" -eq 0 ] || [ "$had_credential_env" -eq 1 ]; then
    if [ "$external_db" -eq 1 ]; then
        exec /bin/sh "$0" --aimee-internal-vault-bootstrapped-external-db "$@"
    fi
    exec /bin/sh "$0" --aimee-internal-vault-bootstrapped-embedded-db "$@"
fi

# 1. Stack rlimit (64 MB == 65536 KiB == 67108864 bytes). Best-effort: some
#    runtimes forbid raising it, in which case the compose ulimit / a host
#    profile is still required.
ulimit -s 65536 2>/dev/null || true
ulimit -c 0 2>/dev/null || true

# 2. Seed the baked default config if it is missing (fresh / bind-mounted
#    volume). Never clobber an operator-provided config.
cfg="$AIMEE_HOME/aimee.yaml"
default="/opt/aimee/defaults/aimee.yaml"
if [ ! -f "$cfg" ] && [ -f "$default" ]; then
    mkdir -p "$AIMEE_HOME"
    cp "$default" "$cfg"
fi

export AIMEE_MODULE_BUS_SOCKET="${AIMEE_MODULE_BUS_SOCKET:-$AIMEE_HOME/kb-module-bus.sock}"
MODULE_MANIFEST="${AIMEE_MODULE_MANIFEST:-/opt/aimee/module-grants/kb.modules}"
module_supervisor_pid=""

start_modules() {
    mkdir -p "$AIMEE_HOME/modules.d/kb"
    for module_grant in /opt/aimee/module-grants/kb/*.grant; do
        [ -f "$module_grant" ] || continue
        grant_target="$AIMEE_HOME/modules.d/kb/$(basename "$module_grant")"
        [ -e "$grant_target" ] || cp "$module_grant" "$grant_target"
    done
    chmod 0700 "$AIMEE_HOME/modules.d" "$AIMEE_HOME/modules.d/kb" 2>/dev/null || true
    chmod 0600 "$AIMEE_HOME/modules.d/kb/"*.grant 2>/dev/null || true
    # Apply the operator's AIMEE_MODULE_<ID> choices over the shipped manifest.
    MODULE_MANIFEST="$(apply_optional_modules kb "$MODULE_MANIFEST" "$AIMEE_HOME")"
    module-supervisor.sh kb "$AIMEE_MODULE_BUS_SOCKET" "$MODULE_MANIFEST" &
    module_supervisor_pid=$!
}

stop_modules() {
    [ -n "$module_supervisor_pid" ] || return 0
    kill "$module_supervisor_pid" 2>/dev/null || true
    wait "$module_supervisor_pid" 2>/dev/null || true
    module_supervisor_pid=""
}

run_kb_with_modules() {
    start_modules
    aimee-kb "$@" &
    kb=$!
    trap 'kill -TERM "$kb" 2>/dev/null || true; stop_modules' HUP INT TERM
    rc=0
    wait "$kb" || rc=$?
    stop_modules
    return "$rc"
}

# 3. Embedded DB2, only when the operator configured no external server.
if [ "$external_db" -eq 0 ]; then
    PGMAJOR="${AIMEE_DB2_PG_MAJOR:-18}"
    # Overridable so the entrypoint's cluster handling is testable without a real
    # PostgreSQL install; deployments never set it.
    PGBIN="${AIMEE_DB2_PG_BIN:-/usr/lib/postgresql/$PGMAJOR/bin}"
    PGDATA="$AIMEE_HOME/postgres"
    PGSOCK="$AIMEE_HOME/run"
    DB=aimee_shared
    mkdir -p "$PGSOCK"

    # A one-shot that SHARES the kb's volume finds the cluster already up, owned
    # by the long-lived kb container -- the managed deploy's aimee-server-identity
    # job is exactly this. Connect to that cluster instead of provisioning a
    # second one over the same data directory.
    #
    # This has to precede the root check below: that job runs as root on purpose
    # (it chowns the server identity it installs), and PostgreSQL forbids running
    # the SERVER as root, not connecting to one as root. Refusing here failed
    # managed server identity enrollment on every clean install.
    if "$PGBIN/pg_isready" --host="$PGSOCK" --quiet 2>/dev/null; then
        echo "aimee-kb: PostgreSQL already running on $PGSOCK; using it instead of" \
             "starting a second cluster" >&2
        # Enrollment and informational one-shots share the live KB's volume,
        # not its bus identity. Starting a second module supervisor here makes
        # every module attach under a principal the serving KB already owns, so
        # the bus correctly denies them and enrollment fails. The binary can
        # reach the shared local-trust socket directly; run only that one-shot.
        if ! kb_is_serving "$@"; then
            exec aimee-kb "$@"
        fi
        start_embedder "$@"
        run_kb_with_modules "$@"
        exit $?
    fi

    # PostgreSQL refuses to run as root, unconditionally. The image declares
    # USER aimee, so this only trips when a runtime overrides it (e.g. --user root
    # to work around bind-mount ownership). Say so, rather than letting initdb
    # fail with "cannot be run as root" and no indication of the fix.
    if [ "$(id -u)" = 0 ]; then
        echo "aimee-kb: the internal database cannot run as root (PostgreSQL forbids it)." >&2
        echo "  Run the container as the 'aimee' user (the image's default), or set" >&2
        echo "  AIMEE_DB2_URL to an external PostgreSQL server to skip the internal one." >&2
        exit 1
    fi

    if [ ! -f "$PGDATA/PG_VERSION" ]; then
        # initdb as the current (aimee) user, so that user is the cluster
        # superuser and no password or role grant is needed over the socket.
        mkdir -p "$PGDATA"
        "$PGBIN/initdb" --pgdata="$PGDATA" --auth-local=trust --encoding=UTF8 >/dev/null
    fi

    # pg_ctl --wait gives up after 60s by default. That is far less than crash
    # recovery needs on a large cluster: an unclean stop (the runtime SIGKILLing
    # postgres, or `docker rm -f` on the kb, neither of which lets the EXIT trap
    # below run) makes the next start fsync the whole data directory before it
    # will accept connections. Past 60s pg_ctl returns failure, the entrypoint
    # exits, `restart: unless-stopped` starts the container again, and recovery
    # replays FROM SCRATCH -- a livelock that never converges, because each
    # attempt is killed at the same deadline it could never have met. Observed
    # as an endless "server did not start in time" loop on a 27k-vector corpus.
    #
    # Wait long enough for recovery to finish instead. This only ever delays the
    # unhealthy case: a cleanly-stopped cluster still starts in seconds, so the
    # value is a ceiling, not a cost.
    export PGCTLTIMEOUT="${AIMEE_DB2_PGCTLTIMEOUT:-1800}"

    # No TCP listener: DB2 is reachable only over the socket inside this
    # container. An operator who wants it exposed runs an external server.
    "$PGBIN/pg_ctl" --pgdata="$PGDATA" --wait --silent \
        --options="-c listen_addresses='' -c unix_socket_directories=$PGSOCK" start
    pg_pid=$(head -1 "$PGDATA/postmaster.pid")
    case "$pg_pid" in
        ''|*[!0-9]*)
            echo "aimee-kb: embedded PostgreSQL started without a valid postmaster PID" >&2
            exit 1
            ;;
    esac

    # Stop the cluster cleanly when the container stops. Without this the runtime
    # SIGKILLs postgres once the kb exits and every start replays WAL recovery.
    trap 'stop_modules; "$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop || true' EXIT
    trap 'stop_modules; "$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop || true' HUP INT TERM

    if ! "$PGBIN/psql" --host="$PGSOCK" --dbname=postgres --no-psqlrc --quiet \
        --tuples-only --command="SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1; then
        "$PGBIN/createdb" --host="$PGSOCK" "$DB"
    fi
    "$PGBIN/psql" --host="$PGSOCK" --dbname="$DB" --no-psqlrc --quiet \
        --command="CREATE EXTENSION IF NOT EXISTS vector" >/dev/null
    # Enable pgvectorscale when its extension library is present. pgvector alone
    # remains a supported fallback for images built without the optional layer.
    # pgrx installs the library version-stamped (vectorscale-0.9.0.so), so match a
    # glob -- testing for a bare vectorscale.so silently never enables it. Resolved
    # in a subshell because $@ still carries the kb's own arguments.
    vectorscale_lib=$(ls "/usr/lib/postgresql/$PGMAJOR/lib/vectorscale"*.so 2>/dev/null | head -1)
    if [ -n "$vectorscale_lib" ]; then
        "$PGBIN/psql" --host="$PGSOCK" --dbname="$DB" --no-psqlrc --quiet \
            --command="CREATE EXTENSION IF NOT EXISTS vectorscale" >/dev/null
    fi

    # libpq reads a directory-valued host as a socket path. Even this local,
    # passwordless DSN follows the credential-shaped config contract: give it
    # to a disposable bootstrap helper, then let the KB load it from Vault.
    #
    # Name the role explicitly. initdb made THIS user the cluster superuser, and
    # this DSN is sealed into a Vault on a volume other containers share: a
    # sharer running as a different OS user (the managed deploy's root
    # aimee-server-identity job) would otherwise have libpq default the role to
    # its own user name and fail with "DB2 not reachable". Vault holds this value
    # for every sharer, and the entrypoint scrubs AIMEE_DB2_URL from the
    # environment before exec, so their own compose-supplied DSN cannot fix it.
    # Fall back to the bare DSN if the runtime has no passwd entry for this uid:
    # an empty user= is worse than none, and libpq's default is right whenever
    # every reader runs as this same user anyway.
    cluster_owner=$(id -un 2>/dev/null || true)
    if [ -n "$cluster_owner" ]; then
        embedded_dsn="postgresql:///$DB?host=$PGSOCK&user=$cluster_owner"
    else
        embedded_dsn="postgresql:///$DB?host=$PGSOCK"
    fi
    # The Go store keeps DDL off its ordinary SQL pool even in the self-contained
    # image. Local trust authentication needs no passwords, but it still uses two
    # distinct PostgreSQL roles so a compromised runtime module cannot migrate.
    "$PGBIN/psql" "$embedded_dsn" --no-psqlrc --quiet >/dev/null <<'SQL'
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_store_migrator') THEN
    CREATE ROLE aimee_store_migrator LOGIN NOINHERIT NOBYPASSRLS;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
    CREATE ROLE aimee_store_runtime LOGIN NOINHERIT NOBYPASSRLS;
  END IF;
END $$;
ALTER ROLE aimee_store_migrator LOGIN NOINHERIT NOBYPASSRLS
  NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_store_runtime LOGIN NOINHERIT NOBYPASSRLS
  NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
GRANT CONNECT ON DATABASE aimee_shared TO aimee_store_migrator, aimee_store_runtime;
GRANT USAGE, CREATE ON SCHEMA public TO aimee_store_migrator;
GRANT USAGE ON SCHEMA public TO aimee_store_runtime;
REVOKE CREATE ON SCHEMA public FROM aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT EXECUTE ON FUNCTIONS TO aimee_store_runtime;
SQL
    embedded_store_runtime_dsn="postgresql:///$DB?host=$PGSOCK&user=aimee_store_runtime"
    embedded_store_migration_dsn="postgresql:///$DB?host=$PGSOCK&user=aimee_store_migrator"
    configure_embedded_store_module "$embedded_store_runtime_dsn" \
        "$embedded_store_migration_dsn"
    AIMEE_DB2_URL="$embedded_dsn" aimee-kb --bootstrap-vault-env

    # The self-contained tier still runs audit construction in a separate OS
    # process and under a separate PostgreSQL role. Local trust auth means this
    # is not the hardened credential boundary (that requires the external
    # worker service), but it preserves the process/control-flow split and keeps
    # the development image functional without giving aimee-kb a worker thread.
    "$PGBIN/psql" "$embedded_dsn" --no-psqlrc --quiet >/dev/null <<'SQL'
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_worm_worker') THEN
    CREATE ROLE aimee_kb_worm_worker LOGIN NOINHERIT NOBYPASSRLS;
  END IF;
END $$;
ALTER ROLE aimee_kb_worm_worker LOGIN NOINHERIT NOBYPASSRLS
  NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
SQL
    embedded_worm_dsn="postgresql:///$DB?host=$PGSOCK&user=aimee_kb_worm_worker"

    # POSIX sh has no portable wait -n. Monitor both children, including Linux
    # zombies: kill -0 still succeeds for a dead-but-unreaped postmaster, which
    # previously left the container running unhealthy forever after PostgreSQL
    # crashed. The same check also puts a hard bound on a KB whose worker
    # threads do not drain after TERM.
    process_alive() {
        _pid=$1
        kill -0 "$_pid" 2>/dev/null || return 1
        [ -r "/proc/$_pid/stat" ] || return 1
        IFS=' ' read -r _stat_pid _stat_comm _stat_state _stat_rest < "/proc/$_pid/stat" || return 1
        [ "$_stat_state" != Z ]
    }

    start_embedded_worm() {
        (
            # aimee-kb owns schema migration. Wait until the bounded claim/ack
            # API exists, then install exactly the worker grants and drop every
            # direct storage/function capability before authenticating as it.
            while process_alive "$kb"; do
                if "$PGBIN/psql" "$embedded_dsn" --no-psqlrc --quiet --tuples-only \
                    --command="SELECT to_regprocedure('aimee_kb_worm_api.claim(integer)') IS NOT NULL AND to_regprocedure('aimee_kb_worm_api.ack(bigint,bigint)') IS NOT NULL" \
                    2>/dev/null | grep -q t; then
                    break
                fi
                sleep 0.1
            done
            process_alive "$kb" || exit 1
            "$PGBIN/psql" "$embedded_dsn" --no-psqlrc --quiet >/dev/null <<'SQL'
  -- Revoking the role itself is not enough: PostgreSQL grants schema public
  -- USAGE to PUBLIC by default, and privileges inherited through PUBLIC cannot
  -- be denied per-role.  The embedded cluster's database owner retains access
  -- through pg_database_owner; close the inherited path before asserting that
  -- the worker can resolve only its private API schema.
  REVOKE USAGE ON SCHEMA public FROM PUBLIC;
  REVOKE ALL ON SCHEMA public FROM aimee_kb_worm_worker;
  REVOKE ALL ON SCHEMA aimee_kb_worm_api FROM PUBLIC;
  GRANT USAGE ON SCHEMA aimee_kb_worm_api TO aimee_kb_worm_worker;
  REVOKE ALL ON ALL TABLES IN SCHEMA public FROM aimee_kb_worm_worker;
  REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM aimee_kb_worm_worker;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA public FROM aimee_kb_worm_worker;
  REVOKE ALL ON ALL FUNCTIONS IN SCHEMA aimee_kb_worm_api FROM aimee_kb_worm_worker;
  GRANT EXECUTE ON FUNCTION aimee_kb_worm_api.claim(INTEGER),
    aimee_kb_worm_api.ack(BIGINT,BIGINT) TO aimee_kb_worm_worker;
SQL
            mkdir -p "$AIMEE_HOME/audit"
            exec env AIMEE_WORM_DB2_URL="$embedded_worm_dsn" \
                AIMEE_WORM_PATH="$AIMEE_HOME/audit/kb-worm-live.db" \
                aimee-kb-worm
        ) &
        worm=$!
    }

    start_embedder "$@"
    start_modules

    # Not exec: the trap above has to outlive the kb so the cluster shuts down
    # cleanly. Forward the stop signal so the kb still gets its own shutdown.
    aimee-kb "$@" &
    kb=$!
    start_embedded_worm
    shutdown_embedded() {
        # A signal trap that only forwards to the KB returns to the monitor loop
        # below. Docker then reaches its stop timeout and SIGKILLs PID 1 plus the
        # still-running postmaster, forcing WAL recovery on every ordinary
        # restart. Make shutdown terminal and keep all children inside the same
        # bounded lifecycle. Reset the traps first so the explicit exit below
        # cannot run this handler a second time.
        trap - EXIT HUP INT TERM
        kill -TERM "$worm" 2>/dev/null || true
        kill -TERM "$kb" 2>/dev/null || true
        stop_modules
        "$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop || true
        _stop_ticks=0
        while process_alive "$kb" && [ "$_stop_ticks" -lt 30 ]; do
            sleep 0.1
            _stop_ticks=$((_stop_ticks + 1))
        done
        if process_alive "$kb"; then
            echo "aimee-kb: KB did not stop after 3s; forcing shutdown after database stop" >&2
            kill -KILL "$kb" 2>/dev/null || true
        fi
        wait "$worm" 2>/dev/null || true
        wait "$kb" 2>/dev/null || true
    }
    trap 'shutdown_embedded' EXIT
    trap 'shutdown_embedded; exit 0' HUP INT TERM

    # Either child is load-bearing, so stop its peer and let the container
    # restart them together.
    first=
    while [ -z "$first" ]; do
        if ! process_alive "$kb"; then
            first=kb
        elif ! process_alive "$pg_pid"; then
            first=postgres
        elif ! process_alive "$worm"; then
            first=worm
        else
            sleep 0.1
        fi
    done

    if [ "$first" = postgres ]; then
        echo "aimee-kb: embedded PostgreSQL exited; restarting the KB container as one unit" >&2
        kill -TERM "$worm" 2>/dev/null || true
        kill -TERM "$kb" 2>/dev/null || true
        # A database failure can leave worker threads blocked in libpq while
        # the kb is trying to shut down.  Do not let PID 1 wait forever: that
        # prevents Docker's restart policy from ever rebuilding the unit and
        # leaves the last successful health result looking deceptively green.
        _stop_ticks=0
        while process_alive "$kb" && [ "$_stop_ticks" -lt 50 ]; do
            sleep 0.1
            _stop_ticks=$((_stop_ticks + 1))
        done
        if process_alive "$kb"; then
            echo "aimee-kb: KB did not stop after 5s; forcing shutdown" >&2
            kill -KILL "$kb" 2>/dev/null || true
        fi
        wait "$kb" 2>/dev/null || true
        wait "$worm" 2>/dev/null || true
        wait "$pg_pid" 2>/dev/null || true
        exit 1
    fi

    if [ "$first" = worm ]; then
        echo "aimee-kb: WORM audit worker exited; restarting the KB container as one unit" >&2
        kill -TERM "$kb" 2>/dev/null || true
        _stop_ticks=0
        while process_alive "$kb" && [ "$_stop_ticks" -lt 50 ]; do
            sleep 0.1
            _stop_ticks=$((_stop_ticks + 1))
        done
        if process_alive "$kb"; then
            echo "aimee-kb: KB did not stop after 5s following WORM worker exit; forcing shutdown" >&2
            kill -KILL "$kb" 2>/dev/null || true
        fi
        wait "$kb" 2>/dev/null || true
        wait "$worm" 2>/dev/null || true
        exit 1
    fi

    rc=0
    wait "$kb" || rc=$?
    exit "$rc"
fi

start_embedder "$@"
run_kb_with_modules "$@"
exit $?
