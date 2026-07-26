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
# 2. Baked config seeding. The default config that selects the embedder/LLM
#    sidecar commands is baked at $AIMEE_HOME/.config/aimee/aimee.yaml. Docker
#    *named* volumes copy image content into a fresh volume, so the baked file
#    survives; *bind* mounts (including SmoothNAS "flat" plugin volumes) shadow
#    the directory with an empty host dir, dropping the config — the kb then
#    falls back to a non-functional builtin embedder (embed_ok=false). Keep the
#    canonical default outside $AIMEE_HOME (at /opt/aimee/defaults) and seed it
#    in if the config is missing, so embeddings work under any volume type.
#
#    The config path is aimee_home()/aimee.yaml; with AIMEE_HOME set,
#    aimee_home() == $AIMEE_HOME verbatim (see src/aimee_home.c), so the file
#    the kb reads is $AIMEE_HOME/aimee.yaml -- NOT $AIMEE_HOME/.config/aimee/
#    (that path only applies when AIMEE_HOME is unset and $HOME/.config is used).
#
# 3. DB2. An unset AIMEE_DB2_URL means the operator configured no database, so
#    run the in-image PostgreSQL 17 + pgvector cluster. Any value in
#    AIMEE_DB2_URL selects an external server and nothing is started here.
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
    PGBIN=/usr/lib/postgresql/17/bin
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

    # Stop the cluster cleanly when the container stops. Without this the runtime
    # SIGKILLs postgres once the kb exits and every start replays WAL recovery.
    trap '"$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop || true' EXIT HUP INT TERM

    if ! "$PGBIN/psql" --host="$PGSOCK" --dbname=postgres --no-psqlrc --quiet \
        --tuples-only --command="SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1; then
        "$PGBIN/createdb" --host="$PGSOCK" "$DB"
    fi
    "$PGBIN/psql" --host="$PGSOCK" --dbname="$DB" --no-psqlrc --quiet \
        --command="CREATE EXTENSION IF NOT EXISTS vector" >/dev/null

    # libpq reads a directory-valued host as a socket path.
    AIMEE_DB2_URL="postgresql:///$DB?host=$PGSOCK"
    export AIMEE_DB2_URL

    # Not exec: the trap above has to outlive the kb so the cluster shuts down
    # cleanly. Forward the stop signal so the kb still gets its own shutdown.
    aimee-kb "$@" &
    kb=$!
    trap 'kill -TERM "$kb" 2>/dev/null || true' HUP INT TERM
    rc=0
    wait "$kb" || rc=$?
    exit "$rc"
fi

exec aimee-kb "$@"
