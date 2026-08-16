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
  restore_owner_role
  runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
  rm -rf -- "$work"
}

# aimee_kb_owner is CLUSTER-global, not per-database, so this rig must hand it back exactly as
# it found it. An earlier version reset the password to NULL unconditionally, which is harmless
# on a scratch box and destroys a live credential anywhere else. rolpassword is the SCRAM
# verifier, and ALTER ROLE ... PASSWORD accepts an already-hashed string verbatim, so the
# original can be put back without ever knowing the cleartext.
snapshot_owner_role() {
  owner_existed=$(runuser -u postgres -- psql -tAX -c \
    "SELECT count(*) FROM pg_authid WHERE rolname='aimee_kb_owner'" 2>/dev/null | tr -d ' ')
  owner_pw_before=$(runuser -u postgres -- psql -tAX -c \
    "SELECT coalesce(rolpassword,'') FROM pg_authid WHERE rolname='aimee_kb_owner'" 2>/dev/null)
  owner_login_before=$(runuser -u postgres -- psql -tAX -c \
    "SELECT rolcanlogin FROM pg_authid WHERE rolname='aimee_kb_owner'" 2>/dev/null | tr -d ' ')
}

restore_owner_role() {
  [ "${owner_existed:-0}" = "0" ] && return 0
  if [ -n "${owner_pw_before:-}" ]; then
    runuser -u postgres -- psql -q -c \
      "ALTER ROLE aimee_kb_owner PASSWORD '$owner_pw_before'" >/dev/null 2>&1
  else
    runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner PASSWORD NULL" >/dev/null 2>&1
  fi
  # schema_roles.sql declares the role NOLOGIN and this rig grants it LOGIN to connect as it,
  # so the restore has to work in BOTH directions -- only ever adding NOLOGIN would leave a role
  # that could log in before the run unable to afterwards.
  case "${owner_login_before:-}" in
    t) runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner LOGIN" >/dev/null 2>&1 ;;
    f) runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner NOLOGIN" >/dev/null 2>&1 ;;
  esac
  return 0
}
trap cleanup EXIT

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; echo "--- kb tail:"; tail -20 "$kb_log" 2>/dev/null; echo "--- server tail:"; tail -20 "$srv_log" 2>/dev/null; exit 1; }
psqlq() { runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" "$@"; }
psqlt() { runuser -u postgres -- psql -tAX -d "$db" "$@"; }

step "Provisioning $db (roles -> schema -> grants)"
snapshot_owner_role
runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
# The roles live at cluster scope, so they are created in the maintenance database first —
# createdb -O below needs the owner role to already exist.
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -f src/modules/db2/c/schema_roles.sql >/dev/null 2>&1
# OWNED BY aimee_kb_owner. kb re-applies the schema at boot as that role, and it cannot
# redefine objects owned by postgres ("must be owner of function pg_now_text") — so whoever
# pre-applies has to be the same role kb will connect as. A real deployment's migrate step is
# that role too.
runuser -u postgres -- createdb -O aimee_kb_owner "$db" 2>/dev/null \
  || runuser -u postgres -- createdb "$db" || fail "createdb"
psqlq -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;' \
  || fail "extensions"
psqlq -f src/modules/db2/c/schema_roles.sql >/dev/null 2>&1
# PostgreSQL 15+ stopped granting CREATE on schema public to non-owners, so the owner role
# cannot apply the schema without this. A real deployment's migrate step holds the same
# privilege; schema_roles.sql does not grant it because it does not know the database name.
psqlq -c 'GRANT USAGE, CREATE ON SCHEMA public TO aimee_kb_owner' >/dev/null 2>&1
# THE SCHEMA IS NOT PRE-APPLIED. kb creates it at boot as the role it connects with, so there
# is no second party to conflict over object ownership — the failures on the way here were all
# of that kind ("must be owner of function ..."). A hardened deployment splits migrate from
# runtime and needs sslmode=verify-full for it; this rig is the single-node dev shape, which is
# enough to exercise the grant path.

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
export AIMEE_KB_API_BEARER_TOKEN="live-grant-token"
KB_PORT=18741
# The port comes from --http-port, not from the environment and not from a flat config key.
# AIMEE_KB_PORT is silently ignored, and kb_api_http_port is parsed inside a config SECTION, so a
# top-level key in aimee.yaml is ignored too — with no port kb starts every subsystem and then
# binds NOTHING, which is what two failed attempts at this rig looked like. The flag is
# unambiguous.
# kb.api.bearer_token is what kb VALIDATES against, and it is a NESTED key — a flat
# `kb_api_bearer_token` is ignored, as is any environment variable.
# AIMEE_KB_API_BEARER_TOKEN is only what a CLIENT sends.
#
# Without a configured token kb runs "auth off", and kb_http.c then manufactures NO owner actor
# on purpose: "the tenancy mutation routes require a real authenticated principal, so an
# auth-off deployment cannot make anonymous admin writes". So every grant call 401s. That is the
# correct behaviour and it is why this rig must configure a token.
cat > "$AIMEE_HOME/aimee.yaml" <<YAML
embedding_dim: 1024
kb:
  api:
    bearer_token: $AIMEE_KB_API_BEARER_TOKEN
YAML
kbpw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')
psqlq -c "ALTER ROLE aimee_kb_owner LOGIN PASSWORD '$kbpw'" >/dev/null 2>&1 \
  || fail "could not give aimee_kb_owner a password"
export AIMEE_DB2_URL="postgres://aimee_kb_owner:$kbpw@127.0.0.1:5432/$db"
export AIMEE_KB_API_BEARER_TOKEN="live-grant-token"
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
echo "aimee-kb healthy on $KB_PORT"

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

# ASSERTIONS ARE ABOUT THE DATABASE AND THE AUDIT LOG, not about wording. The thin client
# renders the server's JSON generically — the prose an earlier draft of these commands printed
# was lost when the local command was replaced by /v1 routing, which is recorded as a known gap
# — so grepping for sentences would test a formatter that does not exist. What matters is
# whether the right row and the right audit event ended up in Postgres.
jq_has() { printf '%s' "$1" | grep -q "$2"; }

step "list on an empty table returns an empty set, not an error"
out=$(G list --server livesrv --team 990001) || fail "list failed: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant")" = "0" ] || fail "the table was not empty"
echo "  ok (output: ${out:-<empty array>})"

