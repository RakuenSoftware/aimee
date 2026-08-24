#!/bin/bash
# Run the DB2 process replay against a real Postgres, creating the container it
# needs if it is not there.
#
# The replay is the only test that proves a DB2 statement parses and runs: the
# unit suites drive stub backends, so a column that does not exist or a function
# that was never created passes them and fails here. It therefore has to be easy
# to run, and it has not been -- the container it uses has been deleted three
# times, and each time the replay silently stopped being run until someone
# noticed. This script rebuilds it rather than failing.
#
# Postgres runs natively inside an unprivileged LXC container. Nothing is
# installed on the hypervisor.
#
#   scripts/db2_replay_env.sh
#
# The replay is NOT re-runnable against a used database: memory.health_record is
# declared unsafe precisely because it appends a row per call, so the
# health-counters assertion fails on a second run against the same database.
# The database is therefore dropped and recreated every time.
set -uo pipefail

PVE=${DB2_REPLAY_PVE:-root@192.168.1.252}
CT=${DB2_REPLAY_CT:-9001}
DB=${DB2_REPLAY_DB:-aimee_db2_process_ci}
TEMPLATE=${DB2_REPLAY_TEMPLATE:-local:vztmpl/debian-13-standard_13.6-1_amd64.tar.zst}

say() { printf 'db2-replay: %s\n' "$1" >&2; }
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$PVE" "$@"; }

if ! on_host true 2>/dev/null; then
   say "cannot reach $PVE over ssh"
   exit 1
fi

if ! on_host "pct status $CT" >/dev/null 2>&1; then
   say "container $CT is missing; creating it"
   on_host "pct create $CT $TEMPLATE --hostname db2-replay --cores 4 --memory 4096 \
      --rootfs local-lvm:16 --net0 name=eth0,bridge=vmbr0,ip=dhcp --unprivileged 1 \
      --features nesting=1 --onboot 0" >/dev/null 2>&1 || { say "create failed"; exit 1; }
fi

if [ "$(on_host "pct status $CT" 2>/dev/null)" != "status: running" ]; then
   on_host "pct start $CT" >/dev/null 2>&1
   # The container needs an address before anything can be installed into it.
   for _ in $(seq 1 30); do
      on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null | grep -q inet && break
      sleep 2
   done
fi


# Renew the lease before doing anything long. The reaper reclaims a guest 4h
# after creation or its last renewal; nothing here ever renewed, which is why
# this container has been destroyed repeatedly mid-run. A renewal here buys a
# full 4h for the run that is about to start.
on_host "aimee-keepalive ct:$CT" >/dev/null 2>&1 ||
   say "could not renew the lease; this container is reaped 4h after creation"

if ! on_host "pct exec $CT -- test -x /usr/lib/postgresql/17/bin/postgres" 2>/dev/null; then
   say "installing postgres, pgvector and pgvectorscale"
   on_host "pct exec $CT -- bash -lc 'export DEBIAN_FRONTEND=noninteractive; \
      apt-get -qq update && apt-get -qq -y install postgresql postgresql-17-pgvector'" \
      >/dev/null 2>&1 || { say "install failed"; exit 1; }
   # pgvectorscale, pinned to the release the image uses. The schema requires it
   # -- diskann is the only index method over the embedding columns -- so an
   # environment without it cannot apply the schema, never mind compare
   # anything. Upstream ships a built .deb per (version, pg major, arch); the
   # container's postgres is 17.
   say "installing pgvectorscale 0.9.0"
   cat <<'PGVS' | on_host "pct exec $CT -- bash -s" >/dev/null 2>&1 || { say "pgvectorscale install failed"; exit 1; }
set -eu
if ls /usr/lib/postgresql/17/lib/vectorscale*.so >/dev/null 2>&1; then exit 0; fi
export DEBIAN_FRONTEND=noninteractive
apt-get -qq -y install curl unzip >/dev/null
cd /tmp
curl -fsSL -o pgvs.zip \
  https://github.com/timescale/pgvectorscale/releases/download/0.9.0/pgvectorscale-0.9.0-pg17-amd64.zip
