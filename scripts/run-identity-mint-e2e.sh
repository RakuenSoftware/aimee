#!/bin/bash
# run-identity-mint-e2e.sh — drive the data-plane identity mint end to end, for
# real, against a live Postgres and a live signed-HWM KMS helper.
#
# WHY THIS EXISTS. Every layer of the per-user /v1 write feature is unit-tested
# and the SQL is covered by the P1 RLS gate, but identity_issue — the step that
# actually uses the vault-custodied signing key and returns a token — had never
# once been run. Its preconditions are the kind that cannot be faked: the
# reserved vault slots (org:p5-token, org:p5-jwks-manifest) refuse to be seeded
# through generic table paths at all ("bootstrap authority is not inferred from
# row shape"), so the only way to reach a mint is to run the real provisioners in
# the real order.
#
# MUST RUN AS ROOT, on a host with Postgres. Not a choice: the provisioners
# require AIMEE_VAULT_KMS_HELPER and AIMEE_VAULT_KMS_HWM_PUBKEY to be root-owned
# fixed files and then clearenv() before forking the helper, so an unprivileged
# run fails configuration before it does anything.
#
# The KMS setup mirrors scripts/run-p2b-egress-ct260.sh, which is the working
# recipe for the same helper. A local Ed25519 key stands in for a hardware
# signer; p2b_kms_helper.py produces the identical
# "aimee-hwm-v1|<key_id>|<version>|<domain>" attestation the CT 261 service does,
# so nothing here depends on that host being reachable.
#
# Usage: run-identity-mint-e2e.sh [--keep]
#   --keep  leave the database and work directory behind for inspection
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

keep=0
[ "${1:-}" = "--keep" ] && keep=1

if [ "$(id -u)" != "0" ]; then
  echo "run-identity-mint-e2e: must run as root (the provisioners require" >&2
  echo "  root-owned helper/pubkey files and clearenv() before forking)" >&2
  exit 2
fi

# The provisioners mlockall(MCL_CURRENT|MCL_FUTURE) so key material can never
# reach swap, which fails with ENOMEM whenever RLIMIT_MEMLOCK is below the
# process size. A stock LXC container caps it at 8MB, which is not enough for a
# libpq + OpenSSL binary. Check it here rather than letting the chain fail three
# steps in with a message about a syscall.
memlock=$(ulimit -l)
if [ "$memlock" != "unlimited" ] && [ "${memlock:-0}" -lt 65536 ]; then
  cat >&2 <<MSG
run-identity-mint-e2e: RLIMIT_MEMLOCK is ${memlock}KB; the provisioners need it
  raised (they mlockall so signing key material never swaps, and fail with
  ENOMEM below roughly their own size).

  Raise it for this shell if the host allows:      ulimit -l unlimited
  For an LXC container, on the Proxmox host add:   lxc.prlimit.memlock: unlimited
    to /etc/pve/lxc/<ctid>.conf and restart the container. A stock container caps
    this at 8MB and the limit cannot be raised from inside.
MSG
  exit 2
fi

db=aimee_identity_mint_e2e
prov_role=aimee_mint_provisioner
prov_pw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')
# The token authority connects as ITSELF, not through a member role: role_assert()
# in modules/db2/c/management_token_authority.c requires current_user to literally be
# aimee_kb_token_authority_runtime, with LOGIN, NOINHERIT, no superuser/bypassrls/
# createdb/createrole/replication, a search_path pinned to 'pg_catalog, pg_temp',
# row_security on, and no CREATE on public. schema_roles.sql already creates it
# with every one of those attributes — it just has no password, because a real
# deploy authenticates it by peer or certificate rather than a secret in a script.
auth_role=aimee_kb_token_authority_runtime
auth_pw=$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')