step "set creates the grant AS THE AUTHENTICATED OPERATOR"
out=$(G set --subject alice --server livesrv --team 990001 --tier data) || fail "set: $out"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice'")" = "data" ] \
  || fail "the grant row does not say data (cli said: $out)"
# THE GRANTER IS THE ACTOR kb AUTHENTICATED, not a value from the request body. This is the
# defect this whole rig existed to find: with no tenant scope the definer saw no actor and
# refused every call, so increment 5 was wired end to end and could not create a grant.
[ "$(rows "SELECT granted_by FROM kb_write_tier_grant WHERE subject='alice'")" = "owner" ] \
  || fail "granted_by is not the authenticated operator"
[ "$(rows "SELECT count(*) FROM kb_audit_event WHERE action='authz.write_tier.set'")" = "1" ] \
  || fail "no WORM audit row for the set"
[ "$(rows "SELECT actor_principal FROM kb_audit_event WHERE action='authz.write_tier.set' LIMIT 1")" = "owner" ] \
  || fail "the audit row does not name the operator as actor"
echo "  row=data granted_by=owner, audited with actor=owner"

# FROM HERE ON, ASSERTIONS ARE DATABASE-ONLY, BY CHOICE.
#
# They used to be database-only by necessity: the response fields — changed, previous_tier,
# was_revoked, is_member, found — were not observable at all, because the local command that
# formatted them was removed when these commands moved onto /v1 and no per-method formatter
# replaced it. That gap is now closed, so the prose IS assertable, and it is asserted — in
# run-grant-cli-hardened-live.sh (live, including both safety warnings) and in
# test_cli_v1_delegate.c (per field, per stream).
#
# This rig stays on the database deliberately rather than duplicating that: what an operator
# ultimately depends on is the row and the audit event, and asserting those here keeps this
# rig meaningful even if the wording changes.

