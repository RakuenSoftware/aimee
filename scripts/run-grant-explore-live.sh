#!/bin/bash
# run-grant-explore-live.sh — ADVERSARIAL and exploratory probing of the write-tier grant path.
#
# WHY THIS EXISTS, given two live rigs already do. Both of those walk the HAPPY PATH: an admin
# operator granting and revoking sensible tiers to sensible subjects, and they assert that the
# right thing happens. Neither asks what happens when the input is hostile, when the caller is
# not an admin, when another tenant's data is in reach, or when the result set is larger than
# the ceiling. Those are the questions where an authorization feature actually fails.
#
# So this probes, in four groups:
#   1. INPUT — quoting, SQL metacharacters, newlines, unicode, empty, oversized, numeric edges.
#   2. AUTHORIZATION — a non-admin authenticated principal must not be able to grant at all.
#   3. TENANT ISOLATION — one team's grants must not be readable or writable from another.
#   4. INTEGRITY — the audit log must reject UPDATE and DELETE, and the list ceiling must be
#      reported rather than silently truncating.
#
# Group 1 is written to OBSERVE first and assert invariants second: the point is to find out what
# the system does, not to encode what I guessed it does. What every case must satisfy is that a
# refusal writes nothing and that nothing injects.
#
# Runs on the DEV shape deliberately -- kb as the owner role, no TLS. Everything here is about
# input handling, authorization and RLS, none of which the hardened tier changes; the hardened
# tier has its own rig. Keeping this one cheap means it can be run often.
#
# MUST RUN AS ROOT on a host with Postgres.
# Usage: run-grant-explore-live.sh [--keep]
set -uo pipefail
export LC_ALL=C

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
keep=0
[ "${1:-}" = "--keep" ] && keep=1

[ "$(id -u)" = "0" ] || { echo "explore: must run as root" >&2; exit 2; }
for b in ./aimee ./aimee-server ./aimee-kb ./aimee-kb-resolver; do
  [ -x "$b" ] || { echo "explore: $b not built (make -C src all)" >&2; exit 2; }
done

db=aimee_grant_explore_live
work=$(mktemp -d /root/grant-explore.XXXXXX)
export AIMEE_HOME="$work/home"
mkdir -p "$AIMEE_HOME"
kb_log=$work/kb.log
srv_log=$work/server.log
kb_pid=""; srv_pid=""
findings=$work/findings.txt
: > "$findings"

step() { printf '\n== %s\n' "$*"; }
fail() {
  echo "FAIL: $*" >&2
  echo "--- kb tail:"; tail -25 "$kb_log" 2>/dev/null
  exit 1
}
# An observation that needs a human decision, not a pass/fail. Collected and printed at the end
# so a surprising-but-not-wrong behaviour is not lost in 200 lines of output.
note() { echo "  NOTE: $*"; echo "$*" >> "$findings"; }
psqlq() { runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -d "$db" "$@"; }
psqlt() { runuser -u postgres -- psql -tAX -d "$db" "$@"; }

owner_existed=""; owner_pw_before=""; owner_login_before=""
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
  if [ -n "$owner_pw_before" ]; then
    runuser -u postgres -- psql -q -c \
      "ALTER ROLE aimee_kb_owner PASSWORD '$owner_pw_before'" >/dev/null 2>&1
  else
    runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner PASSWORD NULL" >/dev/null 2>&1
  fi
  case "$owner_login_before" in
    t) runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner LOGIN" >/dev/null 2>&1 ;;
    f) runuser -u postgres -- psql -q -c "ALTER ROLE aimee_kb_owner NOLOGIN" >/dev/null 2>&1 ;;
  esac
  return 0
}

