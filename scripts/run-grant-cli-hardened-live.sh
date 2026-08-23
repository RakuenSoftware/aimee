#!/bin/bash
# run-grant-cli-hardened-live.sh — the write-tier grant path on the HARDENED tier.
#
# WHY THIS EXISTS, given run-grant-cli-live.sh already exists. That rig proved the composed
# path end to end, but on the SINGLE-NODE DEV SHAPE, and said so: kb connects as the OWNER
# role and applies the schema itself at boot, with no TLS to Postgres. Its own comment called
# the hardened tier "more rig than this test needs".
#
# A hardened deployment differs in three ways that can each independently break these routes,
# and none of them were exercised anywhere:
#
#   1. THE SCHEMA IS PRE-APPLIED, by a migrate role, before kb ever starts. kb creates nothing.
#      Object ownership is then split between two roles, which is exactly the failure class
#      ("must be owner of function ...") that cost several attempts on the dev rig. The grant
#      routes call SECURITY DEFINER functions, and a definer runs as its OWNER — so whether
#      they resolve correctly when the owner is not the connecting role is a real question.
#
#   2. kb CONNECTS AS aimee_kb_runtime, which is NOBYPASSRLS, owns nothing, and has no CREATE
#      on public. Whether that role can EXECUTE every function the grant routes need, and
#      whether RLS still admits the rows once aimee.principal is set by db2_tenant_scope_begin,
#      is not implied by the owner-role run.
#
#   3. sslmode=verify-full — real TLS to Postgres with a verified chain and a matching
#      hostname. db2_init treats this tier differently.
#
# So: same assertions as the dev rig, plus the hardened-specific ones, on the shape an actual
# hardened deployment runs. Nothing is stubbed and nothing is weakened to make it pass.
#
# MUST RUN AS ROOT on a host with Postgres.
# Usage: run-grant-cli-hardened-live.sh [--keep]
set -uo pipefail
export LC_ALL=C

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
keep=0
[ "${1:-}" = "--keep" ] && keep=1

[ "$(id -u)" = "0" ] || { echo "hardened-live: must run as root" >&2; exit 2; }
for b in ./aimee ./aimee-server ./aimee-kb; do
  [ -x "$b" ] || { echo "hardened-live: $b not built (make -C src all)" >&2; exit 2; }
done
# kb FORKS this beside its own executable. A missing resolver is not a startup error, it is a
# later failure that looks like something else entirely.
[ -x ./aimee-kb-resolver ] || { echo "hardened-live: ./aimee-kb-resolver not built" >&2; exit 2; }

db=aimee_grant_hardened_live
work=$(mktemp -d /root/grant-hardened.XXXXXX)
export AIMEE_HOME="$work/home"
mkdir -p "$AIMEE_HOME"
# NOT under $work: that lives in /root, which is mode 700, so the postgres user cannot traverse
# to the key no matter how the key itself is permissioned — the server just fails to start.
certs=/var/lib/postgresql/aimee-hardened-certs.$$
mkdir -p "$certs"
chown postgres:postgres "$certs"
chmod 750 "$certs"
kb_log=$work/kb.log
srv_log=$work/server.log
kb_pid=""; srv_pid=""
pgconf=""; pgconf_backup=""

step() { printf '\n== %s\n' "$*"; }
fail() {
  echo "FAIL: $*" >&2
  echo "--- kb tail:"; tail -30 "$kb_log" 2>/dev/null
  echo "--- server tail:"; tail -20 "$srv_log" 2>/dev/null
  exit 1
}
psqlq() { runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" "$@"; }
psqlt() { runuser -u postgres -- psql -tAX -d "$db" "$@"; }

# Both roles are CLUSTER-global, so this rig hands them back exactly as it found them.
# rolpassword is the SCRAM verifier and ALTER ROLE ... PASSWORD takes an already-hashed string
# verbatim, so the original goes back without the cleartext ever being known.
declare -A pw_before login_before existed_before
snapshot_role() {
  existed_before[$1]=$(runuser -u postgres -- psql -tAX -c \
    "SELECT count(*) FROM pg_authid WHERE rolname='$1'" 2>/dev/null | tr -d ' ')
  pw_before[$1]=$(runuser -u postgres -- psql -tAX -c \
    "SELECT coalesce(rolpassword,'') FROM pg_authid WHERE rolname='$1'" 2>/dev/null)
  login_before[$1]=$(runuser -u postgres -- psql -tAX -c \
    "SELECT rolcanlogin FROM pg_authid WHERE rolname='$1'" 2>/dev/null | tr -d ' ')
}
restore_role() {
  [ "${existed_before[$1]:-0}" = "0" ] && return 0
  if [ -n "${pw_before[$1]:-}" ]; then
    runuser -u postgres -- psql -q -c "ALTER ROLE $1 PASSWORD '${pw_before[$1]}'" >/dev/null 2>&1
  else
    runuser -u postgres -- psql -q -c "ALTER ROLE $1 PASSWORD NULL" >/dev/null 2>&1
  fi
  # BOTH directions. schema_roles.sql declares these roles NOLOGIN, so a rig that only ever
  # adds NOLOGIN silently leaves a role that could log in before the run unable to afterwards.
  case "${login_before[$1]:-}" in
    t) runuser -u postgres -- psql -q -c "ALTER ROLE $1 LOGIN" >/dev/null 2>&1 ;;
    f) runuser -u postgres -- psql -q -c "ALTER ROLE $1 NOLOGIN" >/dev/null 2>&1 ;;
  esac
  return 0
}

