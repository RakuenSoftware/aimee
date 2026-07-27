#!/bin/sh
# Move the aimee-kb internal DB2 to an external PostgreSQL server.
#
# The internal cluster (see aimee-kb-entrypoint.sh) is an ordinary PostgreSQL data
# directory, so this is a plain dump/restore. It exists so outgrowing the embedded
# database is a documented operation rather than a manual pg_dump nobody wrote down.
#
#   docker exec aimee-kb aimee-kb-db-export postgresql://user:pw@host:5432/aimee_shared
#   docker exec aimee-kb aimee-kb-db-export --wipe postgresql://...
#
# Without --wipe the internal cluster is left intact, so the export can be verified
# before anything is destroyed. With --wipe the data directory is removed only
# AFTER the restore verified, and never while the target is unreachable.
#
# The container must then be started with AIMEE_DB2_URL set to the target; the
# entrypoint starts no internal cluster when that variable is set.
set -e

wipe=0
target=""
for arg in "$@"; do
    case "$arg" in
        --wipe) wipe=1 ;;
        -h|--help)
            echo "usage: aimee-kb-db-export [--wipe] <postgresql://target-url>"
            exit 0
            ;;
        postgresql://*|postgres://*) target="$arg" ;;
        *)
            echo "aimee-kb-db-export: unrecognized argument '$arg'" >&2
            exit 2
            ;;
    esac
done

if [ -z "$target" ]; then
    echo "aimee-kb-db-export: a postgresql:// target URL is required" >&2
    exit 2
fi

# Never echo credentials embedded in the target URL. Keep the real URL only in
# the process-local variable passed to libpq.
safe_target=$(printf '%s' "$target" | sed -E \
    's#^(postgres(ql)?://[^:/@]+):[^@]*@#\1:<redacted>@#')

: "${AIMEE_HOME:=/var/lib/aimee}"
PGMAJOR="${AIMEE_DB2_PG_MAJOR:-18}"
PGBIN="/usr/lib/postgresql/$PGMAJOR/bin"
PGDATA="$AIMEE_HOME/postgres"
PGSOCK="$AIMEE_HOME/run"
DB=aimee_shared

if [ ! -f "$PGDATA/PG_VERSION" ]; then
    echo "aimee-kb-db-export: no internal cluster at $PGDATA (nothing to export)" >&2
    exit 1
fi

# The cluster is normally already running under the kb. Start it if this runs in a
# stopped container, and leave it as it was found.
started_here=0
# Name an existing maintenance database explicitly. With only --host, libpq
# defaults the database name to the OS user ("aimee"); the readiness result is
# still positive, but PostgreSQL logs a misleading FATAL for every probe because
# that database intentionally does not exist.
if ! "$PGBIN/pg_isready" --host="$PGSOCK" --dbname=postgres --quiet 2>/dev/null; then
    "$PGBIN/pg_ctl" --pgdata="$PGDATA" --wait --silent \
        --options="-c listen_addresses='' -c unix_socket_directories=$PGSOCK" start
    started_here=1
fi
stop_if_started() {
    [ "$started_here" = 1 ] && "$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop || true
}
trap stop_if_started EXIT

# Fail before dumping if the target is unreachable, so a bad URL never gets as far
# as a half-finished move.
if ! "$PGBIN/psql" "$target" --no-psqlrc --quiet --command="SELECT 1" >/dev/null 2>&1; then
    echo "aimee-kb-db-export: cannot reach target $safe_target" >&2
    exit 1
fi

echo "exporting internal $DB -> $safe_target"
# pgvector/vectorscale must exist on the target before types resolve; the dump
# carries the CREATE EXTENSION, but the extension has to be installed server-side.
if ! "$PGBIN/psql" "$target" --no-psqlrc --quiet --tuples-only \
    --command="SELECT 1 FROM pg_available_extensions WHERE name='vector'" | grep -q 1; then
    echo "aimee-kb-db-export: target has no 'vector' extension available" >&2
    exit 1
fi

"$PGBIN/pg_dump" --host="$PGSOCK" --dbname="$DB" --format=custom --no-owner --no-acl \
    --file=/tmp/aimee-db2-export.dump
"$PGBIN/pg_restore" --dbname="$target" --no-owner --no-acl --exit-on-error \
    /tmp/aimee-db2-export.dump

# Verify before destroying anything: compare exact row counts for every user
# table. pg_stat_user_tables.n_live_tup is an estimate and can legitimately
# differ immediately after restore, so it cannot be used as a migration proof.
count_sql="SELECT format('SELECT %L || '':'' || count(*) FROM %I.%I;', schemaname||'.'||relname, schemaname, relname) FROM pg_stat_user_tables ORDER BY schemaname, relname"
src_counts=$("$PGBIN/psql" --host="$PGSOCK" --dbname="$DB" --no-psqlrc --tuples-only --no-align \
    --command="$count_sql" | "$PGBIN/psql" --host="$PGSOCK" --dbname="$DB" \
    --no-psqlrc --tuples-only --no-align)
dst_counts=$("$PGBIN/psql" "$target" --no-psqlrc --tuples-only --no-align \
    --command="$count_sql" | "$PGBIN/psql" "$target" --no-psqlrc --tuples-only --no-align)
if [ "$src_counts" != "$dst_counts" ]; then
    echo "aimee-kb-db-export: row counts differ after restore; internal data left untouched" >&2
    echo "--- internal ---"; echo "$src_counts"
    echo "--- target ---";   echo "$dst_counts"
    exit 1
fi
rm -f /tmp/aimee-db2-export.dump
echo "verified: $(echo "$src_counts" | wc -l) tables match"

if [ "$wipe" = 1 ]; then
    stop_if_started
    started_here=0
    trap - EXIT
    # Stop the cluster before removing it, whoever started it.
    "$PGBIN/pg_ctl" --pgdata="$PGDATA" --mode=fast --wait --silent stop 2>/dev/null || true
    rm -rf "$PGDATA"
    echo "internal cluster wiped: $PGDATA"
fi

echo
echo "restart aimee-kb with:"
echo "  AIMEE_DB2_URL=$safe_target"
echo "the entrypoint starts no internal cluster while that is set."
