# aimee-live-env.sh — a STANDARD live test environment. Source it; do not run it.
#
# WHY THIS EXISTS. Live rigs on this branch each grew their own copy of the
# same boot sequence, and each rediscovered the same traps: the DSN must be TCP
# because kb runs as root and peer auth would say root; embedding_dim must be
# PINNED; the kb port comes ONLY from --http-port (AIMEE_KB_PORT and a flat
# kb_api_http_port are both silently ignored); the bearer must be the NESTED
# kb.api.bearer_token; PG15+ needs an explicit GRANT CREATE ON SCHEMA public; and
# aimee-kb forks aimee-kb-resolver from beside its own executable. Copying that
# eight-step recipe per rig is how a rig ends up testing its own setup bug.
#
# It also standardises the two ways in to Postgres. A host with a local cluster
# (CT 301) has a `postgres` OS account and peer auth; a CI runner's cluster is a
# container with neither, so it needs a superuser URL. A rig that hard-codes
# either one cannot run in the other place.
#
# Usage:
#   . "$(dirname "$0")/lib/aimee-live-env.sh"
#   live_env_init "myrig" "$@"      # parses --keep and a postgres:// URL
#   live_env_pg_create              # disposable database + owner role
#   live_env_seed_identity_fixture  # team, membership, registry, instance, JWKS gen
#   live_env_start_kb               # aimee-kb, healthy
#   live_env_start_server           # aimee-server with a TCP listener, healthy
#   ... assertions, using pass/fail ...
#   live_env_verdict "what was proven"
#
# live_env_init installs an EXIT trap that stops both daemons and drops the
# database and role. Rigs must not install their own EXIT trap; rig-specific
# teardown goes in LIVE_EXTRA_CLEANUP.
#
# WHO USES IT, and who deliberately does not. run-authz-residual-live.sh,
# run-pam-login-live.sh and run-write-tier-enforce-live.sh are on it.
#
# run-grant-cli-live.sh is NOT, and should not be: it provisions the REAL
# cluster-scope roles from src/modules/db2/c/schema_roles.sql and connects as the real
# aimee_kb_owner, because whoever pre-applies the schema has to be the role kb
# will connect as. That provisioning IS part of what the rig tests, so putting it
# behind this helper's disposable-database path would quietly reduce its coverage
# to prove a tidier call site. Shared setup is worth having; shared setup that
# replaces the thing under test is not.

# --- assertions -------------------------------------------------------------
LIVE_FAILED=0
pass() { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; LIVE_FAILED=$((LIVE_FAILED + 1)); }
step() { echo; echo "== $*"; }
is2xx() { [ "$1" -ge 200 ] && [ "$1" -lt 300 ]; }

live_env_verdict() {
   step "Result"
   if [ "$LIVE_FAILED" = "0" ]; then
      echo "== PASSED — $*"
      echo "LIVE_EXIT=0"
      exit 0
   fi
   echo "== FAILED — $LIVE_FAILED assertion(s)"
   [ -n "${LIVE_KB_LOG:-}" ] && { echo "kb log:"; tail -25 "$LIVE_KB_LOG" 2>/dev/null; }
   [ -n "${LIVE_SRV_LOG:-}" ] && { echo "server log:"; tail -25 "$LIVE_SRV_LOG" 2>/dev/null; }
   echo "LIVE_EXIT=1"
   exit 1
}

