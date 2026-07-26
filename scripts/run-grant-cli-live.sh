#!/bin/bash
# run-grant-cli-live.sh — `aimee kb grant …` against a REAL server, a REAL kb and a REAL
# Postgres. The composed path, end to end, with nothing stubbed.
#
# WHY THIS EXISTS. Increment 5 was built as six layers and each was verified on its own:
# SQL against live Postgres, the C seam in the RLS gate, kb's routes and the server's routes
# and the client under unit tests, and a composed test that stubs kb_client at the bottom.
# A review then found a defect none of that could see — `grant show` with no --subject issued
# an unfiltered listing, because two layers disagreed about a command's identity — and made
# the point that layer-local verification had not established command-level correctness.
#
# This closes that for real: the CLI talks to a server over its unix socket, the server talks
# to kb over HTTP, kb talks to Postgres, and the assertions are about what ends up in the
# grant table and what the operator is told.
#
# MUST RUN AS ROOT on a host with Postgres.
# Usage: run-grant-cli-live.sh [--keep]
set -uo pipefail
export LC_ALL=C

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
keep=0
[ "${1:-}" = "--keep" ] && keep=1

[ "$(id -u)" = "0" ] || { echo "run-grant-cli-live: must run as root" >&2; exit 2; }
for b in ./aimee ./aimee-server ./aimee-kb; do
  [ -x "$b" ] || { echo "run-grant-cli-live: $b not built (make -C src all)" >&2; exit 2; }
done

db=aimee_grant_cli_live
work=$(mktemp -d /root/grant-live.XXXXXX)
export AIMEE_HOME="$work/home"
mkdir -p "$AIMEE_HOME"
kb_log=$work/kb.log
srv_log=$work/server.log
kb_pid=""; srv_pid=""