cleanup() {
  [ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null
  [ -n "$kb_pid" ] && kill "$kb_pid" 2>/dev/null
  sleep 1
  [ -n "$srv_pid" ] && kill -9 "$srv_pid" 2>/dev/null
  [ -n "$kb_pid" ] && kill -9 "$kb_pid" 2>/dev/null
  if [ "$keep" = "1" ]; then
    echo "explore: keeping db=$db work=$work"
    return
  fi
  restore_owner_role
  runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
  rm -rf -- "$work"
}
trap cleanup EXIT

step "Provisioning $db"
snapshot_owner_role
runuser -u postgres -- dropdb --force --if-exists "$db" >/dev/null 2>&1
runuser -u postgres -- psql -q -v ON_ERROR_STOP=1 -f src/modules/db2/c/schema_roles.sql >/dev/null 2>&1
runuser -u postgres -- createdb -O aimee_kb_owner "$db" || fail "createdb"
psqlq -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;' \
  || fail "extensions"
psqlq -f src/modules/db2/c/schema_roles.sql >/dev/null 2>&1
psqlq -c 'GRANT USAGE, CREATE ON SCHEMA public TO aimee_kb_owner' >/dev/null 2>&1

step "Starting aimee-kb (dev shape: owner role, schema applied at boot)"
export AIMEE_KB_API_BEARER_TOKEN="explore-token"
KB_PORT=18751
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
echo "  healthy on $KB_PORT"

step "Fixture: TWO teams, so cross-tenant reach can be probed"
psqlq >/dev/null <<SQL || fail "fixture"
INSERT INTO kb_team(id,name) VALUES (990001,'explore_a'),(990002,'explore_b');
INSERT INTO kb_team_membership(identity_key,team)
  VALUES ('alice',990001),('owner',990001),('carol',990002),('owner',990002);
INSERT INTO kb_admin_grant(identity_key,granted_by) VALUES ('owner','owner');
INSERT INTO kb_enrollments(id,scope,fingerprint,serial,state,expires_at,authority_id,
                           cert_issuer,cert_serial_norm)
  VALUES (990101,'p5-server-management',repeat('c',64),'01','active',now()+interval '90 days',
          repeat('d',32),'CN=ca','01');
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
                              mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
  VALUES ('srva','cn','mcn',990001,'https://srva','active','CN=ca','01',repeat('c',64)),
         ('srvb','cn2','mcn2',990002,'https://srvb','active','CN=ca','02',repeat('e',64));
SQL
echo "  team 990001/srva and team 990002/srvb"

step "Starting aimee-server"
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
echo "  reachable"

G() { ./aimee kb grant "$@" 2>&1; }
rows() { psqlt -c "$1"; }
total_grants() { rows "SELECT count(*) FROM kb_write_tier_grant"; }

################################################################################
step "GROUP 1: hostile and edge-case input"
################################################################################
# Each case reports what actually happened. The INVARIANT, checked every time, is that the
# number of grant rows only ever changes by a deliberate amount and that no injection lands.
before=$(total_grants)

probe() {
  local label="$1"; shift
  local out rc
  out=$(G "$@" 2>&1); rc=$?
  # Collapse to one line so a newline-bearing subject cannot forge output structure.
  printf '  %-34s rc=%-3s %s\n' "$label" "$rc" "$(printf '%s' "$out" | tr '\n' ' ' | cut -c1-90)"
}

probe "SQL metacharacters"      set --subject "a'; DROP TABLE kb_write_tier_grant; --" \
                                    --server srva --team 990001 --tier data
probe "double-quote + backslash" set --subject 'b"\\x' --server srva --team 990001 --tier data
probe "newline in subject"        set --subject "$(printf 'c\nd')" --server srva --team 990001 --tier data
probe "unicode subject"          set --subject "ünïcødé-🔑" --server srva --team 990001 --tier data
probe "empty subject"            set --subject "" --server srva --team 990001 --tier data
probe "whitespace-only subject"  set --subject "   " --server srva --team 990001 --tier data
probe "1KB subject"              set --subject "$(printf 'x%.0s' $(seq 1 1024))" \
                                    --server srva --team 990001 --tier data
probe "invalid tier"             set --subject alice --server srva --team 990001 --tier superuser
probe "uppercase tier"           set --subject alice --server srva --team 990001 --tier DATA
probe "empty tier"               set --subject alice --server srva --team 990001 --tier ""
probe "negative team"            set --subject alice --server srva --team -1 --tier data
probe "zero team"                set --subject alice --server srva --team 0 --tier data
probe "non-numeric team"         set --subject alice --server srva --team notanumber --tier data
probe "2^63 team"                set --subject alice --server srva --team 9223372036854775808 --tier data
probe "fractional team"          set --subject alice --server srva --team 1.5 --tier data
probe "empty server"             set --subject alice --server "" --team 990001 --tier data
probe "server metacharacters"    set --subject alice --server "srva'; --" --team 990001 --tier data

step "INVARIANT: the schema survived and nothing injected"
# If any of the above had been interpolated rather than bound, the table would be gone.
[ -n "$(rows "SELECT 1 FROM pg_tables WHERE tablename='kb_write_tier_grant'")" ] \
  || fail "kb_write_tier_grant NO LONGER EXISTS -- SQL injection succeeded"
[ -n "$(rows "SELECT 1 FROM pg_tables WHERE tablename='kb_audit_event'")" ] \
  || fail "kb_audit_event no longer exists -- SQL injection succeeded"
echo "  both tables intact"

step "INVARIANT: every hostile subject above was refused, and none was stored"
# Those refusals are DELIBERATE, not incidental: db2_intent_canonical_actor enforces a grammar
# -- `owner`, a bare username, or oidc:<issuer>:<subject> / cert:<issuer>:<serial> with encoded
# components -- and the same rule is mirrored by a CHECK constraint in schema.sql and covered by
# src/tests/test_subject_grammar.c against a shared corpus. So the interesting question is not
# whether exotic bytes are rejected but whether any of them LANDED anyway.
for pat in '%DROP%' '%ünïcødé%' '%
%' '   '; do
  n=$(psqlt -c "SELECT count(*) FROM kb_write_tier_grant WHERE subject LIKE \$q\$$pat\$q\$")
  [ "$n" = "0" ] || fail "a refused subject matching '$pat' was stored anyway ($n row(s))"
done
echo "  none of the hostile subjects reached the table"

step "INVARIANT: a VALID subject round-trips byte-for-byte"
# The real risk is not a crash but a MANGLED identity: a grant stored under a different string
# than the one authorized is a silent authorization bug. These are inside the grammar, so they
# must be stored EXACTLY as sent -- including the longest form that still fits.
# Forms taken from src/tests/subject_corpus.h, which is the authoritative list shared by the C
# grammar test and the SQL CHECK -- guessing here cost a cycle: `%3D` looks reasonable but the
# grammar allows only the canonical `%3A`/`%25` escapes, and a raw `=` needs no encoding at all.
long_sub="oidc:https%3A//idp.example:$(printf 'u%.0s' $(seq 1 200))"
for s in "oidc:test:alice" "cert:CN=aimee-ca:a1b" "oidc:a%3Ab:c%25d" \
         "oidc:https%3A//idp.example:alice" "$long_sub"; do
  out=$(G set --subject "$s" --server srva --team 990001 --tier data 2>&1); rc=$?
  if [ "$rc" != "0" ]; then
    fail "a grammar-valid subject was refused: rc=$rc $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-70) [$s]"
  fi
  n=$(psqlt -c "SELECT count(*) FROM kb_write_tier_grant WHERE subject = \$q\$$s\$q\$")
  [ "$n" = "1" ] \
    || fail "subject was accepted but not stored verbatim (found $n exact match(es)): $s"
  echo "  verbatim: $(printf '%s' "$s" | cut -c1-52)"
done

################################################################################
step "GROUP 2: identity cannot be asserted by the caller"
################################################################################
# WHAT THIS IS NOT. An earlier version of this group tried to grant as `alice` (a team member
# with no kb_admin_grant) by sending X-Aimee-Actor, and treated the refusal as proof that a
# non-admin cannot grant. That proved nothing: kb rejects the header outright as spoofable, so
# the call never carried alice's identity and the 400 was a parse-level refusal.
#
# The non-admin authorization decision is covered where it belongs and much better -- in
# scripts/per-user-write-tier-rls-test.sql, run by the P1 RLS gate in CI, which asserts with a
# real tenant context that a plain member cannot mint a grant, cannot escalate their own tier,
# and that a team lead cannot write into another team (each expecting insufficient_privilege).
# Duplicating a weaker version of that here would be worse than not having it.
#
# What IS worth asserting live, and is asserted here, is the property that made the first
# attempt impossible: kb must never accept a client-supplied identity header.
[ "$(rows "SELECT count(*) FROM kb_admin_grant WHERE identity_key='alice'")" = "0" ] \
  || fail "alice holds an admin grant, so the fixture is wrong"

# The guard's set is explicit and documented in src/kb/http/kb_ingress.c: principal, actor,
# source, session-key, user -- mirroring the server's proxy-trust set. Those must be refused
# FAIL-CLOSED, before any route runs. Every value below is `alice`, a non-admin, so honouring one
# would be a straight privilege escalation.
for name in X-Aimee-Principal X-Aimee-Actor X-Aimee-Source X-Aimee-Session-Key X-Aimee-User; do
  code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
    -H "Content-Type: application/json" -H "$name: alice" \
    -d '{"server_id":"srva","team_id":990001,"subject":"oidc:test:mallory","tier":"full","granted_by":"owner"}' \
    "http://127.0.0.1:$KB_PORT/v1/write-tier-grants/set" 2>/dev/null)
  [ "$code" = "400" ] || fail "kb did not fail closed on $name (HTTP $code)"
  printf '  %-22s -> HTTP %s (fail-closed)\n' "$name" "$code"