# --- init -------------------------------------------------------------------
live_env_init() {
   LIVE_NAME=$1
   shift
   LIVE_KEEP=0
   LIVE_PG_URL=""
   for a in "$@"; do
      case "$a" in
      --keep) LIVE_KEEP=1 ;;
      postgres://* | postgresql://*) LIVE_PG_URL="$a" ;;
      *)
         echo "$LIVE_NAME: unknown argument '$a'" >&2
         exit 2
         ;;
      esac
   done

   [ "$(id -u)" = "0" ] || {
      echo "$LIVE_NAME: must run as root" >&2
      exit 2
   }
   LIVE_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
   cd "$LIVE_REPO" || exit 2
   for b in ./aimee ./aimee-server ./aimee-kb; do
      [ -x "$b" ] || {
         echo "$LIVE_NAME: $b not built (make -C src all)" >&2
         exit 2
      }
   done
   # kb FORKS this from beside its own executable. Missing, it fails much later
   # and as something else entirely -- which is how oidc-login-live passed
   # locally and failed on its first real CI runner.
   [ -x ./aimee-kb-resolver ] || {
      echo "$LIVE_NAME: ./aimee-kb-resolver not built" >&2
      exit 2
   }

   LIVE_WORK=$(mktemp -d "/tmp/aimee-${LIVE_NAME}-XXXXXX")
   export AIMEE_HOME="$LIVE_WORK/home"
   mkdir -p "$AIMEE_HOME"
   LIVE_KB_LOG="$LIVE_WORK/kb.log"
   # aimee-server does NOT write its log to stdout/stderr: it writes
   # $AIMEE_HOME/server.log, and the shell redirect below captures an empty file.
   # A rig that greps the redirect target sees nothing and concludes a log line is
   # missing when it is simply somewhere else -- which is exactly the false defect
   # this variable exists to prevent.
   LIVE_SRV_STDIO="$LIVE_WORK/server.stdio"
   LIVE_SRV_LOG="$AIMEE_HOME/server.log"
   LIVE_DB="aimee_${LIVE_NAME//-/_}_$$"
   # NOT a disposable name. The management tables carry
   #   CREATE POLICY ... USING (current_user='aimee_kb_owner')
   # under FORCE ROW LEVEL SECURITY, so a SECURITY DEFINER function owned by any
   # other role sees ZERO rows and refuses with "local management authority
   # denied" -- a fixture that looks perfect in a superuser SELECT and is
   # invisible to the code under test. The role name is part of the schema's
   # contract, so a live rig has to borrow the real one and hand it back.
   #
   # CONSEQUENCE, worth stating because it is not obvious and it bites hard: the
   # name is FIXED, so two helper-based rigs must not run against ONE cluster at
   # the same time. Both would snapshot the role as absent, both would treat it as
   # theirs, and the first to finish would drop the owner out from under the other's
   # database. CI is safe because each live job gets its own Postgres service, and
   # CT 301 runs them one at a time. Postgres refuses to drop a role that still owns
   # objects, so the damage is bounded and live_env_release_owner reports it -- but
   # do not rely on that as a design.
   LIVE_OWNER="aimee_kb_owner"
   LIVE_PW="lv$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
   LIVE_KB_PORT=${LIVE_KB_PORT:-18901}
   LIVE_SRV_PORT=${LIVE_SRV_PORT:-18903}
   LIVE_KB_BEARER="kb-$LIVE_NAME-token"
   LIVE_SRV_BEARER="srv-$LIVE_NAME-token"
   LIVE_TEAM=${LIVE_TEAM:-990001}
   LIVE_SERVER_ID=${LIVE_SERVER_ID:-livesrv}
   LIVE_TOKEN_KID=${LIVE_TOKEN_KID:-p5-token-v1-live}

   if [ -n "$LIVE_PG_URL" ]; then
      LIVE_PG_ADMIN="${LIVE_PG_URL%/*}/postgres"
      LIVE_PG_HOST=$(printf '%s' "$LIVE_PG_URL" | sed -E 's#.*@([^:/]+).*#\1#')
      LIVE_PG_PORT=$(printf '%s' "$LIVE_PG_URL" | sed -nE 's#.*:([0-9]+)/.*#\1#p')
      LIVE_PG_PORT=${LIVE_PG_PORT:-5432}
   else
      LIVE_PG_ADMIN=""
      LIVE_PG_HOST=127.0.0.1
      LIVE_PG_PORT=5432
   fi
   trap live_env_cleanup EXIT
}

pg_admin() {
   if [ -n "$LIVE_PG_ADMIN" ]; then
      psql -v ON_ERROR_STOP=1 -q "$LIVE_PG_ADMIN" -c "$1"
   else
      su postgres -c "psql -v ON_ERROR_STOP=1 -q -c \"$1\""
   fi
}
pg_admin_val() {
   if [ -n "$LIVE_PG_ADMIN" ]; then
      psql -tAX "$LIVE_PG_ADMIN" -c "$1" 2>/dev/null | tr -d ' '
   else
      su postgres -c "psql -tAX -c \"$1\"" 2>/dev/null | tr -d ' '
   fi
}
pg_db() {
   if [ -n "$LIVE_PG_ADMIN" ]; then
      psql -v ON_ERROR_STOP=1 -q "${LIVE_PG_URL%/*}/$LIVE_DB" "$@"
   else
      su postgres -c "psql -v ON_ERROR_STOP=1 -q -d $LIVE_DB $(printf '%q ' "$@")"
   fi
}
# Single-value query, for assertions that read the database back.
pg_val() {
   if [ -n "$LIVE_PG_ADMIN" ]; then
      psql -tAX "${LIVE_PG_URL%/*}/$LIVE_DB" -c "$1"
   else
      su postgres -c "psql -tAX -d $LIVE_DB -c \"$1\""
   fi
}

# Run SQL with the SAME session scope the routes establish.
#
# kb_write_tier_grant_set/_revoke and kb_management_identity_intent_start are all
# SECURITY DEFINER and read the acting identity from aimee.principal, which only a
# tenant scope sets (db2_tenant_scope_begin). Calling them from a bare psql session
# leaves the definer with no actor, and it correctly refuses -- which reads as "the
# feature is broken" rather than "the test forgot the scope". This is the same
# defect that cost this branch an evening at the route layer.
#
# Returns the LAST non-empty output line, so BEGIN/COMMIT chatter does not become
# the value an assertion compares against.
pg_scoped() { # pg_scoped <principal> <sql>
   local principal=$1 sql=$2
   local out
   out=$(pg_val "BEGIN; SET LOCAL aimee.principal='$principal'; SET LOCAL aimee.team='$LIVE_TEAM'; \
           $sql; COMMIT;" 2>&1)
   # An ERROR line is the answer whenever there is one. Taking the last line
   # instead returns psql's trailing CONTEXT: frame, which names the function and
   # line number but not what went wrong -- so an assertion reports a stack
   # location where it meant to report a reason.
   local err
   err=$(printf '%s\n' "$out" | grep -m1 '^ERROR:')
   if [ -n "$err" ]; then
      printf '%s\n' "$err"
      return 1
   fi
   printf '%s\n' "$out" | grep -vE '^(BEGIN|COMMIT|SET)$' | grep -v '^$' | tail -1
}