cleanup() {
  [ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null
  [ -n "$kb_pid" ] && kill "$kb_pid" 2>/dev/null
  sleep 1
  [ -n "$srv_pid" ] && kill -9 "$srv_pid" 2>/dev/null
  [ -n "$kb_pid" ] && kill -9 "$kb_pid" 2>/dev/null
  if [ "$keep" = "1" ]; then
    echo "run-grant-cli-live: keeping db=$db work=$work"
    return
  fi
  runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner PASSWORD NULL" >/dev/null 2>&1
  runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
  rm -rf -- "$work"
}
trap cleanup EXIT

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; echo "--- kb tail:"; tail -20 "$kb_log" 2>/dev/null; echo "--- server tail:"; tail -20 "$srv_log" 2>/dev/null; exit 1; }
psqlq() { runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" "$@"; }
psqlt() { runuser -u postgres -- psql -tAX -d "$db" "$@"; }

step "Provisioning $db (roles -> schema -> grants)"
runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
# The roles live at cluster scope, so they are created in the maintenance database first —
# createdb -O below needs the owner role to already exist.
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -f src/db2/schema_roles.sql >/dev/null 2>&1
# OWNED BY aimee_kb_owner. kb re-applies the schema at boot as that role, and it cannot
# redefine objects owned by postgres ("must be owner of function pg_now_text") — so whoever
# pre-applies has to be the same role kb will connect as. A real deployment's migrate step is
# that role too.
runuser -u postgres -- createdb -O aimee_kb_owner "$db" 2>/dev/null \
  || runuser -u postgres -- createdb "$db" || fail "createdb"
psqlq -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;' \
  || fail "extensions"
psqlq -f src/db2/schema_roles.sql >/dev/null 2>&1
# PostgreSQL 15+ stopped granting CREATE on schema public to non-owners, so the owner role
# cannot apply the schema without this. A real deployment's migrate step holds the same
# privilege; schema_roles.sql does not grant it because it does not know the database name.
psqlq -c 'GRANT USAGE, CREATE ON SCHEMA public TO aimee_kb_owner' >/dev/null 2>&1
{ echo "SET ROLE aimee_kb_owner;"; sed 's/__EMBED_DIM__/1024/g' src/db2/schema.sql; } \
  | psqlq -f - >/dev/null 2>&1 || fail "schema apply"
psqlq -f src/db2/schema_grants.sql >/dev/null 2>&1

step "Tenancy fixture: a team, a registered server, and owner as admin"
# What a real deployment builds by enrolling. `owner` is the local operator identity §7 makes
# the root of trust, and kb_admin_grant is what kb_principal_is_admin() reads.
psqlq >/dev/null <<SQL || fail "fixture"
INSERT INTO kb_team(id,name) VALUES (990001,'grant_cli_live');
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

step "Starting aimee-kb against that database"
# A TCP DSN, not a socket-relative one: kb runs as root here, so peer auth would
# authenticate as root.
kbpw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')
# A TCP DSN, because kb runs as root here and a socket-relative one would authenticate as
# root. The OWNER role, because kb applies the schema itself on the dev path and the runtime
# role deliberately has no CREATE on public.
#
# embedding_dim is PINNED in the config. Without a pin, db2_init prefers the dim RECORDED in
# the database and treats a read failure as fatal ("reading recorded embedding dim failed") —
# pinning skips that read outright. The hardened tier would also skip it, but requires
# sslmode=verify-full, i.e. full TLS to Postgres, which is more rig than this test needs.
#
# Four wrong turns preceded this, all mine, and all resolved by reading db2_init.c instead of
# guessing a fifth time: a socket DSN, the runtime role without hardening, the owner role
# without a pin, and the hardened tier without TLS.
cat > "$AIMEE_HOME/aimee.yaml" <<YAML
embedding_dim: 1024
YAML
kbpw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')
psqlq -c "ALTER ROLE aimee_kb_owner LOGIN PASSWORD '$kbpw'" >/dev/null 2>&1 \
  || fail "could not give aimee_kb_owner a password"
export AIMEE_DB2_URL="postgres://aimee_kb_owner:$kbpw@127.0.0.1:5432/$db"
export AIMEE_KB_API_BEARER_TOKEN="live-grant-token"
KB_PORT=18741
AIMEE_KB_PORT=$KB_PORT ./aimee-kb >"$kb_log" 2>&1 &
kb_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
    "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 && break
  kill -0 "$kb_pid" 2>/dev/null || fail "aimee-kb exited"
  sleep 1
done
curl -sf -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
  "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 || fail "aimee-kb never became healthy"
echo "aimee-kb healthy on $KB_PORT"

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
echo "aimee-server reachable over its unix socket"

# Every assertion below runs the SHIPPING CLI. No stubs anywhere in the path.
G() { ./aimee kb grant "$@" 2>&1; }
rows() { psqlt -c "$1"; }

step "list on an empty table"
out=$(G list --server livesrv --team 990001) || fail "list failed: $out"
printf '%s\n' "$out" | grep -q 'no grants' || fail "expected an empty listing, got: $out"
echo "  $out"

step "set creates the grant, and the DATABASE shows it"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set: $out"
printf '%s\n' "$out" | grep -q 'granted: alice' || fail "expected 'granted', got: $out"
echo "  $out"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice' AND team_id=990001")" = "data" ] \
  || fail "the grant row does not say data"
# The WORM audit row the definer writes, which is where history lives.
[ "$(rows "SELECT count(*) FROM kb_audit_event WHERE action='authz.write_tier.set'")" = "1" ] \
  || fail "no audit row for the set"

step "set again with the same tier is an idempotent no-op"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set2: $out"
printf '%s\n' "$out" | grep -q 'unchanged' || fail "expected 'unchanged', got: $out"
echo "  $out"

step "set with a different tier reports the PREVIOUS one"
out=$(G set --subject alice --server livesrv --team 990001 --tier full) || fail "set3: $out"
printf '%s\n' "$out" | grep -q 'data -> full' || fail "expected 'data -> full', got: $out"
echo "  $out"

step "a grant for a NON-member is created and warns that it is inert"
out=$(G set --subject bob --server livesrv --team 990001 --tier data) || fail "set4: $out"
printf '%s\n' "$out" | grep -qi 'not currently a member' || fail "expected the inert warning: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='bob'")" = "1" ] \
  || fail "the non-member grant was not created"