unzip -p pgvs.zip 'pgvectorscale-postgresql-17_0.9.0-Linux_amd64.deb' > pgvs.deb
dpkg -i pgvs.deb
ls /usr/lib/postgresql/17/lib/vectorscale*.so
PGVS
fi

# Listen on the bridge and trust the local network: this container holds
# throwaway test data and is recreated whenever it goes missing.
#
# OUTSIDE the install branch, and checked rather than assumed. This ran only
# when postgres was installed, so a container that already had postgres but had
# lost its configuration kept a loopback-only server forever. The schema still
# applies -- that goes over the unix socket -- and only the module's TCP connect
# is refused, which surfaces as "db2: database initialization failed" and sends
# whoever reads it into the C. Restart only when something actually changed, so
# the common case costs two greps.
cat <<'PGNET' | on_host "pct exec $CT -- bash -s" >/dev/null 2>&1
set -eu
conf=/etc/postgresql/17/main/postgresql.conf
hba=/etc/postgresql/17/main/pg_hba.conf
changed=0
if ! grep -qE "^listen_addresses *= *'[*]'" "$conf"; then
   sed -i "/^ *#\\?listen_addresses/d" "$conf"
   printf "listen_addresses = '*'\\n" >> "$conf"
   changed=1
fi
if ! grep -q '192.168.0.0/23 trust' "$hba"; then
   printf 'host all all 192.168.0.0/23 trust\n' >> "$hba"
   changed=1
fi
[ "$changed" = 0 ] || systemctl restart postgresql
PGNET

# Everything below runs SQL from a file inside the container. Sending it as
# psql -c through ssh, pct exec and su means four levels of quoting, which is
# how the role creation was silently lost the first time this script ran.
run_sql() {
   local database=$1 sql=$2
   on_host "pct exec $CT -- bash -c 'cat > /tmp/db2-replay.sql'" <<<"$sql" || return 1
   on_host "pct exec $CT -- su postgres -c 'psql -XAt -v ON_ERROR_STOP=1 -d $database \
      -f /tmp/db2-replay.sql'"
}

if ! run_sql postgres "SELECT 1 FROM pg_roles WHERE rolname = 'aimee'" | grep -q 1; then
   say "creating the aimee role"
   run_sql postgres "CREATE ROLE aimee LOGIN SUPERUSER PASSWORD 'aimee'" >/dev/null ||
      { say "could not create the aimee role"; exit 1; }
fi

DBHOST=$(on_host "pct exec $CT -- ip -4 -o addr show eth0" 2>/dev/null |
   sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)
if [ -z "$DBHOST" ]; then
   say "container $CT has no address"
   exit 1
fi
say "using postgres at $DBHOST"

# Anything still holding a connection blocks the drop; the replay leaves its
# module running if it aborted.
pkill -f aimee-module-db2-replay 2>/dev/null
pkill -f unit-test-bus-db2-process 2>/dev/null

run_sql postgres "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null ||
   { say "could not drop $DB"; exit 1; }
run_sql postgres "CREATE DATABASE $DB OWNER aimee" >/dev/null ||
   { say "could not create $DB"; exit 1; }
run_sql "$DB" "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS vectorscale; CREATE EXTENSION IF NOT EXISTS pg_trgm" \
   >/dev/null || { say "could not create the extensions $DB needs"; exit 1; }

cd "$(dirname "$0")/.." || exit 1

# Both the pre-applied schema and the replay itself must agree on this.
EMBED_DIM=384

