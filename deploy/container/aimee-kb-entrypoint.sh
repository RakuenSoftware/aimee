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
# ---- in-container embedder ---------------------------------------------------
# The kb embeds itself: no embedder sidecar, no aimee-llm hop. Weights are baked into
# the image, so this starts instantly and needs no network.
#
# BEST-EFFORT ON PURPOSE. A failed embedder must not stop the kb from booting: the kb
# already degrades honestly when embedding is unavailable (dense retrieval is skipped and
# says so), whereas an entrypoint that refuses to start takes the whole knowledge base
# down with it. The failure is loud in the log and visible on the embedder's /health.
#
# An operator who points embedding_command at an external endpoint gets that instead;
# this only serves the loopback default.
start_embedder() {
    venv="${EMBEDDER_VENV:-/opt/aimee/embedder-venv}"
    server=/opt/aimee/scripts/embedder-server.py
    if [ ! -x "$venv/bin/python" ] || [ ! -f "$server" ]; then
        echo "aimee-kb: no in-container embedder in this image; relying on a configured endpoint" >&2
        return 0
    fi
    # Which model to serve. Precedence: an explicit env (what the wizard-driven deploy
    # passes as EMBEDDER_MODEL), then the operator's own config, then the baked default.
    # Read from the YAML directly: this runs before the entrypoint seeds config, and the
    # kb has no `config get` subcommand to ask.
    #
    # Only exported when non-empty. Exporting an EMPTY value is not the same as leaving it
    # unset — it overrode the server's own default and the embedder refused to start as
    # "(unset)", which took the kb down with it (no embedder -> no dim -> db2 refuses).
    if [ -z "${EMBEDDER_MODEL:-}" ]; then
        for _cfg in "${AIMEE_HOME:-/var/lib/aimee}/.config/aimee/aimee.yaml" \
                    /opt/aimee/defaults/aimee.yaml; do
            [ -f "$_cfg" ] || continue
            _m=$(sed -n 's/^embedding_model:[[:space:]]*"\{0,1\}\([^"]*\)"\{0,1\}[[:space:]]*$/\1/p' \
                 "$_cfg" | head -1)
            if [ -n "$_m" ]; then
                EMBEDDER_MODEL="$_m"
                export EMBEDDER_MODEL
                break
            fi
        done
    fi
    : "${EMBEDDER_PORT:=8760}"
    export EMBEDDER_PORT
    echo "aimee-kb: starting in-container embedder (${EMBEDDER_MODEL:-default}) on :$EMBEDDER_PORT" >&2
    "$venv/bin/python" "$server" >&2 &
    embedder_pid=$!
}

set -e

# 1. Stack rlimit (64 MB == 65536 KiB == 67108864 bytes). Best-effort: some
#    runtimes forbid raising it, in which case the compose ulimit / a host
#    profile is still required.
ulimit -s 65536 2>/dev/null || true

# 2. Seed the baked default config if it is missing (fresh / bind-mounted
#    volume). Never clobber an operator-provided config.
: "${AIMEE_HOME:=/var/lib/aimee}"
cfg="$AIMEE_HOME/aimee.yaml"
default="/opt/aimee/defaults/aimee.yaml"
if [ ! -f "$cfg" ] && [ -f "$default" ]; then
    mkdir -p "$AIMEE_HOME"
    cp "$default" "$cfg"
fi

# 3. Embedded DB2, only when the operator configured no external server.
if [ -z "${AIMEE_DB2_URL:-}" ]; then
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
    PGMAJOR="${AIMEE_DB2_PG_MAJOR:-18}"
    PGBIN="/usr/lib/postgresql/$PGMAJOR/bin"
    PGDATA="$AIMEE_HOME/postgres"
    PGSOCK="$AIMEE_HOME/run"
    DB=aimee_shared
    mkdir -p "$PGSOCK"

    if [ ! -f "$PGDATA/PG_VERSION" ]; then
        # initdb as the current (aimee) user, so that user is the cluster
        # superuser and no password or role grant is needed over the socket.
        mkdir -p "$PGDATA"
        "$PGBIN/initdb" --pgdata="$PGDATA" --auth-local=trust --encoding=UTF8 >/dev/null
    fi

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
    trap '"$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop || true' EXIT HUP INT TERM

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

    # libpq reads a directory-valued host as a socket path.
    AIMEE_DB2_URL="postgresql:///$DB?host=$PGSOCK"
    export AIMEE_DB2_URL

    start_embedder

    # Not exec: the trap above has to outlive the kb so the cluster shuts down
    # cleanly. Forward the stop signal so the kb still gets its own shutdown.
    aimee-kb "$@" &
    kb=$!
    trap 'kill -TERM "$kb" 2>/dev/null || true' HUP INT TERM

    # POSIX sh has no portable wait -n. Monitor both children, including Linux
    # zombies: kill -0 still succeeds for a dead-but-unreaped postmaster, which
    # previously left the container running unhealthy forever after PostgreSQL
    # crashed. Either child is load-bearing, so stop its peer and let the
    # container restart them together.
    process_alive() {
        _pid=$1
        kill -0 "$_pid" 2>/dev/null || return 1
        [ -r "/proc/$_pid/stat" ] || return 1
        IFS=' ' read -r _stat_pid _stat_comm _stat_state _stat_rest < "/proc/$_pid/stat" || return 1
        [ "$_stat_state" != Z ]
    }

    first=
    while [ -z "$first" ]; do
        if ! process_alive "$kb"; then
            first=kb
        elif ! process_alive "$pg_pid"; then
            first=postgres
        else
            sleep 0.1
        fi
    done

    if [ "$first" = postgres ]; then
        echo "aimee-kb: embedded PostgreSQL exited; restarting the KB container as one unit" >&2
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
        wait "$pg_pid" 2>/dev/null || true
        exit 1
    fi

    rc=0
    wait "$kb" || rc=$?
    exit "$rc"
fi

start_embedder
exec aimee-kb "$@"