echo "  warned, and the row exists"

step "show reports ONE subject"
out=$(G show --subject alice --server livesrv --team 990001) || fail "show: $out"
printf '%s\n' "$out" | grep -q alice || fail "show omitted its subject: $out"
printf '%s\n' "$out" | grep -q bob && fail "show leaked another subject: $out"
echo "  show returned only alice"

step "show WITHOUT --subject sends nothing (the defect a review found)"
out=$(G show --server livesrv --team 990001)
printf '%s\n' "$out" | grep -q -- '--subject S is required' \
  || fail "show with no subject was not refused: $out"
printf '%s\n' "$out" | grep -q bob && fail "show with no subject LISTED EVERYTHING: $out"
echo "  refused, and listed nothing"

step "revoke reports found, and the row is retained with revoked_at"
out=$(G revoke --subject alice --server livesrv --team 990001) || fail "revoke: $out"
printf '%s\n' "$out" | grep -q 'revoked: alice' || fail "expected 'revoked', got: $out"
printf '%s\n' "$out" | grep -q '300s' || fail "revoke did not warn about an already-minted token"
[ "$(rows "SELECT revoked_at IS NOT NULL FROM kb_write_tier_grant WHERE subject='alice'")" = "t" ] \
  || fail "revoked_at was not set"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "revocation did not retain exactly one row"
echo "  $(printf '%s' "$out" | head -1)"

step "list hides the revoked grant; --include-revoked widens"
out=$(G list --server livesrv --team 990001) || fail "list2: $out"
printf '%s\n' "$out" | grep -q alice && fail "a revoked grant appeared in the default listing"
out=$(G list --server livesrv --team 990001 --include-revoked) || fail "list3: $out"
printf '%s\n' "$out" | grep -q 'alice.*revoked' || fail "--include-revoked did not show it: $out"
echo "  default hides it, --include-revoked shows it as revoked"

step "revoking a subject that never had a grant says so"
out=$(G revoke --subject nosuchuser --server livesrv --team 990001) || fail "revoke2: $out"
printf '%s\n' "$out" | grep -q 'no grant found' || fail "expected 'no grant found', got: $out"
echo "  $(printf '%s' "$out" | head -1)"

step "revoking twice is idempotent"
out=$(G revoke --subject bob --server livesrv --team 990001) || fail "revoke3: $out"
out=$(G revoke --subject bob --server livesrv --team 990001) || fail "revoke4: $out"
echo "  both calls succeeded"

step "set after revoke clears it IN PLACE and says so"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set5: $out"
printf '%s\n' "$out" | grep -qi 'revocation was cleared' \
  || fail "re-granting a revoked subject did not report it: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "re-granting created a second row"
[ "$(rows "SELECT revoked_at IS NULL FROM kb_write_tier_grant WHERE subject='alice'")" = "t" ] \
  || fail "revoked_at was not cleared"
echo "  cleared in place, still exactly one row"

step "an UNREGISTERED server is refused by the foreign key, through every layer"
out=$(G set --subject alice --server nosuchsrv --team 990001 --tier data)
printf '%s\n' "$out" | grep -qE 'refused|failed' || fail "an unregistered server was accepted: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='nosuchsrv'")" = "0" ] \
  || fail "a grant was created against an unregistered server"
echo "  refused, and nothing was written"

step "the audit trail reconstructs the whole sequence"
psqlt -c "SELECT action||' '||target FROM kb_audit_event
            WHERE action LIKE 'authz.write_tier%' ORDER BY id" | sed 's/^/  /'
n=$(rows "SELECT count(*) FROM kb_audit_event WHERE action LIKE 'authz.write_tier%'")
[ "$n" -ge 8 ] || fail "expected at least 8 audit rows, found $n"

step "PASSED"
cat <<'MSG'
  The composed path ran with nothing stubbed: the shipping CLI over a unix socket, to a real
  aimee-server, to a real aimee-kb, to real Postgres — and the assertions were about the grant
  table and the audit log, not about what a mock was told.
MSG