# aimee_kb_owner is CLUSTER-global, so a rig that borrows it must hand it back
# exactly as it found it. rolpassword is the SCRAM verifier and ALTER ROLE ...
# PASSWORD accepts an already-hashed string verbatim, so the original goes back
# without the cleartext ever being known. Restoring LOGIN/NOLOGIN has to work in
# BOTH directions: only ever adding NOLOGIN would leave a role that could log in
# before the run unable to afterwards.
live_env_snapshot_owner() {
   LIVE_OWNER_EXISTED=$(pg_admin_val "SELECT count(*) FROM pg_authid WHERE rolname='$LIVE_OWNER'")
   LIVE_OWNER_PW=$(pg_admin_val "SELECT coalesce(rolpassword,'') FROM pg_authid WHERE rolname='$LIVE_OWNER'")
   LIVE_OWNER_LOGIN=$(pg_admin_val "SELECT rolcanlogin FROM pg_authid WHERE rolname='$LIVE_OWNER'")
}
# Hands the owner role back. Two different obligations, and conflating them
# leaked a role: a role the rig CREATED must be DROPPED (leaving it behind means
# a cluster-global LOGIN role with this file's known test password sitting on the
# host forever), while a role it BORROWED must be restored and never dropped.
# Only safe once the database it owns is gone, so cleanup calls it after the DROP
# DATABASE — with --keep the database survives, so the created role has to as
# well and cleanup says so instead.
live_env_release_owner() {
   if [ "${LIVE_OWNER_EXISTED:-0}" = "0" ]; then
      local drop_err
      drop_err=$(pg_admin "DROP ROLE IF EXISTS $LIVE_OWNER" 2>&1)
      local still
      still=$(pg_admin_val "SELECT count(*) FROM pg_authid WHERE rolname='$LIVE_OWNER'")
      if [ "$still" != "0" ]; then
         # FAIL, do not merely report. The whole point of dropping it is that a
         # cluster-global role with this file's known password must not outlive the
         # run; a rig that detects the leak and still exits 0 has converted a
         # credential leak into a log line nobody reads.
         echo "$LIVE_NAME: could not drop the role this rig created ($LIVE_OWNER)." >&2
         echo "  It has this rig's known password and can log in. Drop it by hand:" >&2
         echo "    psql -c \"DROP ROLE $LIVE_OWNER\"" >&2
         [ -n "$drop_err" ] && echo "  psql said: $(printf '%s' "$drop_err" | head -1)" >&2
         LIVE_OWNER_LEAKED=1
      fi
      return 0
   fi
   live_env_restore_owner
}

live_env_restore_owner() {
   [ "${LIVE_OWNER_EXISTED:-0}" = "0" ] && return 0
   if [ -n "${LIVE_OWNER_PW:-}" ]; then
      pg_admin "ALTER ROLE $LIVE_OWNER PASSWORD '$LIVE_OWNER_PW'" >/dev/null 2>&1
   else
      pg_admin "ALTER ROLE $LIVE_OWNER PASSWORD NULL" >/dev/null 2>&1
   fi
   case "${LIVE_OWNER_LOGIN:-}" in
   t) pg_admin "ALTER ROLE $LIVE_OWNER LOGIN" >/dev/null 2>&1 ;;
   f) pg_admin "ALTER ROLE $LIVE_OWNER NOLOGIN" >/dev/null 2>&1 ;;
   esac
   return 0
}

live_env_pg_create() {
   step "Provisioning a disposable database"
   live_env_snapshot_owner
   if [ "${LIVE_OWNER_EXISTED:-0}" = "0" ]; then
      pg_admin "CREATE ROLE $LIVE_OWNER LOGIN PASSWORD '$LIVE_PW'" >/dev/null 2>&1
   else
      # Borrowed, not created: give it a password we know and LOGIN for the run.
      pg_admin "ALTER ROLE $LIVE_OWNER LOGIN PASSWORD '$LIVE_PW'" >/dev/null 2>&1
   fi
   pg_admin_val "SELECT 1 FROM pg_authid WHERE rolname='$LIVE_OWNER'" >/dev/null 2>&1 ||
      {
         echo "$LIVE_NAME: could not prepare the owner role" >&2
         exit 2
      }
   pg_admin "CREATE DATABASE $LIVE_DB OWNER $LIVE_OWNER" >/dev/null 2>&1 ||
      {
         echo "$LIVE_NAME: could not create the database" >&2
         exit 2
      }
   # PG15+: the database owner still needs CREATE on public explicitly.
   pg_db -c "GRANT CREATE ON SCHEMA public TO $LIVE_OWNER" >/dev/null 2>&1
   pg_db -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null 2>&1
   echo "database $LIVE_DB on $LIVE_PG_HOST:$LIVE_PG_PORT"
}