# HOW THIS REACHES POSTGRES AS A SUPERUSER. Two shapes, because the two places
# this has to run are genuinely different:
#
#   default   runuser -u postgres over the local peer socket. What a bare-metal or
#             LXC host gives you, and what every other live gate in scripts/ uses.
#   CI        AIMEE_TEST_PG_SUPERUSER_URL set. CI's Postgres is a pgvector
#             CONTAINER: there is no postgres OS account to runuser into and no
#             peer socket on the host, only TCP with a superuser password. The
#             extensions this schema needs (vector, pg_trgm) are why CI cannot
#             just use the runner's preinstalled Postgres instead.
#
# Only the SUPERUSER hop changes. The roles actually under test — the provisioner
# and aimee_kb_token_authority_runtime — authenticate as themselves over TCP in
# both shapes, with the same passwords and the same role_assert() checks, so the
# thing being gated is identical either way.
pg_host=127.0.0.1
pg_port=5432
maint_db=postgres
if [ -n "${AIMEE_TEST_PG_SUPERUSER_URL:-}" ]; then
  # Derived once into PG* rather than threaded through forty psql invocations as
  # flags. urllib does the parsing because a DSN with a percent-encoded password
  # is not something to take apart with sed.
  eval "$(python3 - "$AIMEE_TEST_PG_SUPERUSER_URL" <<'PARSE'
import sys, urllib.parse as u
p = u.urlparse(sys.argv[1])
def sh(v):
    return "'" + str(v).replace("'", "'\\''") + "'"
host = u.unquote(p.hostname or '127.0.0.1')
port = p.port or 5432
print('export PGHOST=%s' % sh(host))
print('export PGPORT=%s' % sh(port))
print('export PGUSER=%s' % sh(u.unquote(p.username or 'postgres')))
if p.password:
    print('export PGPASSWORD=%s' % sh(u.unquote(p.password)))
print('pg_host=%s' % sh(host))
print('pg_port=%s' % sh(port))
# The maintenance database must not be the one this rig drops and recreates.
print('maint_db=%s' % sh((p.path or '/postgres').lstrip('/') or 'postgres'))
PARSE
)"
  as_super() { "$@"; }
  echo "run-identity-mint-e2e: superuser via TCP $PGUSER@$pg_host:$pg_port (maintenance db $maint_db)"
else
  as_super() { runuser -u postgres -- "$@"; }
fi

# NOT under /tmp. root_owned_fixed_file() in the provisioners walks every parent
# directory up to / and rejects any that is group- or other-writable, which /tmp
# (mode 1777) is. That is an anti-substitution guard — it stops a non-root user
# swapping the helper or the pubkey under a path root will execute — so the work
# directory has to sit somewhere only root can write. run-p2b-egress-ct260.sh uses
# /tmp safely because it never invokes these binaries, only a unit test.
workroot=/var/lib/aimee-identity-mint
install -d -m 0700 -o root -g root "$workroot"
work=$(mktemp -d "$workroot/run.XXXXXX")
chmod 0700 "$work"
cleanup() {
  if [ "$keep" = "1" ]; then
    echo "run-identity-mint-e2e: keeping db=$db work=$work"
    echo "  authority dsn: postgres://$auth_role:$auth_pw@$pg_host:$pg_port/$db"
    echo "  intent: correlation=${corr:-none} jti=${jti:-none}"
    return
  fi
  as_super dropdb --maintenance-db="$maint_db" --force --if-exists "$db" >/dev/null 2>&1 || true
  as_super psql -q -d "$maint_db" -c "DROP ROLE IF EXISTS $prov_role" >/dev/null 2>&1 || true
  as_super psql -q -d "$maint_db" -c "ALTER ROLE $auth_role PASSWORD NULL" >/dev/null 2>&1 || true
  case "$work" in "$workroot"/run.*) rm -rf -- "$work" ;; esac
}
trap cleanup EXIT

step() { printf '\n== %s\n' "$*"; }