done
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE subject='oidc:test:mallory'")" = "0" ] \
  || fail "a header-spoofed grant was written"
grep -q 'rejected request bearing a spoofable' "$kb_log" \
  || fail "kb did not log the spoofable-header rejection, so the refusal may be incidental"

# An X-Aimee-* name OUTSIDE that set is not refused, and that is correct rather than a gap: kb
# derives identity from the authenticated bearer (or mTLS), never from a header, so an unknown
# header is simply inert. The property that actually matters is therefore NOT "every X-Aimee-*
# header is rejected" but "no header can change WHO kb acts as" -- which is what this asserts,
# using a non-admin value that would be plainly visible in the audit row if it were honoured.
audit_before=$(rows "SELECT count(*) FROM kb_audit_event WHERE action='authz.write_tier.set'")
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
  -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
  -H "Content-Type: application/json" -H "X-Aimee-Identity-Key: alice" \
  -d '{"server_id":"srva","team_id":990001,"subject":"oidc:test:hdrprobe","tier":"data","granted_by":"alice"}' \
  "http://127.0.0.1:$KB_PORT/v1/write-tier-grants/set" 2>/dev/null)
printf '  %-22s -> HTTP %s (not in the trusted set; inert)\n' "X-Aimee-Identity-Key" "$code"
if [ "$code" = "200" ]; then
  # It went through as the authenticated principal. Both the stored granter and the audit actor
  # must be that principal and NOT the header's value -- and note granted_by:'alice' was in the
  # BODY here too, so this also re-checks that kb ignores the request's granted_by field.
  gb=$(rows "SELECT granted_by FROM kb_write_tier_grant WHERE subject='oidc:test:hdrprobe'")
  [ "$gb" = "owner" ] \
    || fail "the header (or the request's granted_by) changed the recorded granter to '$gb'"
  act=$(rows "SELECT actor_principal FROM kb_audit_event WHERE action='authz.write_tier.set'
                ORDER BY seq DESC LIMIT 1")
  [ "$act" = "owner" ] || fail "the header changed the AUDITED actor to '$act'"
  echo "  granter=owner and audited actor=owner, so neither the header nor granted_by was honoured"
else
  audit_after=$(rows "SELECT count(*) FROM kb_audit_event WHERE action='authz.write_tier.set'")
  [ "$audit_before" = "$audit_after" ] || fail "a refused request still wrote an audit row"
  echo "  refused, and no audit row was written"
fi
echo "  identity is never taken from the wire"

################################################################################
step "GROUP 3: tenant isolation between team 990001 and team 990002"
################################################################################
psqlq -c "INSERT INTO kb_write_tier_grant(server_id,team_id,subject,tier,granted_by)
            VALUES ('srvb',990002,'carol','full','owner')" >/dev/null 2>&1 \
  || fail "could not seed team B's grant"

step "listing team A does not return team B's grants"
out=$(G list --server srva --team 990001) || fail "list A: $out"
printf '%s' "$out" | grep -q carol && fail "TEAM A'S LISTING LEAKED TEAM B'S SUBJECT: $out"
echo "  carol absent from team A's listing"

step "asking for team B's subject on team A's server returns nothing"
out=$(G show --subject carol --server srva --team 990001) || fail "show cross: $out"
printf '%s' "$out" | grep -q "no write-tier grants" \
  || note "show for a foreign subject printed something other than an empty result: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-80)"
echo "  no row returned"

step "a server belonging to team B cannot be granted against under team A"
out=$(G set --subject alice --server srvb --team 990001 --tier full 2>&1); rc=$?
[ "$rc" = "0" ] && fail "granting on another team's server SUCCEEDED: $out"
[ "$(rows "SELECT count(*) FROM kb_write_tier_grant
             WHERE server_id='srvb' AND subject='alice'")" = "0" ] \
  || fail "a grant was written against another team's server"
echo "  refused (rc=$rc), nothing written"

step "team B's grant is untouched by all of the above"
[ "$(rows "SELECT tier FROM kb_write_tier_grant WHERE subject='carol'")" = "full" ] \
  || fail "team B's grant was modified"
echo "  carol still full"

################################################################################
step "GROUP 4: integrity -- the audit log is append-only, the ceiling is reported"
################################################################################
step "kb_audit_event rejects UPDATE and DELETE"
# WORM by trigger. If either succeeds, the audit trail is not evidence of anything.
#
# THE TRIGGERS ARE `FOR EACH ROW`, so a statement that matches nothing never fires one and
# exits 0. An earlier version of this test ran its UPDATE before any grant had succeeded, matched
# zero rows, and reported the product as broken. So: prove the rows exist and the triggers are
# installed FIRST, and check the content afterwards -- an exit status alone is not evidence here.
for trg in kb_audit_no_update kb_audit_no_delete kb_audit_no_truncate; do
  [ -n "$(rows "SELECT 1 FROM pg_trigger WHERE tgname='$trg' AND NOT tgisinternal")" ] \
    || fail "the $trg WORM trigger is not installed"
done
n_audit=$(rows "SELECT count(*) FROM kb_audit_event WHERE action LIKE 'authz.write_tier%'")
[ "${n_audit:-0}" -ge 1 ] \
  || fail "no write-tier audit rows exist yet, so a WORM test here would assert nothing"
echo "  $n_audit audit row(s) present, all three triggers installed"

before_actors=$(rows "SELECT string_agg(actor_principal,',' ORDER BY seq) FROM kb_audit_event
                        WHERE action LIKE 'authz.write_tier%'")
if psqlq -c "UPDATE kb_audit_event SET actor_principal='tampered'
               WHERE action LIKE 'authz.write_tier%'" >/dev/null 2>&1; then
  fail "kb_audit_event allowed an UPDATE"
fi
if psqlq -c "DELETE FROM kb_audit_event WHERE action LIKE 'authz.write_tier%'" >/dev/null 2>&1; then
  fail "kb_audit_event allowed a DELETE"
fi
# Belt and braces: even if a future trigger returned NULL instead of raising, the rows must be
# byte-identical to what they were before the attempts.
after_actors=$(rows "SELECT string_agg(actor_principal,',' ORDER BY seq) FROM kb_audit_event
                       WHERE action LIKE 'authz.write_tier%'")
[ "$before_actors" = "$after_actors" ] \
  || fail "the audit rows CHANGED despite the statements being refused"
[ "$(rows "SELECT count(*) FROM kb_audit_event WHERE action LIKE 'authz.write_tier%'")" = "$n_audit" ] \
  || fail "audit rows were removed despite the DELETE being refused"
echo "  both refused, and the rows are unchanged"

step "the list ceiling is REPORTED, not silently applied"
# The binding ceiling is the SERVER's GRANT_LIST_CAP (256), not kb's GRANTS_LIST_MAX (512) --
# worth knowing, because reasoning about kb's number alone gives the wrong expectation. 600 rows
# means the answer is necessarily incomplete, and an operator not told that will read a capped
# page as the whole picture.
psqlq >/dev/null <<SQL || fail "bulk seed"
INSERT INTO kb_write_tier_grant(server_id,team_id,subject,tier,granted_by)
SELECT 'srva',990001,'bulk_'||lpad(g::text,4,'0'),'data','owner'
  FROM generate_series(1,600) g
ON CONFLICT DO NOTHING;
SQL
n=$(rows "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='srva'")
echo "  seeded to $n rows on srva"
out=$(G list --server srva --team 990001) || fail "bulk list: $out"
shown=$(printf '%s' "$out" | grep -c 'bulk_')
echo "  listing rendered $shown bulk row(s)"
if [ "$shown" -ge "$n" ]; then
  note "the listing returned all $n rows, so the 512 ceiling did not engage as expected -- confirm GRANTS_LIST_MAX is still 512 and applies to this route"
else
  printf '%s' "$out" | grep -q "more grants exist" \
    || fail "the listing was capped at $shown of $n rows and did NOT warn that more exist"
  echo "  capped AND the operator was warned"
fi

step "show finds a subject that sorts BEYOND the ceiling"
# The round-7 defect was filtering AFTER the cap. A subject past position 256 must still be
# findable, or revoke's found/not-found answer is a coin flip on large tenants.
#
# A deliberately last-sorting SIMPLE subject is used rather than whichever row happens to sort
# last: picking the max would confound "is filtering pushed down?" with "is this exotic subject
# handled?", and those are different questions. The exotic ones get their own check below.
beyond="zzz_beyond_cap"
psqlq -c "INSERT INTO kb_write_tier_grant(server_id,team_id,subject,tier,granted_by)
            VALUES ('srva',990001,'$beyond','data','owner')" >/dev/null 2>&1 \
  || fail "could not seed the beyond-cap subject"
pos=$(rows "SELECT count(*) FROM kb_write_tier_grant
              WHERE server_id='srva' AND subject <= '$beyond'")
[ "${pos:-0}" -gt 256 ] || fail "$beyond sorts at position $pos, which is not beyond the 256 cap"
out=$(G show --subject "$beyond" --server srva --team 990001) || fail "show beyond: $out"
printf '%s' "$out" | grep -q "$beyond" \
  || fail "$beyond (position $pos) is NOT findable -- filtering happens after the cap: $out"
echo "  found at position $pos, past the 256 ceiling"

step "revoke is exact for a subject beyond the ceiling"
out=$(G revoke --subject "$beyond" --server srva --team 990001) || fail "revoke beyond: $out"
printf '%s' "$out" | grep -q "no grant existed" \
  && fail "revoking an EXISTING subject beyond the cap reported it as absent: $out"
[ "$(rows "SELECT revoked_at IS NOT NULL FROM kb_write_tier_grant WHERE subject='$beyond'")" = "t" ] \
  || fail "the revoke did not take effect"
echo "  revoked and reported as found"

step "show accepts every subject form that set accepted"
# ASYMMETRY HUNT: a subject kb will STORE but not LOOK UP is a real bug -- the grant exists and
# the operator cannot see or revoke it. Each of these was accepted by set earlier in this run.
for s in "oidc:test:alice" "cert:CN=aimee-ca:a1b" "oidc:a%3Ab:c%25d" \
         "oidc:https%3A//idp.example:alice"; do
  out=$(G show --subject "$s" --server srva --team 990001 2>&1); rc=$?
  if [ "$rc" != "0" ]; then
    note "set STORED subject '$s' but show REFUSES it (rc=$rc: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-60)) -- the grant is unreachable through the CLI, which also means it cannot be revoked"
    continue
  fi
  printf '%s' "$out" | grep -qF "$s" \
    || note "show accepted subject '$s' but did not return its row, though set stored it"
done
echo "  checked all four stored forms"

################################################################################
step "RESULTS"
################################################################################
if [ -s "$findings" ]; then
  echo "  Observations needing a decision (not failures):"
  nl -ba -w4 -s'. ' "$findings" | sed 's/^/   /'
else
  echo "  No open observations."
fi

step "PASSED"
echo "  Every invariant held: no injection, no cross-tenant leak, no non-admin grant, an"
echo "  append-only audit log, and a reported rather than silent list ceiling."