# --- the identity fixture ---------------------------------------------------
# Everything kb_management_identity_login_context() and
# kb_management_identity_intent_start() read, so a login can reach the point of
# filing a mint intent. Deliberately NOT the vault: filing an intent needs these
# ROWS, and only the signing step needs custodied keys. Standing this up is what
# lets a rig test grant honouring and revocation without the KMS chain.
live_env_seed_identity_fixture() {
   step "Seeding the identity fixture (team, registry, instance, publication)"
   local now
   now=$(date +%s)
   # `cmd <<SQL || { ... }` puts the closing brace INSIDE the heredoc body; use an
   # if-block so the error handling is outside it.
   # `cmd <<SQL || { ... }` would put the closing brace INSIDE the heredoc body,
   # so error handling goes in an if-block around it.
   #
   # The team/enrollment/registry rows are taken VERBATIM from
   # run-grant-cli-live.sh, which is known to work. Re-deriving them from the base
   # CREATE TABLE statements produces the wrong columns: several (authority_id,
   # cert_issuer, mgmt_fingerprint, ...) are added by later idempotent ALTERs and
   # do not appear in the original definition. Copying a proven fixture beats
   # rediscovering that.
   if ! pg_db 2>&1 <<SQL >"$LIVE_WORK/fixture.log"
INSERT INTO kb_team(id,name) VALUES ($LIVE_TEAM,'$LIVE_NAME') ON CONFLICT DO NOTHING;
INSERT INTO kb_team_membership(identity_key,team)
  VALUES ('alice',$LIVE_TEAM),('bob',$LIVE_TEAM),('owner',$LIVE_TEAM) ON CONFLICT DO NOTHING;
INSERT INTO kb_admin_grant(identity_key,granted_by) VALUES ('owner','owner') ON CONFLICT DO NOTHING;
INSERT INTO kb_enrollments(id,scope,fingerprint,serial,state,expires_at,authority_id,
                           cert_issuer,cert_serial_norm)
  VALUES (990101,'p5-server-management',repeat('c',64),'01','active',now()+interval '90 days',
          repeat('d',32),'CN=ca','01')
  ON CONFLICT DO NOTHING;
-- The LOCAL management enrollment. intent_start looks this up by
-- kb_management_instance.current_enrollment_id and requires scope
-- 'p5-kb-management' AND authority_id equal to the instance's authority_id --
-- distinct from the 'p5-server-management' enrollment above, which is matched
-- against the TARGET server's registry certificate instead. One enrollment
-- cannot satisfy both: they differ in scope and in what they are checked against.
INSERT INTO kb_enrollments(id,scope,fingerprint,serial,state,expires_at,authority_id,
                           cert_issuer,cert_serial_norm)
  VALUES (990102,'p5-kb-management',repeat('e',64),'02','active',now()+interval '90 days',
          repeat('7',32),'CN=ca','02')
  ON CONFLICT DO NOTHING;
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
                               mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
  VALUES ('$LIVE_SERVER_ID','cn','mcn',$LIVE_TEAM,'https://$LIVE_SERVER_ID','active',
          'CN=ca','01',repeat('c',64))
  ON CONFLICT DO NOTHING;

-- Beyond what the grant rigs need: the management instance and JWKS publication
-- that kb_management_identity_login_context() and _intent_start() read. These are
-- ROWS, not custodied keys -- which is what lets a rig reach the mint's grant
-- check without the vault/KMS chain.
INSERT INTO kb_management_instance_grant(installation_id,replacement_lineage_id,team_id,
    workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
    expected_ca_issuer,expected_ca_fingerprint,creator_identity,state,consumed_at)
  -- A FIRST install, not a replacement: the table requires
  -- replacement_lineage_id = installation_id when replaces_installation_id is NULL,
  -- and state='consumed' requires consumed_at.
  VALUES (repeat('1',32),repeat('1',32),$LIVE_TEAM,'issuer','subject',
          repeat('3',64),repeat('4',64),repeat('5',64),'CN=ca',repeat('6',64),
          'owner','consumed',now())
  ON CONFLICT DO NOTHING;

-- EXACTLY ONE active instance for the team, or the login context refuses: two
-- would make the mint's later instance checks arbitrary.
INSERT INTO kb_management_instance(installation_id,replacement_lineage_id,authority_id,
    team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
    expected_ca_issuer,expected_ca_fingerprint,current_generation,current_enrollment_id,state)
  VALUES (repeat('1',32),repeat('1',32),repeat('7',32),$LIVE_TEAM,'issuer','subject',
          repeat('3',64),repeat('4',64),repeat('5',64),'CN=ca',repeat('6',64),
          1,990102,'active')
  ON CONFLICT DO NOTHING;

-- The publication tables are WORM: a BEFORE-trigger refuses any write without a
-- matching row in kb_management_jwks_publication_permit for THIS backend pid and
-- transaction. That is not being bypassed here -- the permit is the documented
-- write path the real publisher uses, and both inserts below map to the
-- 'finalize' operation, so one permit in one transaction covers them. Seeding
-- these tables any other way would mean disabling an integrity control, which
-- would make the fixture prove less than the product enforces.
BEGIN;
INSERT INTO kb_management_jwks_publication_permit(backend_pid,transaction_id,operation)
  VALUES (pg_backend_pid(),txid_current(),'finalize') ON CONFLICT DO NOTHING;

-- The publication must be current AND inside its validity window; a kid from an
-- expired publication would be recorded and then refused at mint time.
INSERT INTO kb_management_jwks_publication_generation(generation,candidate_id,valid_from,
    valid_until,previous_manifest_sha256,jwks_bytes,payload_bytes,envelope_bytes,
    envelope_sha256,manifest_sha256,jwks_sha256,payload_sha256,signature,manifest_wire_id,
    manifest_public_digest,token_wire_id,token_public_digest,token_jwk_digest,
    publication_identity_digest,hwm1_attestation_digest,hwm2_attestation_digest,
    seal_epoch,finalized_at)
  VALUES (1,repeat('c',64),$now-3600,$now+86400,sha256('prev'),'{"keys":[]}','payload',
          'envelope',sha256('envelope'),sha256('manifest'),sha256('{"keys":[]}'),
          sha256('payload'),decode(repeat('ab',64),'hex'),'manifest-v1',
          sha256('mpub'),'$LIVE_TOKEN_KID',sha256('tpub'),sha256('tjwk'),
          sha256('pubident'),sha256('hwm1'),sha256('hwm2'),$now,now())
  ON CONFLICT DO NOTHING;

INSERT INTO kb_management_jwks_publication_registry(singleton,current_generation,candidate_id,
    manifest_sha256,envelope_sha256,hwm2_attestation_digest,finalized_at)
  VALUES (1,1,repeat('c',64),sha256('manifest'),sha256('envelope'),sha256('hwm'),now())
  ON CONFLICT DO NOTHING;
COMMIT;
SQL
   then
      echo "$LIVE_NAME: identity fixture failed" >&2
      grep -iE 'error|detail' "$LIVE_WORK/fixture.log" | head -5 >&2
      exit 2
   fi
   echo "team $LIVE_TEAM, server $LIVE_SERVER_ID, one active instance, publication current"
}