# The provisioners emit one fixed error class and a numeric exit code. Report both:
# the class alone is ambiguous (its switch falls through to "integrity" for any
# unmapped code), so a bare class can send you looking at the wrong gate.
provisioner_exit() {
  case "$1" in
    64) echo "usage" ;;         65) echo "configuration (env or a non-root-owned helper/pubkey path)" ;;
    66) echo "hardening" ;;     67) echo "database (DSN, or session_user not a member of aimee_kb_migrate)" ;;
    68) echo "custody (the KMS helper refused or could not be run)" ;;
    69) echo "conflict (already provisioned)" ;;
    70) echo "retry (serialization)" ;;
    71) echo "sealed (the vault is sealed)" ;;
    72) echo "integrity (a verification failed)" ;;
    73) echo "output" ;;
    *)  echo "UNMAPPED — note the class printed above falls through to 'integrity'" ;;
  esac
}
run_provisioner() {
  local label="$1"; shift
  set +e
  "$@"
  local rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    printf '%s: exit=%s -> %s\n' "$label" "$rc" "$(provisioner_exit "$rc")" >&2
    exit "$rc"
  fi
}
psqlq() { as_super psql -q -v ON_ERROR_STOP=1 -d "$db" "$@"; }
psqlt() { as_super psql -tAX -v ON_ERROR_STOP=1 -d "$db" "$@"; }

step "Provisioning $db (roles -> schema -> grants, the hardened deploy order)"
as_super dropdb --maintenance-db="$maint_db" --force --if-exists "$db"
as_super createdb --maintenance-db="$maint_db" "$db"
psqlq -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;'
psqlq -f src/modules/db2/c/schema_roles.sql
sed 's/__EMBED_DIM__/1024/g' src/modules/db2/c/schema.sql | psqlq -f -
psqlq -f src/modules/db2/c/schema_grants.sql

# The provisioners require pg_has_role(session_user,'aimee_kb_migrate','MEMBER')
# and then SET ROLE to their own NOINHERIT provisioning role. aimee_kb_migrate is
# itself NOLOGIN — DDL authority is deliberately not something you can log in as —
# so a real deploy runs these out of band under a login role that is a member of
# it. The schema creates no such role, on purpose: naming it is the operator's
# choice. This rig makes one.
# Idempotent: a previous --keep run leaves the role behind, and it is per-rig
# rather than per-database so dropdb does not remove it.
as_super psql -q -d "$maint_db" -c "DROP ROLE IF EXISTS $prov_role" >/dev/null 2>&1 || true
psqlq <<SQL
CREATE ROLE $prov_role LOGIN PASSWORD '$prov_pw';
GRANT aimee_kb_migrate TO $prov_role;

-- Give the authority role a password for this rig, and pin the search_path
-- role_assert() demands. Both are rig concessions: the attributes that matter
-- (NOINHERIT, no superuser, no CREATE on public) come from schema_roles.sql and
-- are NOT relaxed here.
ALTER ROLE $auth_role PASSWORD '$auth_pw';
ALTER ROLE $auth_role SET search_path = 'pg_catalog, pg_temp';
ALTER ROLE $auth_role SET row_security = on;
SQL

step "Signed-HWM KMS helper (local Ed25519; same attestation format as CT 261)"
openssl genpkey -algorithm Ed25519 -out "$work/hwm-private.pem" 2>/dev/null
openssl pkey -in "$work/hwm-private.pem" -pubout -outform DER | tail -c 32 >"$work/hwm-public.raw"
head -c 32 /dev/urandom >"$work/kek.raw"
# The protected-use path reaches the vault store and KEK cache under AIMEE_HOME.
# run-p2b-egress-ct260.sh sets this for the same reason; without it the store has
# nowhere to live and the signing step fails with a bare UNAVAILABLE.
export AIMEE_HOME="$work/aimee-home"
mkdir -p "$AIMEE_HOME"
chmod 0700 "$AIMEE_HOME"

export AIMEE_VAULT_KMS_HELPER="$work/kms-helper"
export AIMEE_VAULT_KMS_KEY_ID=identity-mint-e2e-kek
export AIMEE_VAULT_KMS_HWM_PUBKEY="$work/hwm-public.raw"
export AIMEE_VAULT_KMS_HWM_DOMAIN=identity-mint-e2e

install -m 0700 scripts/p2b_kms_helper.py "$work/p2b_kms_helper.py"

