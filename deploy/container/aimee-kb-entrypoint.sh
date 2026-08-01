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
#   none of the above       -> start nothing. The kb falls back to its builtin lexical
#                              embedder, which needs no model and no port, so an
#                              unconfigured container is idle rather than half-configured.
#
# When it DOES start, the loopback URL is exported as EMBEDDER_URL. That makes the
# bundled embedder just "an embedder at a URL" and reuses one precedence rule for both
# cases, instead of a second mechanism that can disagree with the first.
#
# Starting is best-effort: the kb degrades honestly when embedding is unavailable, whereas
# an entrypoint that refuses takes the whole knowledge base down with it.
# Ask the binary, never the file. This used to parse aimee.yaml with a sed regex, which
# hardcoded the config paths and assumed a top-level `embedding_model:` key — a second
# reader of a setting config owns. It worked only because config_save happens to write
# the key at root, and it failed SILENTLY: an unparsed key reads as "nothing selected",
# so the builtin serves forever and nothing says why.
read_cfg_embedding_model() {
    aimee-kb --print-embedding-model 2>/dev/null || true
}

start_embedder() {
    if [ -n "${EMBEDDER_URL:-}" ]; then
        echo "aimee-kb: external embedder configured ($EMBEDDER_URL); bundled model not loaded" >&2
        return 0
    fi
    if [ -z "${EMBEDDER_MODEL:-}" ]; then
        EMBEDDER_MODEL="$(read_cfg_embedding_model)"
    fi
    if [ -z "$EMBEDDER_MODEL" ]; then
        echo "aimee-kb: no embedder selected; the bundled model stays unloaded (the builtin" \
             "lexical embedder serves until the wizard selects one)" >&2
        return 0
    fi
    export EMBEDDER_MODEL

    venv="${EMBEDDER_VENV:-/opt/aimee/embedder-venv}"
    server=/opt/aimee/scripts/embedder-server.py
    if [ ! -x "$venv/bin/python" ] || [ ! -f "$server" ]; then
        echo "aimee-kb: '$EMBEDDER_MODEL' selected but this image has no bundled embedder" >&2
        return 0
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

# ---- bundled synthesis (the *-llm image variants) ---------------------------
# Same shape as the embedder above, and deliberately so: selection resolves to a
# URL, and the kb then reaches a bundled model exactly as it reaches a remote one.
#
#   SYNTHESIS_ENDPOINT set -> a remote endpoint; start nothing.
#   SYNTHESIS_MODEL set    -> the bundled model; fetch if needed, start it, and
#                             export SYNTHESIS_ENDPOINT at loopback.
#   neither                -> start nothing. Synthesis is OFF, which is supported:
#                             embedding, search, recall and indexing never call it.
#
# WEIGHTS LIVE ON THE PERSISTENT VOLUME, NOT IN THE IMAGE. gemma-4-E4B is ~7.5GB
# at the shipped UD-Q6_K_XL; baking that would roughly double the image
# and re-download on every image bump. /var/lib/aimee is the aimee-kb-home volume,
# so the weights survive image upgrades, rollbacks and rebuilds.
#
# HF_HOME IS SCOPED TO THIS PROCESS ONLY. The image sets HF_HOME=/opt/aimee/models
# with HF_HUB_OFFLINE=1 for the BAKED EMBEDDER weights. Exporting a different
# HF_HOME globally would leave the embedder unable to find them and, being offline,
# it would fail closed rather than re-download. Two model stores, two lifetimes:
# the embedder's is immutable and in the image, synthesis' is mutable and on the
# volume.
#
# Downloading is best-effort and can take a long time on first start. The kb
# degrades honestly when synthesis is unavailable, whereas an entrypoint that
# blocked or refused would take the whole knowledge base down waiting for a model.
synthesis_repo_for_model() {
    # Explicit map, not a string-built repo name. An unknown model must not
    # silently become a 404 download that leaves synthesis quietly dead.
    #
    # THE QUANT IS EXPLICIT AND MUST EXIST IN THE REPO. llama.cpp's -hf defaults to
    # Q4_K_M and, when that is absent, falls back to another file in the repo — so a
    # quant that is not published does not fail, it silently serves something else.
    # UD-Q6_K_XL (Unsloth Dynamic) is published in these repos and is what we ship;
    # llama.cpp matches the tag as "-<TAG>." against the filename, case-insensitively,
    # so this resolves gemma-4-E{2,4}B-it-UD-Q6_K_XL.gguf and nothing adjacent.
    #
    # unsloth/, not ggml-org/: the UD quants only exist there.
    case "$1" in
        gemma-4-E2B-it) echo "unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL" ;;
        gemma-4-E4B-it) echo "unsloth/gemma-4-E4B-it-GGUF:UD-Q6_K_XL" ;;
        *) echo "" ;;
    esac
}

start_synthesis() {
    if [ -n "${SYNTHESIS_ENDPOINT:-}" ]; then
        echo "aimee-kb: remote synthesis configured ($SYNTHESIS_ENDPOINT); bundled model not loaded" >&2
        return 0
    fi
    if [ -z "${SYNTHESIS_MODEL:-}" ]; then
        echo "aimee-kb: no synthesis model selected; synthesis is off (embedding, search," \
             "recall and indexing are unaffected)" >&2
        return 0
    fi

    llama=/opt/aimee/llama.cpp/llama-server
    if [ ! -x "$llama" ]; then
        echo "aimee-kb: '$SYNTHESIS_MODEL' selected but this image has no bundled llama.cpp." \
             "Use an aimee-kb-*-llm image, or point SYNTHESIS_ENDPOINT at a remote endpoint." >&2
        return 0
    fi

    repo="$(synthesis_repo_for_model "$SYNTHESIS_MODEL")"
    if [ -z "$repo" ]; then
        echo "aimee-kb: '$SYNTHESIS_MODEL' is not a known bundled synthesis model" \
             "(gemma-4-E2B-it, gemma-4-E4B-it); synthesis stays off" >&2
        return 0
    fi

    : "${SYNTHESIS_PORT:=8761}"
    export SYNTHESIS_PORT
    models_dir="${AIMEE_HOME:-/var/lib/aimee}/models"
    mkdir -p "$models_dir" 2>/dev/null || true

    echo "aimee-kb: starting bundled synthesis ($SYNTHESIS_MODEL) on :$SYNTHESIS_PORT;" \
         "weights under $models_dir (first start downloads them)" >&2
    # --no-mmproj: every benchmark run passed it and the shipped service never did,
    # so production loaded a ~0.5GB vision/audio projector for a text-only task that
    # cannot use it. -hf also FETCHES the projector unless MMPROJ_AUTO is off, so
    # both the download and the resident cost are avoided here.
    HF_HOME="$models_dir" \
    HF_HUB_OFFLINE=0 \
    LLAMA_ARG_MMPROJ_AUTO=false \
        "$llama" -hf "$repo" --host 127.0.0.1 --port "$SYNTHESIS_PORT" \
                 -c 8192 --no-webui --no-mmproj >&2 &
    synthesis_pid=$!

    # One precedence rule for both cases, as with the embedder: the kb reaches the
    # bundled model the same way it would reach a remote one.
    SYNTHESIS_ENDPOINT="http://127.0.0.1:$SYNTHESIS_PORT/v1"
    export SYNTHESIS_ENDPOINT
}


set -e

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
        start_embedder
        exec aimee-kb "$@"
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
    AIMEE_DB2_URL="$embedded_dsn" aimee-kb --bootstrap-vault-env

    start_embedder
    start_synthesis

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
start_synthesis
exec aimee-kb "$@"