step "set again with the same tier leaves the row alone"
G set --subject alice --server livesrv --team 990001 --tier data >/dev/null || fail "set2"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice'")" = "data" ] \
  || fail "an idempotent re-grant changed the tier"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "an idempotent re-grant duplicated the row"
echo "  still one row at data"

step "set with a different tier changes it"
G set --subject alice --server livesrv --team 990001 --tier full >/dev/null || fail "set3"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice'")" = "full" ] \
  || fail "the tier did not change to full"
echo "  data -> full in the row"

step "a NON-member grant is still created (inert, not refused)"
G set --subject bob --server livesrv --team 990001 --tier data >/dev/null || fail "set4"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='bob'")" = "1" ] \
  || fail "the non-member grant was not created"
[ "$(rows "SELECT count(*) FROM kb_team_membership WHERE identity_key='bob'")" = "0" ] \
  || fail "bob was unexpectedly a member, so this proves nothing"
echo "  created for a non-member, as designed"

step "show WITHOUT --subject sends nothing (the round-7 defect)"
out=$(G show --server livesrv --team 990001)
printf '%s' "$out" | grep -q -- '--subject S is required' \
  || fail "show with no subject was not refused: $out"
printf '%s' "$out" | grep -q bob && fail "show with no subject LISTED EVERYTHING: $out"
echo "  refused before any request"

step "revoke retains the row, sets revoked_at, and audits as the operator"
G revoke --subject alice --server livesrv --team 990001 >/dev/null || fail "revoke"
[ "$(rows "SELECT revoked_at IS NOT NULL FROM kb_write_tier_grant WHERE subject='alice'")" = "t" ] \
  || fail "revoked_at was not set"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "revocation did not retain exactly one row"
[ "$(rows "SELECT actor_principal FROM kb_audit_event WHERE action='authz.write_tier.revoke' LIMIT 1")" = "owner" ] \
  || fail "the revoke audit row does not name the operator"
echo "  retained, revoked_at set, audited as owner"

step "revoking twice is idempotent and leaves one row"
G revoke --subject alice --server livesrv --team 990001 >/dev/null || fail "revoke2"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "a second revoke changed the row count"
echo "  still one row"

step "set after revoke clears revoked_at IN PLACE"
G set --subject alice --server livesrv --team 990001 --tier data >/dev/null || fail "set5"
[ "$(rows "SELECT revoked_at IS NULL FROM kb_write_tier_grant WHERE subject='alice'")" = "t" ] \
  || fail "revoked_at was not cleared"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='alice'")" = "1" ] \
  || fail "re-granting created a second row"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='alice'")" = "data" ] \
  || fail "the re-granted tier is wrong"
echo "  cleared in place, one row, tier data"

step "an UNREGISTERED server writes nothing"
G set --subject alice --server nosuchsrv --team 990001 --tier data >/dev/null 2>&1
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='nosuchsrv'")" = "0" ] \
  || fail "a grant was created against an unregistered server"
echo "  nothing written"

step "the audit trail reconstructs the sequence"
psqlt -c "SELECT action||' '||actor_principal||' '||subject FROM kb_audit_event
            WHERE action LIKE 'authz.write_tier%' ORDER BY seq" | sed 's/^/  /'
n=$(rows "SELECT count(*) FROM kb_audit_event WHERE action LIKE 'authz.write_tier%'")
[ "$n" -ge 7 ] || fail "expected at least 7 audit rows, found $n"

step "PASSED"
cat <<'MSG'
  The composed path ran with nothing stubbed: the shipping CLI over a unix socket, to a real
  aimee-server, to a real aimee-kb, to real Postgres — and the assertions were about the grant
  table and the audit log, not about what a mock was told.
MSG