# The helper is invoked AFTER the provisioner clearenv()s down to the four
# AIMEE_VAULT_KMS_* variables, so it cannot receive its own key paths from the
# environment. That is the reason AIMEE_VAULT_KMS_HELPER must be a root-owned
# fixed FILE rather than a command line: a helper's configuration is meant to be
# baked into the executable a root-only path guarantees, not inherited. So wrap it.
# PER-KEY HWM STATE. The provisioner drives THREE custody keys (token, manifest,
# publication) and each needs its own monotonic counter: it reads the live version
# and expects 1 (fresh) or 2 (already provisioned), so a shared counter makes the
# second key see the first key's advance and fail verification.
#
# p2b_kms_helper.py keeps a single counter, which was fine for p2b because it uses
# one key. The CT 261 service keys its state by key_id precisely because this path
# needs that. Rather than depend on that host, the wrapper gives each key_id its
# own state and lock file — same guarantee, locally.
cat >"$work/kms-helper" <<WRAP
#!/bin/sh
set -eu
key=\$2
case "\$key" in *[!A-Za-z0-9._-]*) exit 2 ;; esac
AIMEE_P2B_KMS_PRIVATE_KEY=$work/hwm-private.pem
AIMEE_P2B_KMS_KEK=$work/kek.raw
AIMEE_P2B_KMS_HWM_STATE=$work/hwm.\$key.state
AIMEE_P2B_KMS_HWM_LOCK=$work/hwm.\$key.lock
if [ ! -f "\$AIMEE_P2B_KMS_HWM_STATE" ]; then
  printf '1\\n' >"\$AIMEE_P2B_KMS_HWM_STATE"
  chmod 0600 "\$AIMEE_P2B_KMS_HWM_STATE"
fi
if [ ! -f "\$AIMEE_P2B_KMS_HWM_LOCK" ]; then
  : >"\$AIMEE_P2B_KMS_HWM_LOCK"
  chmod 0600 "\$AIMEE_P2B_KMS_HWM_LOCK"
fi
export AIMEE_P2B_KMS_PRIVATE_KEY AIMEE_P2B_KMS_KEK AIMEE_P2B_KMS_HWM_STATE AIMEE_P2B_KMS_HWM_LOCK
exec $work/p2b_kms_helper.py "\$@"
WRAP
chmod 0600 "$work"/hwm-private.pem "$work"/hwm-public.raw "$work"/kek.raw
chmod 0700 "$work/kms-helper" "$work/p2b_kms_helper.py"

# Prove the helper answers before handing it to a provisioner: a silent helper
# failure surfaces only as a "custody" exit class, which says nothing about why.
if ! "$work/kms-helper" hwm-read "$AIMEE_VAULT_KMS_KEY_ID" >/dev/null; then
  echo "run-identity-mint-e2e: the KMS helper does not answer hwm-read" >&2
  exit 3
fi
echo "kms helper: answers hwm-read"


step "Token roots (establishes the reserved vault slots + the RS256 token root)"
export AIMEE_KB_TOKEN_ROOTS_PROVISION_DSN="postgres://$prov_role:$prov_pw@$pg_host:$pg_port/$db"
export AIMEE_KB_TOKEN_ROOT_CUSTODY_ID=identity-mint-token-root
export AIMEE_KB_JWKS_MANIFEST_ROOT_CUSTODY_ID=identity-mint-manifest-root
export AIMEE_KB_JWKS_PUBLICATION_HWM_CUSTODY_ID=identity-mint-publication-hwm
run_provisioner "token-roots" ./aimee-kb-token-roots-provision
echo "token roots: provisioned"
psqlt -c "SELECT 'token_root: kind='||root_kind||' v='||current_version||' wire='||wire_id||
                 ' enabled='||enabled FROM kb_management_token_root ORDER BY root_kind;"
psqlt -c "SELECT 'vault: principal='||principal||' v='||version FROM org_vault_current ORDER BY 1;"