cleanup() {
  [ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null
  [ -n "$kb_pid" ] && kill "$kb_pid" 2>/dev/null
  sleep 1
  [ -n "$srv_pid" ] && kill -9 "$srv_pid" 2>/dev/null
  [ -n "$kb_pid" ] && kill -9 "$kb_pid" 2>/dev/null
  # TLS is switched on in the cluster's own config, so it must come back off (or back to
  # whatever it was) even when an assertion fails midway.
  if [ -n "$pgconf_backup" ] && [ -f "$pgconf_backup" ]; then
    cp -- "$pgconf_backup" "$pgconf" 2>/dev/null
    chown postgres:postgres "$pgconf" 2>/dev/null
    # RESTART, not reload: a reload of a cluster that failed to start trivially "succeeds" and
    # leaves it down, which is how an early run of this script left the box.
    systemctl restart postgresql@17-main 2>/dev/null || systemctl restart postgresql 2>/dev/null
    pg_up=0
    for _ in $(seq 1 30); do
      runuser -u postgres -- psql -tAX -c 'SELECT 1' >/dev/null 2>&1 && { pg_up=1; break; }
      sleep 1
    done
    # Loud, because leaving the cluster down is worse than any assertion this script makes.
    [ "$pg_up" = "1" ] || echo "hardened-live: WARNING - Postgres did not come back up; \
config restored to $pgconf, start it with: systemctl restart postgresql" >&2
  fi
  rm -rf -- "$certs" 2>/dev/null
  if [ "$keep" = "1" ]; then
    echo "hardened-live: keeping db=$db work=$work"
    return
  fi
  restore_role aimee_kb_migrate
  restore_role aimee_kb_runtime
  restore_role aimee_kb_owner
  runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
  rm -rf -- "$work"
}
trap cleanup EXIT

step "Issuing a CA and a server certificate for Postgres"
# verify-full checks the chain AND that the hostname matches, so the CN/SAN must be the host
# kb will actually dial. 127.0.0.1 as an IP SAN, because that is the DSN host.
openssl req -new -x509 -days 2 -nodes -newkey rsa:2048 \
  -subj "/CN=aimee-hardened-test-ca" \
  -keyout "$certs/ca.key" -out "$certs/ca.crt" >/dev/null 2>&1 || fail "CA generation"
openssl req -new -nodes -newkey rsa:2048 -subj "/CN=127.0.0.1" \
  -keyout "$certs/server.key" -out "$certs/server.csr" >/dev/null 2>&1 || fail "server CSR"
cat > "$certs/server.ext" <<EXT
subjectAltName = IP:127.0.0.1, DNS:localhost
EXT
openssl x509 -req -in "$certs/server.csr" -CA "$certs/ca.crt" -CAkey "$certs/ca.key" \
  -CAcreateserial -days 2 -extfile "$certs/server.ext" \
  -out "$certs/server.crt" >/dev/null 2>&1 || fail "server certificate"
chown postgres:postgres "$certs/server.key" "$certs/server.crt"
chmod 600 "$certs/server.key"
echo "  CA + server cert with IP:127.0.0.1 SAN"

step "Turning on TLS in the running Postgres cluster"
pgconf=$(runuser -u postgres -- psql -tAX -c 'SHOW config_file' | tr -d ' ')
[ -n "$pgconf" ] && [ -f "$pgconf" ] || fail "could not locate postgresql.conf"
pgconf_backup=$work/postgresql.conf.bak
cp -- "$pgconf" "$pgconf_backup" || fail "could not back up postgresql.conf"
# Appended, so the originals are the ones overridden and the backup restores them.
cat >> "$pgconf" <<CONF

# added by run-grant-cli-hardened-live.sh (restored on exit)
ssl = on
ssl_cert_file = '$certs/server.crt'
ssl_key_file = '$certs/server.key'
CONF
if ! systemctl restart postgresql@17-main 2>/dev/null && ! systemctl restart postgresql; then
  echo "--- postgres log:"; tail -25 "$(runuser -u postgres -- \
    psql -tAX -c 'SHOW log_directory' 2>/dev/null)"/*.log 2>/dev/null
  journalctl -u postgresql@17-main -n 25 --no-pager 2>/dev/null | tail -25
  fail "postgres would not restart with TLS on"
fi
for i in $(seq 1 30); do
  runuser -u postgres -- psql -tAX -c 'SELECT 1' >/dev/null 2>&1 && break
  sleep 1
done
[ "$(runuser -u postgres -- psql -tAX -c 'SHOW ssl' | tr -d ' ')" = "on" ] \
  || fail "TLS did not come up in the cluster"
echo "  ssl=on"

step "Provisioning: roles, then the MIGRATE role applies the schema, before kb exists"
snapshot_role aimee_kb_owner
snapshot_role aimee_kb_migrate
snapshot_role aimee_kb_runtime
runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -f src/modules/db2/c/schema_roles.sql >/dev/null 2>&1
runuser -u postgres -- createdb -O aimee_kb_owner "$db" || fail "createdb"
psqlq -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;' \
  || fail "extensions"
psqlq -f src/modules/db2/c/schema_roles.sql >/dev/null 2>&1
# PG15+ stopped granting CREATE on public to non-owners. A real migrate step holds this.
psqlq -c 'GRANT USAGE, CREATE ON SCHEMA public TO aimee_kb_owner, aimee_kb_migrate' >/dev/null 2>&1

migpw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')
psqlq -c "ALTER ROLE aimee_kb_migrate LOGIN PASSWORD '$migpw'" >/dev/null 2>&1 \
  || fail "could not give aimee_kb_migrate a password"
# THE DEFINING DIFFERENCE FROM THE DEV RIG: the schema is applied here, out of band, as the
# migrate role, over TLS — not by kb at boot. Objects end up owned by aimee_kb_owner (migrate
# is a member of owner), and kb will connect as a role that owns none of them.
# schema.sql IS A TEMPLATE, not a script: it declares its vector columns as
# vector(__EMBED_DIM__) so a deployment can pick an embedder, and db_schema.c calls itself
# "the one place the schema is applied to Postgres" because on the dev path it is. On the
# HARDENED path it is not — the migrate step applies the schema out of band, so the migrate
# tooling has to do the substitution kb would have done, and handing schema.sql straight to
# psql fails with `invalid input syntax for type integer: "__embed_dim__"`. This rig found that
# by being the first thing to apply the schema the way a hardened deployment does.
#
# EMBED_DIM must equal the embedding_dim kb is configured with, or the vector columns will not
# match the embedder; that equality is asserted below rather than left to coincide.
EMBED_DIM=1024
sed "s/__EMBED_DIM__/$EMBED_DIM/g" src/modules/db2/c/schema.sql | \
  PGPASSWORD=$migpw PGSSLROOTCERT=$certs/ca.crt \
  psql -q -v ON_ERROR_STOP=1 \
  "postgresql://aimee_kb_migrate@127.0.0.1:5432/$db?sslmode=verify-full" \
  -f - > "$work/migrate.log" 2>&1 \
  || { echo "--- migrate log:"; tail -30 "$work/migrate.log"; fail "migrate-role schema apply"; }
echo "  schema applied by aimee_kb_migrate over verify-full TLS (embed_dim=$EMBED_DIM)"

step "The schema is re-appliable (a second apply must be clean)"
# The dev rig documented an FK/UNIQUE drop-order fix but never reproduced it, because it only
# ever applied the schema once. A hardened deployment re-runs migrations over an existing
# database on every upgrade, so this is that path.
sed "s/__EMBED_DIM__/$EMBED_DIM/g" src/modules/db2/c/schema.sql | \
  PGPASSWORD=$migpw PGSSLROOTCERT=$certs/ca.crt \
  psql -q -v ON_ERROR_STOP=1 \
  "postgresql://aimee_kb_migrate@127.0.0.1:5432/$db?sslmode=verify-full" \
  -f - > "$work/migrate2.log" 2>&1 \
  || { echo "--- second apply log:"; tail -30 "$work/migrate2.log"; \
       fail "schema.sql is not re-appliable"; }
echo "  second apply exited 0"

step "Phase 3: schema_grants.sql gives the runtime role its DML and EXECUTE"
# THE HARDENED DEPLOY IS THREE PHASES, not two: roles (create) -> schema (DDL) -> grants
# (runtime DML/EXECUTE), the order run-p1-rls-gate.sh documents. Phase 3 has to come last
# because GRANT EXECUTE ON ALL FUNCTIONS is evaluated against the functions that exist when it
# runs -- omit it, or run it before the schema, and the runtime role can execute none of the
# definers. An earlier draft of this rig skipped it and every grant would have failed.
# Phases 1 and 3 are SUPERUSER operations and phase 2 is not: schema_grants.sql re-asserts
# role attributes with ALTER ROLE ... NOSUPERUSER, which only a superuser may do, so handing
# phase 3 to the migrate role fails with "permission denied to alter role". Only the DDL is the
# migrate role's job, and only kb's own connection is the one that must be verify-full TLS --
# these two run over the local admin socket, as a real deployment's provisioning does.
psqlq -f src/modules/db2/c/schema_grants.sql > "$work/grants.log" 2>&1 \
  || { echo "--- grants log:"; tail -30 "$work/grants.log"; fail "phase-3 grants apply"; }
echo "  applied as superuser (phase 2 stays the migrate role's)"

step "The applied vector width matches the dimension kb will be configured with"
# If these ever diverge, kb boots against columns the embedder cannot fill, and the failure
# surfaces far from its cause.
applied_dim=$(psqlt -c "SELECT atttypmod FROM pg_attribute
                          WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'")
[ "$applied_dim" = "$EMBED_DIM" ] \
  || fail "memory_embeddings.embedding is vector($applied_dim) but kb is pinned to $EMBED_DIM"
echo "  vector($applied_dim) == pinned embedding_dim $EMBED_DIM"

step "Confirming the grant machinery is owned by the owner role, not the runtime role"
# These three are the SECURITY DEFINER functions; the exact-lookup path reads the
# kb_write_tier_grant_live view rather than a function, which is checked separately below.
for fn in kb_write_tier_grant_set kb_write_tier_grant_set_reporting \
          kb_write_tier_grant_revoke; do
  owner=$(psqlt -c "SELECT pg_get_userbyid(proowner) FROM pg_proc WHERE proname='$fn' LIMIT 1")
  [ -n "$owner" ] || fail "$fn does not exist after the migrate apply"
  [ "$owner" != "aimee_kb_runtime" ] || fail "$fn is owned by the runtime role"
  echo "  $fn owned by $owner"
done
# A definer runs as its owner, so "not the runtime role" is the property that matters. Here the
# owner is the migrate role, which is a member of aimee_kb_owner — objects belong to whoever
# created them, and on the hardened tier that is the migrate step, not kb.
# The reads (lookup, list) go straight at the kb_write_tier_grant TABLE under RLS; only the
# writes go through the definers. So the runtime role needs SELECT on the table itself.
[ -n "$(psqlt -c "SELECT 1 FROM pg_tables WHERE tablename='kb_write_tier_grant'")" ] \
  || fail "kb_write_tier_grant does not exist after the migrate apply"
[ "$(psqlt -c "SELECT relrowsecurity FROM pg_class WHERE relname='kb_write_tier_grant'")" = "t" ] \
  || fail "RLS is not enabled on kb_write_tier_grant"
echo "  kb_write_tier_grant present with RLS enabled"
# The runtime role has to be able to USE all of it without owning any of it.
for fn in kb_write_tier_grant_set kb_write_tier_grant_set_reporting kb_write_tier_grant_revoke; do
  ok=$(psqlt -c "SELECT bool_or(has_function_privilege('aimee_kb_runtime', p.oid, 'EXECUTE'))
                   FROM pg_proc p WHERE p.proname='$fn'")
  [ "$ok" = "t" ] || fail "aimee_kb_runtime cannot EXECUTE $fn"
done
[ "$(psqlt -c "SELECT has_table_privilege('aimee_kb_runtime','kb_write_tier_grant','SELECT')")" = "t" ] \
  || fail "aimee_kb_runtime cannot SELECT kb_write_tier_grant"
echo "  runtime role can EXECUTE all three definers and read the table, owning none of them"

step "Starting aimee-kb as aimee_kb_runtime over verify-full TLS"
kbpw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')
psqlq -c "ALTER ROLE aimee_kb_runtime LOGIN PASSWORD '$kbpw'" >/dev/null 2>&1 \
  || fail "could not give aimee_kb_runtime a password"
export AIMEE_KB_API_BEARER_TOKEN="hardened-grant-token"
KB_PORT=18743
# Boot recipe, all of it learned the hard way on the dev rig — see run-grant-cli-live.sh for the
# full account. In short: TCP DSN not socket (kb runs as root, peer auth would say root);
# embedding_dim PINNED (db2_init otherwise reads the recorded dim and a read failure is fatal);
# the port ONLY via --http-port (AIMEE_KB_PORT and a flat kb_api_http_port are both ignored, and
# kb then binds nothing); and the bearer token as the NESTED kb.api.bearer_token, which is what
# kb VALIDATES against — with no token kb runs auth-off and manufactures no owner actor on
# purpose, so every grant call 401s.
# aimee.api.{http_port,bearer_token} opens the OPTIONAL localhost TCP listener, and
# remote_writes: full is deliberately the most permissive setting there is -- because
# server_http_conn_caps hands a remote_writes=full TCP bearer CAPS_ALL. That is exactly the
# situation v1_route_requires_uds exists to stop, so the UDS gate below is only a real test with
# this configured. Without it there is no TCP listener and the assertion can only be skipped.
SRV_TCP_PORT=18745
export AIMEE_SERVER_API_BEARER="hardened-tcp-token"
cat > "$AIMEE_HOME/aimee.yaml" <<YAML
embedding_dim: 1024
kb:
  api:
    bearer_token: $AIMEE_KB_API_BEARER_TOKEN
aimee:
  api:
    http_port: $SRV_TCP_PORT
    bearer_token: $AIMEE_SERVER_API_BEARER
    remote_writes: full
YAML
# sslmode=verify-full is the hardened tier's requirement, and sslrootcert is what makes it
# verifiable rather than a claim.
export AIMEE_DB2_URL="postgres://aimee_kb_runtime:$kbpw@127.0.0.1:5432/$db?sslmode=verify-full&sslrootcert=$certs/ca.crt"
# THE FLAG THAT MAKES THIS THE HARDENED TIER. Without it kb takes the dev path and applies the
# schema itself at boot -- which as the runtime role fails with "permission denied for schema
# public" and kb never becomes healthy, retrying until it is killed. With it, db2_init calls
# db2_verify_pre_provisioned instead and NEVER runs DDL: it checks that the owner-migrated
# schema is present and dimension-compatible, and fails closed if it is not. This flag is also
# what makes kb require an sslmode=verify-full DSN, asserted separately below.
export AIMEE_KB_HARDENED=1
./aimee-kb --http-port="$KB_PORT" >"$kb_log" 2>&1 &
kb_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
    "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 && break
  kill -0 "$kb_pid" 2>/dev/null || fail "aimee-kb exited"
  sleep 1
done
curl -sf -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
  "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 || fail "aimee-kb never became healthy"
echo "  aimee-kb healthy on $KB_PORT as aimee_kb_runtime"

step "Hardened mode REFUSES a DSN that is not verify-full"
# The tier's TLS requirement has to be enforced by the product, not merely satisfied by this
# rig's DSN. A hardened kb that quietly accepted sslmode=require would be the whole point
# missed, so this boots one with a weakened DSN and requires it to fail.
(
  export AIMEE_DB2_URL="postgres://aimee_kb_runtime:$kbpw@127.0.0.1:5432/$db?sslmode=require"
  export AIMEE_KB_HARDENED=1
  timeout 25 ./aimee-kb --http-port=18744 >"$work/kb_weak.log" 2>&1
)
weak_rc=$?
# The EXACT message from db2_init.c, not a keyword search: the DSN is echoed in every retry
# line and contains the string "sslmode", so a grep for that can never fail and would assert
# nothing. kb also RETRIES db2_init rather than exiting on the first refusal, so the exit code
# here is timeout's 124 rather than kb's own -- which is why the message, not the code, is the
# evidence.
grep -q 'hardened tier requires sslmode=verify-full' "$work/kb_weak.log" \
  || { echo "--- weak-DSN log:"; tail -15 "$work/kb_weak.log"; \
       fail "hardened kb did not refuse a non-verify-full DSN for the documented reason"; }
[ "$weak_rc" != "0" ] || fail "hardened kb exited 0 with a non-verify-full DSN"
echo "  refused with the documented message (kb retries, so rc=$weak_rc is the timeout's)"

step "PROVING the connection is really encrypted, not just asked to be"
# A verify-full DSN that silently fell back to cleartext is the exact thing this rig exists to
# catch, and the DSN string is not evidence. pg_stat_ssl reports what the backend negotiated.
ssl_rows=$(psqlt -c "SELECT count(*) FROM pg_stat_ssl s JOIN pg_stat_activity a USING (pid)
                       WHERE a.usename='aimee_kb_runtime' AND s.ssl")
plain_rows=$(psqlt -c "SELECT count(*) FROM pg_stat_ssl s JOIN pg_stat_activity a USING (pid)
                         WHERE a.usename='aimee_kb_runtime' AND NOT s.ssl")
[ "${ssl_rows:-0}" -ge 1 ] || fail "no TLS-encrypted backend for aimee_kb_runtime (ssl=$ssl_rows)"
[ "${plain_rows:-0}" = "0" ] || fail "$plain_rows cleartext backend(s) for aimee_kb_runtime"
echo "  $ssl_rows encrypted backend(s), 0 cleartext"

step "Confirming the runtime role really is unprivileged"
# If this role could create objects, the hardened posture would be a label rather than a fact,
# and the SECURITY DEFINER assertions above would prove much less.
[ "$(psqlt -c "SELECT has_schema_privilege('aimee_kb_runtime','public','CREATE')")" = "f" ] \
  || fail "the runtime role has CREATE on public"
[ "$(psqlt -c "SELECT rolbypassrls FROM pg_authid WHERE rolname='aimee_kb_runtime'")" = "f" ] \
  || fail "the runtime role can bypass RLS"
[ "$(psqlt -c "SELECT rolsuper FROM pg_authid WHERE rolname='aimee_kb_runtime'")" = "f" ] \
  || fail "the runtime role is a superuser"
echo "  no CREATE on public, NOBYPASSRLS, not superuser"

step "Tenancy fixture: a team, a registered server, and owner as admin"
psqlq >/dev/null <<SQL || fail "fixture"
INSERT INTO kb_team(id,name) VALUES (990001,'grant_hardened_live');
INSERT INTO kb_team_membership(identity_key,team) VALUES ('alice',990001),('owner',990001);
INSERT INTO kb_admin_grant(identity_key,granted_by) VALUES ('owner','owner');
INSERT INTO kb_enrollments(id,scope,fingerprint,serial,state,expires_at,authority_id,
                           cert_issuer,cert_serial_norm)
  VALUES (990101,'p5-server-management',repeat('c',64),'01','active',now()+interval '90 days',
          repeat('d',32),'CN=ca','01');
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
                              mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
  VALUES ('livesrv','cn','mcn',990001,'https://livesrv','active','CN=ca','01',repeat('c',64));
SQL

step "Starting aimee-server pointed at it"
export AIMEE_KB_API_URL="http://127.0.0.1:$KB_PORT"
export AIMEE_SERVER_TEAM_ID=990001
./aimee-server >"$srv_log" 2>&1 &
srv_pid=$!
for i in $(seq 1 60); do
  ./aimee status >/dev/null 2>&1 && break
  kill -0 "$srv_pid" 2>/dev/null || fail "aimee-server exited"
  sleep 1
done
./aimee status >/dev/null 2>&1 || fail "aimee-server never became reachable"
echo "  aimee-server reachable over its unix socket"

# Every assertion below runs the SHIPPING CLI. No stubs anywhere in the path.
G() { ./aimee kb grant "$@" 2>&1; }
rows() { psqlt -c "$1"; }

step "set creates the grant, as the authenticated operator, on the hardened tier"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set: $out"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice'")" = "data" ] \
  || fail "the grant row does not say data (cli said: $out)"
# The granter is the actor kb AUTHENTICATED, never a value from the request body.
[ "$(rows "SELECT granted_by FROM kb_write_tier_grant WHERE subject='alice'")" = "owner" ] \
  || fail "granted_by is not the authenticated operator"
[ "$(rows "SELECT actor_principal FROM kb_audit_event
             WHERE action='authz.write_tier.set' LIMIT 1")" = "owner" ] \
  || fail "the audit row does not name the operator as actor"
# THIS is what the hardened tier adds: the SECURITY DEFINER write succeeded while connected as
# a non-owning, NOBYPASSRLS role against a schema someone else created.
echo "  row=data granted_by=owner audited=owner — as a non-owning runtime role"

step "The CLI TELLS the operator what happened (rendered text, not just a row)"
# The dev rig could only assert on Postgres because no formatter existed; one does now, so the
# operator-visible prose is part of the contract and gets asserted here.
printf '%s' "$out" | grep -q "created" \
  || fail "set printed nothing about creating the grant: $out"

step "re-granting the same tier is idempotent and says so"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set2: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "an idempotent re-grant duplicated the row"
printf '%s' "$out" | grep -q "unchanged" || fail "an unchanged re-grant did not say so: $out"
echo "  one row, reported unchanged"

step "changing the tier reports the tier it changed FROM"
out=$(G set --subject alice --server livesrv --team 990001 --tier full) || fail "set3: $out"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice'")" = "full" ] \
  || fail "the tier did not change to full"
printf '%s' "$out" | grep -q "changed from data" \
  || fail "the previous tier was not reported: $out"
echo "  data -> full, reported"

step "a NON-member grant is created but WARNS that it is inert"
out=$(G set --subject bob --server livesrv --team 990001 --tier data) || fail "set4: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='bob'")" = "1" ] \
  || fail "the non-member grant was not created"
[ "$(rows "SELECT count(*) FROM kb_team_membership WHERE identity_key='bob'")" = "0" ] \
  || fail "bob was unexpectedly a member, so this proves nothing"
# The warning an operator could not previously see. A grant to a non-member does nothing at all
# until they join, and silence read as success.
printf '%s' "$out" | grep -q "not a member" \
  || fail "no non-member warning was printed: $out"
echo "  created for a non-member, and the operator was warned"

step "show WITHOUT --subject sends nothing"
out=$(G show --server livesrv --team 990001)
printf '%s' "$out" | grep -q -- '--subject S is required' \
  || fail "show with no subject was not refused: $out"
printf '%s' "$out" | grep -q bob && fail "show with no subject LISTED EVERYTHING: $out"
echo "  refused before any request"

step "show renders the row it was asked for"
out=$(G show --subject alice --server livesrv --team 990001) || fail "show: $out"
printf '%s' "$out" | grep -q alice || fail "show did not render the subject: $out"
printf '%s' "$out" | grep -q bob && fail "show leaked another subject: $out"
echo "  alice rendered, bob absent"

step "list renders every row"
out=$(G list --server livesrv --team 990001) || fail "list: $out"
printf '%s' "$out" | grep -q alice || fail "list omitted alice: $out"
printf '%s' "$out" | grep -q bob || fail "list omitted bob: $out"
echo "  both subjects rendered"

step "revoke retains the row, sets revoked_at, warns about the residual token"
out=$(G revoke --subject alice --server livesrv --team 990001) || fail "revoke: $out"
[ "$(rows "SELECT revoked_at IS NOT NULL FROM kb_write_tier_grant WHERE subject='alice'")" = "t" ] \
  || fail "revoked_at was not set"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "revocation did not retain exactly one row"
[ "$(rows "SELECT actor_principal FROM kb_audit_event
             WHERE action='authz.write_tier.revoke' LIMIT 1")" = "owner" ] \
  || fail "the revoke audit row does not name the operator"
# The second warning an operator could not previously see: access is gone SHORTLY, not now.
printf '%s' "$out" | grep -q "300s" \
  || fail "no residual-token warning on revoke: $out"
echo "  retained, revoked_at set, audited as owner, residual window disclosed"

step "revoking something that was never granted is not reported as a revocation"
out=$(G revoke --subject nosuchuser --server livesrv --team 990001) || fail "revoke miss: $out"
printf '%s' "$out" | grep -q "no grant existed" \
  || fail "a no-op revoke did not say the grant did not exist: $out"
printf '%s' "$out" | grep -q "300s" \
  && fail "a no-op revoke warned about a residual token it never invalidated: $out"
echo "  reported as nothing to revoke, with no residual-token claim"

step "a revoked grant is HIDDEN by default and marked revoked when asked for"
# Revocation history is not shown unless it is asked for -- the default listing answers "who can
# write", which is the question an operator is usually asking.
out=$(G show --subject alice --server livesrv --team 990001) || fail "show revoked: $out"
printf '%s' "$out" | grep -q "no write-tier grants" \
  || fail "a revoked grant appeared in the default listing: $out"
out=$(G show --subject alice --server livesrv --team 990001 --include-revoked) \
  || fail "show --include-revoked: $out"
printf '%s' "$out" | grep -q alice || fail "--include-revoked did not return the row: $out"
printf '%s' "$out" | grep -q "revoked" \
  || fail "the revoked grant is not MARKED revoked: $out"
echo "  hidden by default, returned and marked with --include-revoked"

step "set after revoke clears revoked_at IN PLACE and reports reinstatement"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set5: $out"
[ "$(rows "SELECT revoked_at IS NULL FROM kb_write_tier_grant WHERE subject='alice'")" = "t" ] \
  || fail "revoked_at was not cleared"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "re-granting created a second row"
printf '%s' "$out" | grep -q "reinstated" \
  || fail "the reinstated revocation was not reported: $out"
echo "  cleared in place, one row, reinstatement reported"

step "an UNREGISTERED server writes nothing AND is refused visibly"
# The dev rig discarded the exit status here, so it would have passed on a crash or a success.
out=$(G set --subject alice --server nosuchsrv --team 990001 --tier data 2>&1)
rc=$?
[ "$rc" != "0" ] || fail "a grant against an unregistered server SUCCEEDED: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='nosuchsrv'")" = "0" ] \
  || fail "a grant was created against an unregistered server"
printf '%s' "$out" | grep -qi "created\|unchanged" \
  && fail "a refused grant printed a success outcome: $out"
echo "  rc=$rc, nothing written, no success prose"

step "the /v1 grant routes are UDS-only: a TCP caller cannot reach them"
# The transport gate matters more here than anywhere: server_http_conn_caps gives a
# remote_writes=full TCP bearer CAPS_ALL, so without the UDS check a remote bearer would hold
# grant-admin. Asserted against the running server, not against a unit stub.
for i in $(seq 1 20); do
  curl -s -o /dev/null "http://127.0.0.1:$SRV_TCP_PORT/v1/health" 2>/dev/null && break
  sleep 1
done
# A VALID bearer on the most permissive remote_writes setting. If this were merely an auth
# failure the test would prove nothing about the transport gate, so the sanity check below
# confirms the same credential DOES work on a non-grant route.
sanity=$(curl -s -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $AIMEE_SERVER_API_BEARER" \
  "http://127.0.0.1:$SRV_TCP_PORT/v1/health" 2>/dev/null)
[ "$sanity" = "200" ] \
  || fail "the TCP listener is not usable with its bearer (health returned $sanity), so the \
UDS assertion below would prove nothing"
for route in set revoke; do
  code=$(curl -s -o "$work/tcp.$route" -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $AIMEE_SERVER_API_BEARER" -H "Content-Type: application/json" \
    -d '{"subject":"mallory","server_id":"livesrv","team_id":990001,"tier":"full"}' \
    "http://127.0.0.1:$SRV_TCP_PORT/v1/grants/write-tier/$route" 2>/dev/null)
  [ "$code" = "200" ] && fail "a TCP caller reached /v1/grants/write-tier/$route (HTTP 200)"
  echo "  POST $route over TCP -> HTTP $code"
done
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='mallory'")" = "0" ] \
  || fail "a TCP caller CREATED a grant"
echo "  health=200 with the same bearer, so the refusal is the TRANSPORT gate, not auth"

step "the audit trail reconstructs the sequence"
psqlt -c "SELECT action||' '||actor_principal||' '||subject FROM kb_audit_event
            WHERE action LIKE 'authz.write_tier%' ORDER BY seq" | sed 's/^/  /'
n=$(rows "SELECT count(*) FROM kb_audit_event WHERE action LIKE 'authz.write_tier%'")
[ "$n" -ge 7 ] || fail "expected at least 7 audit rows, found $n"

step "kb logged no schema or permission failure"
grep -iE 'must be owner of|permission denied|schema apply failed' "$kb_log" \
  && fail "kb logged an ownership or permission failure on the hardened tier"
echo "  clean"

step "PASSED"
cat <<'MSG'
  The grant path ran on the HARDENED shape, not the dev shape: the schema was applied out of
  band by aimee_kb_migrate over verify-full TLS; kb connected as aimee_kb_runtime, which owns
  nothing, has no CREATE on public and cannot bypass RLS; the connection was confirmed
  encrypted from pg_stat_ssl rather than from the DSN string; and the SECURITY DEFINER grant
  functions were confirmed to be owned by someone other than the connecting role.
  The operator-visible prose was asserted too, including both safety warnings.
MSG