# --- daemons ----------------------------------------------------------------
# Creates a real host account and returns via LIVE_PAM_USER / LIVE_PAM_PASS.
# A rig that needs to prove what a SUCCESSFUL login does cannot do it with a
# wrong password: the response to a rejected credential is a different response.
# Removed by cleanup — a rig that leaves login accounts behind on a host is the
# same class of mistake as leaving a database role behind.
live_env_add_host_account() {
   LIVE_PAM_USER="${1:-liveuser_$$}"
   LIVE_PAM_PASS="${2:-Live-Pass-$$-xyz}"
   userdel -r "$LIVE_PAM_USER" >/dev/null 2>&1
   useradd -M -s /usr/sbin/nologin "$LIVE_PAM_USER" 2>/dev/null || {
      echo "$LIVE_NAME: could not create the host account $LIVE_PAM_USER" >&2
      exit 2
   }
   printf '%s:%s\n' "$LIVE_PAM_USER" "$LIVE_PAM_PASS" | chpasswd || {
      echo "$LIVE_NAME: could not set a password for $LIVE_PAM_USER" >&2
      exit 2
   }
   LIVE_HOST_ACCOUNTS="${LIVE_HOST_ACCOUNTS:-} $LIVE_PAM_USER"
   echo "host account $LIVE_PAM_USER created"
}

live_env_remove_host_accounts() {
   local u
   for u in ${LIVE_HOST_ACCOUNTS:-}; do
      userdel -r "$u" >/dev/null 2>&1
   done
   LIVE_HOST_ACCOUNTS=""
}

live_env_write_config() {
   cat >"$AIMEE_HOME/aimee.yaml" <<YAML
embedding_dim: 1024
kb:
  api:
aimee:
  api:
    http_port: $LIVE_SRV_PORT
    remote_writes: ${LIVE_REMOTE_WRITES:-off}
YAML
}

live_env_start_kb() {
   step "Starting aimee-kb"
   live_env_write_config
   # EVERY OIDC variable must be unset. kb treats a configured OIDC profile as
   # mutually exclusive with PAM, so a stray AIMEE_KB_OIDC_* in the environment
   # makes the PAM route answer 409 and every PAM assertion in every rig passes
   # VACUOUSLY. This is in the shared helper rather than one rig because the
   # failure is silent and identical wherever it happens.
   local v
   for v in $(env | sed -nE 's/^(AIMEE_KB_OIDC[A-Z_]*)=.*/\1/p'); do unset "$v"; done
   # TCP, not the socket: kb runs as root here and peer auth would present root.
   export AIMEE_DB2_URL="postgres://$LIVE_OWNER:$LIVE_PW@$LIVE_PG_HOST:$LIVE_PG_PORT/$LIVE_DB"
   export AIMEE_KB_API_BEARER_TOKEN="$LIVE_KB_BEARER"
   live_env_start_kb_modules
   ./aimee-kb --http-port="$LIVE_KB_PORT" >"$LIVE_KB_LOG" 2>&1 &
   LIVE_KB_PID=$!
   local i
   for i in $(seq 1 60); do
      curl -sf -H "Authorization: Bearer $LIVE_KB_BEARER" \
         "http://127.0.0.1:$LIVE_KB_PORT/v1/health" >/dev/null 2>&1 && break
      kill -0 "$LIVE_KB_PID" 2>/dev/null || {
         echo "$LIVE_NAME: aimee-kb exited" >&2
         tail -20 "$LIVE_KB_LOG" >&2
         exit 2
      }
      sleep 1
   done
   curl -sf -H "Authorization: Bearer $LIVE_KB_BEARER" \
      "http://127.0.0.1:$LIVE_KB_PORT/v1/health" >/dev/null 2>&1 || {
      echo "$LIVE_NAME: aimee-kb never became healthy" >&2
      tail -20 "$LIVE_KB_LOG" >&2
      exit 2
   }
   echo "aimee-kb healthy on $LIVE_KB_PORT"
}