# Copy a local SQL file into the container and run it there, for anything too
# large to pass through run_sql's heredoc.
run_file() {
   local database=$1 path=$2
   # schema.sql carries the __EMBED_DIM__ placeholder that db_schema.c fills in
   # at runtime, so it is not valid SQL until the dimension is substituted. The
   # value has to match the EMBEDDER_DIMS the replay runs with, or the module
   # would find a vector of the wrong width already there.
   sed "s/__EMBED_DIM__/$EMBED_DIM/g" "$path" |
      on_host "pct exec $CT -- bash -c 'cat > /tmp/db2-replay-file.sql'" || return 1
   on_host "pct exec $CT -- su postgres -c 'psql -XAt -v ON_ERROR_STOP=1 -d $database \
      -f /tmp/db2-replay-file.sql'"
}

# The replay's module applies the schema itself on connect, so the database
# only has to exist. It is applied here as well, first, because some replay
# cases need a row to exist before the module starts: on a fresh schema every
# read answers zero and every write against a missing row is refused, and
# neither distinguishes a working operation from a broken one.
say "applying the schema so the replay can be seeded"
run_file "$DB" src/modules/db2/c/schema.sql >/dev/null ||
   { say "could not apply the schema to $DB"; exit 1; }

# Two artifacts. The first is committed and carries a payload of its own, for
# the flag-review replay to merge into and for the assertion afterwards to read
# back. Both ends of a link are foreign keys and a citation's citing end is
# one, so the cite and link replays need real rows or they can only ever show a
# refusal.
run_sql "$DB" "INSERT INTO artifacts (id, kind, state, payload)
   VALUES ('replay-flag-probe', 'probe', 'committed', '{\"kept\": 1}'::jsonb),
          ('replay-link-target', 'probe', 'proposed', '{}'::jsonb)
   ON CONFLICT (id) DO UPDATE SET state = EXCLUDED.state, payload = EXCLUDED.payload" \
   >/dev/null || { say "could not seed the artifact fixtures"; exit 1; }

AIMEE_DB2_URL="postgres://aimee:aimee@$DBHOST:5432/$DB" \
   EMBEDDER_DIMS=$EMBED_DIM \
   make -C src -j8 GIT_VERSION=ci GIT_COMMIT_TIME=1700000000 db2-replay || exit 1