step "JWKS publication (generation 1, the wire key the token's kid names)"
export AIMEE_KB_JWKS_PUBLISH_DSN="postgres://$prov_role:$prov_pw@$pg_host:$pg_port/$db"
# Unix seconds, not ISO-8601: canonical_time() accepts digits only, deliberately —
# there is no timezone or format to get wrong in an authenticated validity window.
# Under PUBLISH_MAX_LIFETIME (86400s). A publication window is a validity claim
# the mint checks `now` against, so it is deliberately short-lived; a 30-day
# window is refused outright rather than clamped.
# valid_from must be within PUBLISH_CLOCK_SKEW_SECONDS of now (fresh_time_valid) —
# a publication is minted for the present, not backdated — and the window must be
# under PUBLISH_MAX_LIFETIME. So: start now, end well inside a day.
from=$(date -u +%s)
until=$(( from + 36000 ))
run_provisioner "jwks-publish" ./aimee-kb-jwks-publish --valid-from "$from" --valid-until "$until"
echo "jwks: published $from -> $until"
kid=$(psqlt -c "SELECT token_wire_id FROM kb_management_jwks_publication_generation
                 WHERE generation=1;")
echo "wire kid: $kid"
psqlt -c "SELECT 'publication: gen='||generation||' phase='||phase||' epoch='||seal_epoch
            FROM kb_management_jwks_publication_candidate ORDER BY generation;"

step "Tenancy + cert-chain fixture (the parts a real deploy enrolls)"
# Everything below is state a real deployment builds by enrolling: a team, the
# subject's membership, the target server's registry row and its management
# enrollment, and a local management instance with its own enrollment. The mint
# checks all of it — scope, state, revoked_at, an expiry in the future, and that
# each id matches what the intent recorded — so it is spelled out rather than
# defaulted.
inst=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
anchor=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
authid=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
mgmt_fp=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
kb_fp=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
expiry=$(date -u -d '+90 days' '+%Y-%m-%d %H:%M:%S')

psqlq <<SQL
INSERT INTO kb_team(id, name) VALUES (770001, 'identity_mint_team');
INSERT INTO kb_team_membership(identity_key, team) VALUES ('alice', 770001);

-- The target server, and the p5-server-management enrollment the registry names.
INSERT INTO kb_enrollments(id, scope, fingerprint, serial, state, expires_at, authority_id,
                           cert_issuer, cert_serial_norm)
  VALUES (770101, 'p5-server-management', '$mgmt_fp', '01', 'active', '$expiry',
          '$authid', 'CN=aimee-mgmt-ca', '01'),
         (770102, 'p5-kb-management', '$kb_fp', '02', 'active', '$expiry',
          '$authid', 'CN=aimee-kb-ca', '02');

INSERT INTO kb_server_registry(server_id, cert_cn, mgmt_cert_cn, team_id, endpoint, status,
                              mgmt_issuer, mgmt_serial_norm, mgmt_fingerprint)
  VALUES ('mintsrv', 'mintsrv-cn', 'mintsrv-mgmt-cn', 770001, 'https://mintsrv', 'active',
          'CN=aimee-mgmt-ca', '01', '$mgmt_fp');

-- The local management instance. instance_grant is the FK parent; the instance
-- itself carries the generation and enrollment the intent must agree with.
INSERT INTO kb_management_instance_grant(installation_id, replacement_lineage_id, team_id,
    workload_issuer, workload_subject, proof_anchor, custody_anchor, binding_digest,
    expected_ca_issuer, expected_ca_fingerprint, creator_identity, state, consumed_at)
  VALUES ('$inst', '$inst', 770001, 'https://workload.example', 'mint-e2e',
          '$anchor', '$anchor', '$anchor', 'CN=aimee-kb-ca', '$anchor', 'owner',
          'consumed', now());

INSERT INTO kb_management_instance(installation_id, replacement_lineage_id, authority_id, team_id,
    workload_issuer, workload_subject, proof_anchor, custody_anchor, binding_digest,
    expected_ca_issuer, expected_ca_fingerprint, current_generation, current_enrollment_id, state)
  VALUES ('$inst', '$inst', '$authid', 770001, 'https://workload.example', 'mint-e2e',
          '$anchor', '$anchor', '$anchor', 'CN=aimee-kb-ca', '$anchor', 1, 770102, 'active');

-- The subject's write-tier grant: the authorization the mint re-reads live.
INSERT INTO kb_write_tier_grant(server_id, team_id, subject, tier, granted_by)
  VALUES ('mintsrv', 770001, 'alice', 'data', 'owner');
SQL
echo "fixture: installation=$inst enrollment(target)=770101 enrollment(local)=770102"
psqlt -c "SELECT 'revocation generation: '||generation FROM kb_cert_revocation_generation
            WHERE singleton=1;"

step "Login context: the two values a login front-end reads rather than is told"
# kb_management_identity_login_context is what the OIDC callback and the PAM route
# call before filing an intent. It must return exactly the kid and installation
# this rig provisioned — if it returned anything else, a real login would file an
# intent the mint then refuses, and the failure would surface far from its cause.
psqlt >"$work/logctx.out" 2>&1 <<SQL || true
BEGIN;
SELECT set_config('aimee.principal', 'alice', true);
SELECT set_config('aimee.team', '770001', true);
SELECT 'LOGCTX '||installation_id||' '||kid
  FROM kb_management_identity_login_context(770001);
COMMIT;
SQL
logctx=$(grep '^LOGCTX ' "$work/logctx.out" | head -1)
if [ -z "$logctx" ]; then
  echo "login context refused — a login front-end could not file an intent:" >&2
  sed -n '1,6p' "$work/logctx.out" >&2
  exit 5
fi
set -- $logctx
if [ "$2" != "$inst" ] || [ "$3" != "$kid" ]; then
  echo "login context DISAGREES with provisioned state:" >&2
  echo "  returned installation=$2 kid=$3" >&2
  echo "  provisioned installation=$inst kid=$kid" >&2
  exit 5
fi
echo "login context agrees: installation=${2:0:12}... kid=$3"

# The negative cases, which are the whole reason this is not a plain SELECT: a
# non-member must not be able to learn that this team has an active instance, and
# the named team must match the scope the caller set.
for probe in "notamember:770001:a stranger" "alice:770002:a team the caller is not scoped to"; do
  who=${probe%%:*}; rest=${probe#*:}; team=${rest%%:*}; label=${rest#*:}
  out=$(psqlt 2>&1 <<SQL || true
BEGIN;
SELECT set_config('aimee.principal', '$who', true);
SELECT set_config('aimee.team', '$team', true);
SELECT 'LEAK '||installation_id FROM kb_management_identity_login_context($team);
COMMIT;
SQL
)
  if printf '%s' "$out" | grep -q '^LEAK '; then
    echo "login context LEAKED to $label ($who/$team)" >&2
    exit 5
  fi
  echo "  refused $label: correct"
done

step "Identity intent (the login seam) then the snapshot (all 11 gates)"
# The intent writer runs as the authenticated principal with the grant as the
# authorization, exactly as a login front end drives it. Then the snapshot: it is
# read-only and enforces every precondition the mint has, so a returned ROW means
# all of them hold. That is the thing that has never happened before.
psqlt >"$work/intent.out" <<SQL
BEGIN;
SELECT set_config('aimee.principal', 'alice', true);
SELECT set_config('aimee.team', '770001', true);
SELECT correlation_id||' '||jti||' '||subject||' '||auth_mode
  FROM kb_management_identity_intent_start(
    -- sha256() is built in; gen_random_bytes would need pgcrypto. Distinct salts
    -- so the three identifiers cannot coincide.
    encode(sha256(convert_to('c'||random()||clock_timestamp(),'UTF8')),'hex'),
    encode(sha256(convert_to('j'||random()||clock_timestamp(),'UTF8')),'hex'),
    encode(sha256(convert_to('t'||random()||clock_timestamp(),'UTF8')),'hex'),
    770001, 'mintsrv', 'pam', 'kb', '$kid', 300, '$inst');
COMMIT;
SQL
intent=$(grep -E '^[0-9a-f]{64} ' "$work/intent.out" | head -1)
if [ -z "$intent" ]; then
  echo "intent was refused:" >&2
  cat "$work/intent.out" >&2
  exit 4
fi
set -- $intent
corr=$1; jti=$2
echo "intent filed: correlation=${corr:0:16}... subject=$3 auth_mode=$4"

step "Snapshot: every mint precondition, verified against real state"
psqlt >"$work/snapshot.out" 2>&1 <<SQL || true
SELECT 'SNAPSHOT_OK kid='||kid||' subject='||subject||' tier='||tier||
       ' team='||team_id||' token_version='||token_version||' seal_epoch='||vault_seal_epoch||
       ' pubkey_bytes='||octet_length(token_public_key)
  FROM kb_management_identity_authority_snapshot('$corr', '$jti');
SQL
if grep -q SNAPSHOT_OK "$work/snapshot.out"; then
  grep SNAPSHOT_OK "$work/snapshot.out"
  echo
  echo "  ALL 11 MINT GATES PASS. The authority is provisioned and the intent is"
  echo "  admissible: grant live, registry and both enrollments valid, instance"
  echo "  active, revocation generation current, JWKS publication in its window,"
  echo "  token root bound to it, and the vault unsealed at the matching epoch."
else
  echo "snapshot refused — the message names exactly one unmet precondition:" >&2
  sed -n '1,6p' "$work/snapshot.out" >&2
  exit 5
fi

step "MINT: identity_issue under vault custody"
# The one step psql cannot reach: admit -> use -> sign -> finalize happens in C.
# identity-mint-live links the authority service and vault objects and calls the
# same kb_mgmt_token_authority_service_issue the daemon hands to its IPC loop.
if [ ! -x ./identity-mint-live ]; then
  echo "identity-mint-live not built; run: make -C src identity-mint-live" >&2
  exit 6
fi
./identity-mint-live "postgres://$auth_role:$auth_pw@$pg_host:$pg_port/$db" "$corr" "$jti"

step "The mint is single-use"
# The same (correlation_id, jti) again must be refused: a token is spent on issue,
# so a replayed request can never yield a second one.
if ./identity-mint-live "postgres://$auth_role:$auth_pw@$pg_host:$pg_port/$db" "$corr" "$jti" \
     >/dev/null 2>"$work/replay.err"; then
  echo "FAIL: a replayed mint succeeded — the jti was not spent" >&2
  exit 7
fi
sed -n '1,2p' "$work/replay.err"
echo "  correct: the second attempt is refused"

step "What the mint recorded"
# The token is NOT stored: kb_management_identity_authority_finalize re-verifies the
# whole snapshot and returns, without writing the jwt back. That is deliberate — a
# minted token is a live bearer credential, and keeping one at rest would be a
# liability, so the intent's jwt/jwt_sha256 columns stay NULL on this path even
# though the table shape carries them. Single use is enforced by the key-use row
# below (unique on jti), which is what makes the replay above ALREADY_USED.
psqlt -c "SELECT 'intent state='||state||' jwt stored='||(jwt IS NOT NULL)::text||
                 '  (NULL is correct: a live token is never persisted)'
            FROM kb_management_identity_intent WHERE correlation_id='$corr';"
psqlt -c "SELECT 'key_use rows='||count(*)::text||'  (this row is what spends the jti)'
            FROM kb_management_identity_key_use_intent WHERE correlation_id='$corr';"

step "State summary"
psqlt <<'SQL'
SELECT 'token_root'  AS what, count(*)::text FROM kb_management_token_root
UNION ALL SELECT 'vault_current',  count(*)::text FROM org_vault_current
UNION ALL SELECT 'vault_secret',   count(*)::text FROM org_vault_secret
UNION ALL SELECT 'vault_rotation', count(*)::text FROM org_vault_rotation
UNION ALL SELECT 'vault_control_sealed',
       coalesce((SELECT sealed::text FROM kb_vault_control WHERE singleton=1), 'no row')
UNION ALL SELECT 'jwks_registry',  count(*)::text FROM kb_management_jwks_publication_registry
UNION ALL SELECT 'jwks_generation',count(*)::text FROM kb_management_jwks_publication_generation
UNION ALL SELECT 'jwks_candidate', count(*)::text FROM kb_management_jwks_publication_candidate;
SQL

printf '\nrun-identity-mint-e2e: provisioning chain completed\n'