live_env_restart_kb() {
   live_env_stop_kb_modules
   kill "$LIVE_KB_PID" 2>/dev/null
   sleep 1
   kill -9 "$LIVE_KB_PID" 2>/dev/null
   wait "$LIVE_KB_PID" 2>/dev/null
   rm -f "$AIMEE_HOME/kb-module-bus.sock"
   live_env_start_kb_modules
   ./aimee-kb --http-port="$LIVE_KB_PORT" >>"$LIVE_KB_LOG" 2>&1 &
   LIVE_KB_PID=$!
   local i
   for i in $(seq 1 60); do
      curl -sf -H "Authorization: Bearer $LIVE_KB_BEARER" \
         "http://127.0.0.1:$LIVE_KB_PORT/v1/health" >/dev/null 2>&1 && return 0
      sleep 1
   done
   echo "$LIVE_NAME: aimee-kb did not come back" >&2
   exit 2
}

# The DB1 module. The daemon holds no store -- it reaches DB1 over the module
# bus -- so without this every family is unreachable and the rig measures a
# server that cannot answer: write-tier grants verify against DB1 (jti consume,
# jwks trust), so every grant read as INVALID and every authorized write got a
# 403 that had nothing to do with the policy under test.
#
# Armed BEFORE the daemon, waiting for the socket the daemon is about to create.
# The daemon does one-shot startup work that needs the store (the mTLS ramp among
# it), so a module that attaches afterwards is already too late.
live_env_prepare_modules() {
   local config_module="src/build/obj/aimee-module-config"
   local multicall="src/build/obj/aimee-module"
   [ -x "$config_module" ] || make -C src build/obj/aimee-module-config >/dev/null 2>&1 || true
   [ -x "$multicall" ] || make -C src build/obj/aimee-module >/dev/null 2>&1 || true
   [ -x "$config_module" ] && [ -x "$multicall" ] || {
      echo "$LIVE_NAME: could not build required Go module processes" >&2
      exit 2
   }
   [ -x src/build/obj/aimee-module-postgres ] ||
      cp "$multicall" src/build/obj/aimee-module-postgres
   local bundle="src/build/obj/module-bundle"
   [ -r "$bundle/grants/server/config.grant" ] ||
      python3 scripts/export_c_repositories.py --runtime-bundle "$bundle" >/dev/null 2>&1 || true
   [ -r "$bundle/grants/server/config.grant" ] &&
      [ -r "$bundle/grants/kb/config.grant" ] &&
      [ -r "$bundle/grants/kb/postgres.grant" ] || {
      echo "$LIVE_NAME: generated module grants are unavailable" >&2
      exit 2
   }
}

live_env_arm_module() { # executable bus-socket log-file pid-variable [env assignment ...]
   local executable=$1 socket=$2 log=$3 pid_var=$4
   shift 4
   (
      child=
      trap '[ -n "$child" ] && kill "$child" 2>/dev/null; [ -n "$child" ] && wait "$child" 2>/dev/null; exit 0' TERM INT
      i=0
      while [ "$i" -lt 600 ]; do
         if [ -S "$socket" ]; then
            env "$@" AIMEE_HOME="$AIMEE_HOME" "$executable" "$socket" &
            child=$!
            wait "$child"
            child=
         fi
         i=$((i + 1))
         sleep 0.1
      done
      echo "module: bus socket never appeared: $socket" >&2
   ) >>"$log" 2>&1 &
   eval "$pid_var=$!"
}

live_env_start_kb_modules() {
   live_env_stop_kb_modules
   live_env_prepare_modules
   local bus="$AIMEE_HOME/kb-module-bus.sock"
   local grants="$AIMEE_HOME/modules.d/kb"
   mkdir -p "$grants"
   sed "s|^executable=.*|executable=$PWD/src/build/obj/aimee-module-config|" \
      src/build/obj/module-bundle/grants/kb/config.grant >"$grants/config.grant"
   sed "s|^executable=.*|executable=$PWD/src/build/obj/aimee-module-postgres|" \
      src/build/obj/module-bundle/grants/kb/postgres.grant >"$grants/postgres.grant"
   live_env_arm_module "$PWD/src/build/obj/aimee-module-config" "$bus" \
      "$AIMEE_HOME/kb-config-module.log" LIVE_KB_CONFIG_PID \
      "AIMEE_MODULE_POLICY_DIR=$grants"
   live_env_arm_module "$PWD/src/build/obj/aimee-module-postgres" "$bus" \
      "$AIMEE_HOME/kb-postgres-module.log" LIVE_KB_POSTGRES_PID \
      "AIMEE_MODULE_POLICY_DIR=$grants" "AIMEE_DB2_URL=$AIMEE_DB2_URL"
}