# What the replay cannot assert for itself: the flagged artifact came back with
# its own payload intact beside the flag, and its state reset. A merge that
# dropped the existing keys, and a write that never happened, both satisfy the
# replay's assertion on the reply and fail here.
say "checking what the flag-review replay wrote"
flagged=$(run_sql "$DB" "SELECT state
   || ' kept=' || COALESCE((payload->>'kept'), 'missing')
   || ' flagged=' || COALESCE((payload->>'flagged_for_review'), 'missing')
   || ' reason=' || COALESCE((payload->>'flagged_reason'), 'missing')
   FROM artifacts WHERE id = 'replay-flag-probe'")
if [ "$flagged" != "proposed kept=1 flagged=true reason=replayed" ]; then
   say "flag-review left the artifact as: ${flagged:-<no row>}"
   exit 1
fi
say "flag-review merged the flag into the artifact's own payload"

# Multi-valued labels, against the real schema rather than a substring match on
# it. Scope visibility is a four-way disjunction -- active project, active
# workspace, global, shared, and the ABSENCE of any scope row meaning
# legacy-untagged -- and it becomes one set-membership predicate on the wire only
# if a point can carry every scope it belongs to under ONE key. A JSONB object
# cannot hold a key twice, so the value may be an array, and everything
# downstream counts PAIRS rather than keys.
say "checking the outbox contract accepts a multi-valued label"
run_sql "$DB" "INSERT INTO db3_provider (principal, state, corpus_generation)
   VALUES (424242, 'active', 1)
   ON CONFLICT (principal) DO UPDATE SET state = EXCLUDED.state,
     corpus_generation = EXCLUDED.corpus_generation" >/dev/null ||
   { say "could not seed a provider for the label probe"; exit 1; }

# The enqueue and the read-back are two STATEMENTS, not one.
#
# db3_enqueue_vector INSERTs, and a statement reads the snapshot taken when it
# started, so a scan of db3_outbox in the same statement as the enqueue cannot
# see the row the enqueue just wrote -- whatever the call sits in. Two earlier
# versions of this check were one statement (the call in WHERE, then the call in
# a MATERIALIZED CTE) and both reported a refusal for a contract that was
# working. The id is captured with \gset and read back in the next statement,
# which takes a new snapshot.
#
# COALESCE rather than a bare aggregate, so a zero return -- the enqueue finding
# no provider generation -- is distinguishable from a refusal.
multi=$(run_sql "$DB" "SELECT public.db3_enqueue_vector(
     'memory', 987654321, 'upsert', '[0.5]',
     '{\"record_type\":\"memory\",\"visibility\":[\"workspace:acme\",\"global\"]}'::jsonb)
     AS enqueued
\gset
SELECT COALESCE(string_agg(pair.key || '=' || pair.value, ',' ORDER BY pair.key, pair.value),
                'operation ' || :enqueued || ' wrote no labels')
  FROM public.db3_outbox AS outbox,
       LATERAL public.db3_label_pairs(outbox.labels) AS pair
 WHERE outbox.operation_id = :enqueued")
if [ "$multi" != "record_type=memory,visibility=global,visibility=workspace:acme" ]; then
   say "a multi-valued label did not survive the outbox: ${multi:-<refused>}"
   exit 1
fi
say "outbox carried a repeated key as separate pairs"

# Each of these must be refused, and each stands for a real way a point would go
# out wrong: a label that vanishes on the wire, a value that is not text, and a
# key whose values exceed the wire's label count once flattened. Bounding KEYS
# rather than pairs would have let the last one through.
for bad in \
   'empty-array:{"visibility":[]}' \
   'non-string:{"visibility":["global",7]}' \
   'duplicate-pair:{"visibility":["global","global"]}' \
   'over-count:{"visibility":["a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q"]}'
do
   name=${bad%%:*}
   labels=${bad#*:}
   if run_sql "$DB" "SELECT db3_enqueue_vector('memory', 987654322, 'upsert', '[0.5]',
      '$(printf '%s' "$labels" | sed "s/'/''/g")'::jsonb)" >/dev/null 2>&1; then
      say "the outbox accepted a label form it must refuse: $name"
      exit 1
   fi
done
say "outbox refused every malformed multi-valued label"

# ---------------------------------------------------------------- visibility
#
# The point of the multi-valued label: scope visibility, denormalised onto the
# point so a provider that can neither join nor ask about a missing row can
# still answer it. memory_visibility_labels() writes the point's half and
# db2_memory_visibility_filter_values() builds the caller's; visible means the
# two sets intersect, which is what DB2_MEMORY_SCOPE_RANK_SQL > 0 means today.
#
# The matrix below is the discriminating one. Every case is a scope shape that
# a set-membership test could get wrong in a way no smaller fixture would show:
# the legacy memory_workspaces row that carries no memory_scopes row at all, and
# the global row whose value is not '_global' -- which leaves a memory neither
# globally visible NOR untagged, because the untagged clause is the absence of
# the ROW and not of the value.
say "checking visibility labels against the scope rank"
run_sql "$DB" "
INSERT INTO memories (id, key) VALUES
  (9200001,'vis-project'),(9200002,'vis-workspace'),(9200003,'vis-global'),
  (9200004,'vis-shared'),(9200005,'vis-untagged'),(9200006,'vis-legacy-ws'),
  (9200007,'vis-global-other'),(9200008,'vis-project-and-workspace')
ON CONFLICT (id) DO NOTHING;
INSERT INTO memory_scopes (memory_id, scope_type, scope_value) VALUES
  (9200001,'project','pj-a'),
  (9200002,'workspace','ws-a'),
  (9200003,'global','_global'),
  (9200004,'workspace','_shared'),
  (9200007,'global','other'),
  (9200008,'project','pj-a'),(9200008,'workspace','ws-b')
ON CONFLICT DO NOTHING;
INSERT INTO memory_workspaces (memory_id, workspace) VALUES (9200006,'ws-a')
ON CONFLICT DO NOTHING;" >/dev/null ||
   { say "could not seed the visibility fixtures"; exit 1; }

# The values a caller in (workspace ws-a, project pj-a) may see, written out
# rather than computed, so this states the vocabulary instead of re-deriving it.
inside="ARRAY['project:pj-a','workspace:ws-a','global','workspace:_shared','untagged']"
outside="ARRAY['project:pj-z','workspace:ws-z','global','workspace:_shared','untagged']"

for probe in \
   "9200001:project scope:true:false" \
   "9200002:workspace scope:true:false" \
   "9200003:global row:true:true" \
   "9200004:_shared workspace row:true:true" \
   "9200005:no scope rows at all:true:true" \
   "9200006:legacy memory_workspaces row:true:false" \
   "9200007:global row that is not _global:false:false" \
   "9200008:project and workspace rows:true:false"
do
   memory=$(printf '%s' "$probe" | cut -d: -f1)
   name=$(printf '%s' "$probe" | cut -d: -f2)
   want_in=$(printf '%s' "$probe" | cut -d: -f3)
   want_out=$(printf '%s' "$probe" | cut -d: -f4)
   got=$(run_sql "$DB" "SELECT
       (SELECT bool_or(v.value = ANY($inside))
          FROM jsonb_array_elements_text(memory_visibility_labels($memory)) AS v(value))
       IS TRUE
    || ':' ||
       ((SELECT bool_or(v.value = ANY($outside))
           FROM jsonb_array_elements_text(memory_visibility_labels($memory)) AS v(value))
        IS TRUE)")
   if [ "$got" != "$want_in:$want_out" ]; then
      say "$name: visible in/out = ${got:-<refused>}, expected $want_in:$want_out"
      exit 1
   fi
done
say "visibility labels agree with the rank on every scope shape"

# The label has to reach the OUTBOX, not just the function. A projection whose
# label_sources never named the column would pass everything above.
vector="(SELECT '[' || string_agg('0.1', ',') || ']' FROM generate_series(1,$EMBED_DIM))"
run_sql "$DB" "INSERT INTO memory_embeddings (point_id, embedding, record_type)
   VALUES (9200005, ($vector)::vector, 'memory')
 ON CONFLICT (point_id) DO UPDATE SET embedding=EXCLUDED.embedding" >/dev/null ||
   { say "could not write the embedding the outbox probe reads"; exit 1; }
# Read in a separate call, not a separate statement: psql prints a command tag
# for anything that is not a SELECT, and a captured mixture of tag and result
# equals neither.
enqueued=$(run_sql "$DB" "SELECT
     COALESCE(string_agg(pair.value, ',' ORDER BY pair.value), 'no visibility label')
  FROM public.db3_outbox AS outbox,
       LATERAL public.db3_label_pairs(outbox.labels) AS pair
 WHERE outbox.point_id = 9200005 AND pair.key = 'visibility'")
if [ "$enqueued" != "untagged" ]; then
   say "an untagged memory reached the outbox as: ${enqueued:-<refused>}"
   exit 1
fi
say "outbox carried the point's visibility"

# A scope row is one row; the points that carried its answer are however many
# the memory has. Relabelling them is the cost the denormalisation buys, and a
# stale label is a point visible in a scope it has left.
run_sql "$DB" "INSERT INTO memory_scopes (memory_id, scope_type, scope_value)
   VALUES (9200005,'project','pj-a') ON CONFLICT DO NOTHING" >/dev/null ||
   { say "could not add the scope row the relabel probe changes"; exit 1; }
relabelled=$(run_sql "$DB" "SELECT
     COALESCE(string_agg(v.value, ',' ORDER BY v.value), 'none')
  FROM memory_embeddings e,
       LATERAL jsonb_array_elements_text(e.visibility) AS v(value)
 WHERE e.point_id = 9200005")
if [ "$relabelled" != "project:pj-a" ]; then
   say "a scope change left the point labelled: ${relabelled:-<refused>}"
   exit 1
fi
say "a scope change relabelled the points that carried its answer"