live_env_stop_kb_modules() {
   local var pid
   for var in LIVE_KB_CONFIG_PID LIVE_KB_POSTGRES_PID; do
      eval "pid=\${$var:-}"
      [ -n "$pid" ] || continue
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      eval "$var="
   done
}

live_env_start_module() {
   live_env_stop_module
   live_env_prepare_modules
   # The store is the multicall binary under its own name; the grant pins the
   # resolved path, and the binary takes its identity from argv[0].
   local module="${LIVE_DB1_MODULE:-src/build/obj/aimee-module-aimee}"
   [ -x "$module" ] || cp src/build/obj/aimee-module "$module" 2>/dev/null || true
   [ -x "$module" ] || {
      echo "$LIVE_NAME: could not provide the store module at $module" >&2
      exit 2
   }
   local grant="src/build/obj/module-bundle/grants/server/aimee.grant"
   [ -r "$grant" ] || python3 scripts/export_c_repositories.py \
      --runtime-bundle src/build/obj/module-bundle >/dev/null 2>&1 || true
   [ -r "$grant" ] || {
      echo "$LIVE_NAME: no generated store grant at $grant" >&2
      exit 2
   }
   # The store does not reach PostgreSQL itself: it calls the postgres module's
   # SQL stage over the bus. Without that module, and without a grant for the
   # store's OUTBOUND principal, the store attaches and then finds no backend.
   local pgmodule="src/build/obj/aimee-module-postgres"
   [ -x "$pgmodule" ] || cp src/build/obj/aimee-module "$pgmodule" 2>/dev/null || true
   mkdir -p "$AIMEE_HOME/modules.d/server"
   sed "s|^executable=.*|executable=$PWD/$module|" "$grant" \
      >"$AIMEE_HOME/modules.d/server/aimee.grant"
   # 11265 is the postgres module's health stage, 11266 its SQL stage.
   cat >"$AIMEE_HOME/modules.d/server/aimee-postgres.grant" <<PGGRANT
version=1
principal_class=1
principal_ref=28
uid=self
executable=$PWD/$pgmodule
publish=
subscribe=
request=
serve=11265,11266
PGGRANT
   # A module's serve grant admits what it answers, not what it asks for.
   cat >"$AIMEE_HOME/modules.d/server/aimee-store-client.grant" <<CLIENTGRANT
version=1
principal_class=1
principal_ref=68
uid=self
executable=$PWD/$module
publish=
subscribe=
request=11266
serve=
CLIENTGRANT
   sed "s|^executable=.*|executable=$PWD/src/build/obj/aimee-module-config|" \
      src/build/obj/module-bundle/grants/server/config.grant \
      >"$AIMEE_HOME/modules.d/server/config.grant"
   local bus="$AIMEE_HOME/server-module-bus.sock"
   # Postgres first: the store checks for its backend as it comes up.
   live_env_arm_module "$PWD/$pgmodule" "$bus" "$AIMEE_HOME/pg-module.log" \
      LIVE_PG_MODULE_PID "AIMEE_MODULE_POLICY_DIR=$AIMEE_HOME/modules.d/server" \
      "AIMEE_STORE_URL=${AIMEE_STORE_URL:-}"
   live_env_arm_module "$PWD/$module" "$bus" "$AIMEE_HOME/db1-module.log" \
      LIVE_MODULE_PID "AIMEE_MODULE_POLICY_DIR=$AIMEE_HOME/modules.d/server" \
      "AIMEE_STORE_URL=${AIMEE_STORE_URL:-}"
   live_env_arm_module "$PWD/src/build/obj/aimee-module-config" "$bus" \
      "$AIMEE_HOME/server-config-module.log" LIVE_SERVER_CONFIG_PID \
      "AIMEE_MODULE_POLICY_DIR=$AIMEE_HOME/modules.d/server"
}

live_env_stop_module() {
   local var pid
   for var in LIVE_MODULE_PID LIVE_PG_MODULE_PID LIVE_SERVER_CONFIG_PID; do
      eval "pid=\${$var:-}"
      [ -n "$pid" ] || continue
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      eval "$var="
   done
}

live_env_start_server() {
   live_env_start_module
   step "Starting aimee-server with a TCP listener"
   export AIMEE_KB_API_URL="http://127.0.0.1:$LIVE_KB_PORT"
   # API credentials are Vault/runtime-secret capabilities, not configuration
   # values.  The extracted config module deliberately quarantines legacy
   # bearer_token YAML fields, so give the daemon its first-boot bearer through
   # the supported runtime-secret ingress.  The daemon seals and removes this
   # variable in its own process; the harness retains its copy for probes.
   export AIMEE_API_BEARER_TOKEN="$LIVE_SRV_BEARER"
   export AIMEE_SERVER_ID="$LIVE_SERVER_ID"
   export AIMEE_SERVER_TEAM_ID="$LIVE_TEAM"
   ./aimee-server >"$LIVE_SRV_STDIO" 2>&1 &
   LIVE_SRV_PID=$!
   local i
   for i in $(seq 1 60); do
      curl -sf -H "x-api-key: $LIVE_SRV_BEARER" \
         "http://127.0.0.1:$LIVE_SRV_PORT/v1/health" >/dev/null 2>&1 && break
      kill -0 "$LIVE_SRV_PID" 2>/dev/null || {
         echo "$LIVE_NAME: aimee-server exited" >&2
         tail -20 "$LIVE_SRV_LOG" >&2
         exit 2
      }
      sleep 1
   done
   # The loop above ends the same way whether it broke on success or ran out of
   # attempts with a live-but-wedged process, so it cannot be the proof. Without
   # this final probe the rig announced "healthy" against a server answering
   # nothing, and every later curl returned empty — which assertions looking for
   # the ABSENCE of a header (no Set-Cookie, no redirect, no CORS) would have
   # read as a pass. Health has to be demonstrated, not assumed.
   curl -sf -H "x-api-key: $LIVE_SRV_BEARER" \
      "http://127.0.0.1:$LIVE_SRV_PORT/v1/health" >/dev/null 2>&1 || {
      echo "$LIVE_NAME: aimee-server never became healthy" >&2
      tail -20 "$LIVE_SRV_LOG" >&2
      exit 2
   }
   echo "aimee-server TCP listener healthy on $LIVE_SRV_PORT"
}

live_env_restart_server() {
   # The module attaches to a socket the daemon owns, so a daemon restart takes
   # its attachment with it. Stop it, clear the stale socket, and arm a fresh one
   # against the socket the new daemon is about to create.
   live_env_stop_module
   kill "$LIVE_SRV_PID" 2>/dev/null
   sleep 1
   kill -9 "$LIVE_SRV_PID" 2>/dev/null
   wait "$LIVE_SRV_PID" 2>/dev/null
   rm -f "$AIMEE_HOME/server-module-bus.sock"
   live_env_start_module
   ./aimee-server >"${LIVE_SRV_STDIO}.$1" 2>&1 &
   LIVE_SRV_PID=$!
   local i
   for i in $(seq 1 60); do
      curl -sf -H "x-api-key: $LIVE_SRV_BEARER" \
         "http://127.0.0.1:$LIVE_SRV_PORT/v1/health" >/dev/null 2>&1 && return 0
      sleep 1
   done
   echo "$LIVE_NAME: aimee-server did not come back" >&2
   exit 2
}

live_env_cleanup() {
   live_env_stop_module
   live_env_stop_kb_modules
   [ -n "${LIVE_SRV_PID:-}" ] && kill "$LIVE_SRV_PID" 2>/dev/null
   [ -n "${LIVE_KB_PID:-}" ] && kill "$LIVE_KB_PID" 2>/dev/null
   sleep 1
   [ -n "${LIVE_SRV_PID:-}" ] && kill -9 "$LIVE_SRV_PID" 2>/dev/null
   [ -n "${LIVE_KB_PID:-}" ] && kill -9 "$LIVE_KB_PID" 2>/dev/null
   [ -n "${LIVE_SRV_PID:-}" ] && wait "$LIVE_SRV_PID" 2>/dev/null
   [ -n "${LIVE_KB_PID:-}" ] && wait "$LIVE_KB_PID" 2>/dev/null
   [ -n "${LIVE_EXTRA_CLEANUP:-}" ] && eval "$LIVE_EXTRA_CLEANUP"
   live_env_remove_host_accounts
   # --keep must keep the DATABASE too. Keeping only the work directory is useless
   # for the thing --keep is for -- looking at the rows a failing assertion saw.
   if [ "${LIVE_KEEP:-0}" = "1" ]; then
      echo "kept: $LIVE_WORK"
      echo "kept database: $LIVE_DB (owner $LIVE_OWNER); drop with:"
      echo "  psql -c 'DROP DATABASE $LIVE_DB'"
      # The kept database is still owned by the role, so a role this rig CREATED
      # cannot be dropped yet — say so, with the command, rather than leaving an
      # unannounced login role behind.
      if [ "${LIVE_OWNER_EXISTED:-0}" = "0" ]; then
         echo "kept role: $LIVE_OWNER (created by this rig, has its test password); after the drop:"
         echo "  psql -c 'DROP ROLE $LIVE_OWNER'"
      else
         # A BORROWED cluster-global role goes back even when the database stays.
         live_env_restore_owner
      fi
      return 0
   fi
   # A crashed daemon or module must not turn cleanup itself into a credential
   # leak. PostgreSQL 13+ can atomically terminate any remaining test sessions
   # while dropping this rig's uniquely named database.
   pg_admin "DROP DATABASE IF EXISTS $LIVE_DB WITH (FORCE)" >/dev/null 2>&1
   # After the database is gone: drop a role this rig created, restore one it
   # borrowed. A borrowed role is cluster-global and other databases may own
   # objects with it, so it is never dropped.
   live_env_release_owner
   rm -rf "$LIVE_WORK"
   # A leaked login role FAILS THE RUN, even when every assertion passed. This
   # runs in the EXIT trap, after live_env_verdict has already called exit, so
   # overriding the status here is the only way to make it stick -- and it must
   # stick, or "cleanup leaves no credential behind" is an aspiration rather than
   # a property CI enforces.
   if [ "${LIVE_OWNER_LEAKED:-0}" = "1" ]; then
      echo "== FAILED — the run left the cluster-global role $LIVE_OWNER behind"
      echo "LIVE_EXIT=1"
      exit 1
   fi
}
