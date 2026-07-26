#!/bin/bash
# run-pam-login-live.sh — the PAM half of acceptance §11, against a REAL PAM stack
# and REAL host accounts.
#
# WHY THIS EXISTS. §11 requires the happy path on BOTH identity paths: "OIDC:
# subjects with tiers data/off -> memory.store 2xx/403 ... PAM: same via two PAM
# accounts." The OIDC path has a live rig and a blocking CI job. The PAM path had
# neither. Its only coverage is test_kb_http_identity_login.c, which cannot call
# PAM at all — so the question "does this authenticate a real OS user on a real
# host?" had never been asked. §10 phase 5 flagged exactly this as an
# implementer-facing gap ("a working pam_unix-backed service file and two known
# test users") and it was never closed.
#
# The failure modes here are precisely the ones a unit test cannot see: the wrong
# PAM service name, a missing service file, or a kb without the privilege to read
# /etc/shadow. Each yields "authentication failed" for every user — indistinguishable
# from a wrong password, on purpose, which is what makes it so easy to ship broken.
#
# WHAT MAKES THIS DECISIVE WITHOUT THE VAULT CHAIN. post_login_pam checks the
# password FIRST and only then files a mint intent. So the two outcomes separate
# cleanly on the credential check alone:
#
#   correct password -> 403 "no write-tier grant..." / "not a member of that team"
#                       (PAM ACCEPTED; refused later, for a reason that is not PAM)
#   wrong password   -> 401 "authentication failed"  (PAM REJECTED)
#
# A 401 is the ONLY answer that means the credential check failed. That is why this
# rig needs no vault-custodied signing key, no grant and no token authority: what it
# tests is finished before any of those are consulted. Collecting an actual token is
# run-identity-mint-e2e.sh's job, and the tier gate is
# run-write-tier-enforce-live.sh's.
#
# ON THE SERVICE NAME, which is a real portability finding. pam_check_credentials
# calls pam_start("aimee", ...), and NOTHING in this repo installs /etc/pam.d/aimee
# — the only shipped service file is pam-aimee-runtime-web, for a different service
# name. On Debian a missing service file falls through to /etc/pam.d/other, which
# @includes common-auth and therefore works; on a distribution whose `other` is
# pam_deny.so it would fail closed for every user. The rig asserts BOTH shapes: the
# host's own fallback, and an explicitly installed service file. Neither result is
# assumed.
#
# MUST RUN AS ROOT: pam_unix reads /etc/shadow, and the rig creates and deletes
# throwaway host accounts.
#
# Usage: run-pam-login-live.sh [--keep] [postgres://superuser@host:port/db]
set -uo pipefail
export LC_ALL=C

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
keep=0
PG_ARG=""
for a in "$@"; do
  case "$a" in
    --keep) keep=1 ;;
    postgres://*|postgresql://*) PG_ARG="$a" ;;
    *) echo "pam-live: unknown argument '$a'" >&2; exit 2 ;;
  esac
done

[ "$(id -u)" = "0" ] || { echo "pam-live: must run as root (pam_unix reads /etc/shadow)" >&2; exit 2; }
for b in ./aimee-kb; do
  [ -x "$b" ] || { echo "pam-live: $b not built (make -C src all)" >&2; exit 2; }
done
[ -x ./aimee-kb-resolver ] || { echo "pam-live: ./aimee-kb-resolver not built" >&2; exit 2; }

work=$(mktemp -d /tmp/aimee-pam-live-XXXXXX)
export AIMEE_HOME="$work/home"
mkdir -p "$AIMEE_HOME"
kb_log="$work/kb.log"
db="aimee_pam_$$"
kbpw="pam$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
KB_PORT=18861
KB_BEARER="pam-live-token"
TEAM_ID=990001
SERVER_ID="pam-srv"
# Two accounts, as §11 asks for. Distinct passwords, so a rig that mixed them up
# would show as a failure rather than a pass.
U1="aimeepamt1$$"; P1="Correct-Horse-$$-one"
U2="aimeepamt2$$"; P2="Correct-Horse-$$-two"
INSTALLED_PAM=0

FAILED=0
pass() { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; FAILED=$((FAILED+1)); }
step() { echo; echo "== $*"; }

pg_admin() { :; }
cleanup() {
  [ -n "${kb_pid:-}" ] && kill "$kb_pid" 2>/dev/null
  sleep 1
  [ -n "${kb_pid:-}" ] && kill -9 "$kb_pid" 2>/dev/null
  [ -n "${kb_pid:-}" ] && wait "$kb_pid" 2>/dev/null
  # Host accounts are real system state; leaving them behind would be a slow leak
  # of login-capable users on any box this ever runs on.
  userdel -r "$U1" 2>/dev/null
  userdel -r "$U2" 2>/dev/null
  [ "$INSTALLED_PAM" = "1" ] && rm -f /etc/pam.d/aimee
  pg_admin "DROP DATABASE IF EXISTS $db" >/dev/null 2>&1
  pg_admin "DROP ROLE IF EXISTS aimee_pam_owner_$$" >/dev/null 2>&1
  if [ "$keep" = "1" ]; then echo "kept: $work"; else rm -rf "$work"; fi
}
trap cleanup EXIT

# --- postgres, the same two ways in as the enforcement rig ------------------
PGSU_URL="${PG_ARG:-${AIMEE_TEST_PG_URL:-}}"
if [ -n "$PGSU_URL" ]; then
  PG_ADMIN_URL="${PGSU_URL%/*}/postgres"
  pg_admin() { psql -v ON_ERROR_STOP=1 -q "$PG_ADMIN_URL" -c "$1"; }
  pg_indb()  { psql -v ON_ERROR_STOP=1 -q "${PGSU_URL%/*}/$db" -c "$1"; }
  pg_host=$(printf '%s' "$PGSU_URL" | sed -E 's#.*@([^:/]+).*#\1#')
  pg_port=$(printf '%s' "$PGSU_URL" | sed -nE 's#.*:([0-9]+)/.*#\1#p'); pg_port=${pg_port:-5432}
else
  pg_admin() { su postgres -c "psql -v ON_ERROR_STOP=1 -q -c \"$1\""; }
  pg_indb()  { su postgres -c "psql -v ON_ERROR_STOP=1 -q -d $db -c \"$1\""; }
  pg_host=127.0.0.1; pg_port=5432
fi

step "Provisioning a disposable database"
owner="aimee_pam_owner_$$"
pg_admin "CREATE ROLE $owner LOGIN PASSWORD '$kbpw'" >/dev/null 2>&1 \
  || { echo "pam-live: could not create the owner role" >&2; exit 2; }
pg_admin "CREATE DATABASE $db OWNER $owner" >/dev/null 2>&1 \
  || { echo "pam-live: could not create the database" >&2; exit 2; }
pg_indb "GRANT CREATE ON SCHEMA public TO $owner" >/dev/null 2>&1
pg_indb "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null 2>&1
echo "database $db on $pg_host:$pg_port"

# --- the two host accounts --------------------------------------------------
step "Creating two real host accounts"
for pair in "$U1:$P1" "$U2:$P2"; do
  u=${pair%%:*}; p=${pair#*:}
  useradd -M -s /usr/sbin/nologin "$u" 2>/dev/null \
    || { echo "pam-live: could not create $u" >&2; exit 2; }
  printf '%s:%s\n' "$u" "$p" | chpasswd \
    || { echo "pam-live: could not set a password for $u" >&2; exit 2; }
done
# Independent proof the accounts really do authenticate, so a later 401 can be
# attributed to kb rather than to a broken fixture. A rig that cannot tell those
# apart reports "PAM is broken" when it created the users wrong.
if command -v python3 >/dev/null 2>&1 && python3 -c 'import pam' >/dev/null 2>&1; then
  echo "  (python3-pam present; kb remains the authority under test)"
fi
echo "accounts $U1 and $U2 created"
if [ -f /etc/pam.d/aimee ]; then
  echo "  /etc/pam.d/aimee EXISTS on this host — testing the host's own stack"
else
  echo "  /etc/pam.d/aimee ABSENT — pam_start(\"aimee\") will fall through to /etc/pam.d/other"
fi

# --- kb ---------------------------------------------------------------------
step "Starting aimee-kb in PAM mode (no OIDC profile configured)"
cat > "$AIMEE_HOME/aimee.yaml" <<YAML
embedding_dim: 1024
kb:
  api:
    bearer_token: $KB_BEARER
YAML
export AIMEE_DB2_URL="postgres://$owner:$kbpw@$pg_host:$pg_port/$db"
export AIMEE_KB_API_BEARER_TOKEN="$KB_BEARER"
# Every OIDC variable must be unset: a configured profile makes the PAM route
# answer 409 and this whole rig would pass vacuously.
for v in $(env | sed -nE 's/^(AIMEE_KB_OIDC[A-Z_]*)=.*/\1/p'); do unset "$v"; done
./aimee-kb --http-port="$KB_PORT" >"$kb_log" 2>&1 &
kb_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "Authorization: Bearer $KB_BEARER" "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 && break
  kill -0 "$kb_pid" 2>/dev/null || { echo "pam-live: aimee-kb exited; see $kb_log" >&2; tail -20 "$kb_log" >&2; exit 2; }
  sleep 1
done
curl -sf -H "Authorization: Bearer $KB_BEARER" "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 \
  || { echo "pam-live: aimee-kb never became healthy" >&2; tail -20 "$kb_log" >&2; exit 2; }
echo "aimee-kb healthy on $KB_PORT"

login() { # login <user> <password> -> prints "<status> <body>"
  local u=$1 p=$2
  local body
  body=$(printf '{"username":"%s","password":"%s","server_id":"%s","team_id":%d}' \
           "$u" "$p" "$SERVER_ID" "$TEAM_ID")
  curl -s -o "$work/out" -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $KB_BEARER" -H 'Content-Type: application/json' \
    --data "$body" "http://127.0.0.1:$KB_PORT/v1/identity/login/pam"
}

# A 401 is the ONE answer that means the credential check itself failed. Anything
# else means PAM accepted the password and the request was refused further along,
# where the reasons are grants and membership rather than authentication.
assert_pam_accepted() { # <label> <user> <password>
  local label=$1 code; code=$(login "$2" "$3")
  local body; body=$(head -c200 "$work/out")
  if [ "$code" = "401" ]; then
    fail "$label -> 401 (PAM REJECTED a correct password): $body"
  else
    pass "$label -> $code (PAM accepted; refused later for a non-PAM reason)"
  fi
}
assert_pam_rejected() { # <label> <user> <password>
  local label=$1 code; code=$(login "$2" "$3")
  if [ "$code" = "401" ]; then pass "$label -> 401"
  else fail "$label -> $code (expected 401; a bad credential must not get further)"; fi
}

# --- the assertions ---------------------------------------------------------
step "Acceptance §11 (PAM): two real accounts authenticate"
assert_pam_accepted "$U1 correct password" "$U1" "$P1"
assert_pam_accepted "$U2 correct password" "$U2" "$P2"

step "Negatives: every bad credential gets the SAME 401, with no enumeration"
assert_pam_rejected "$U1 wrong password  " "$U1" "not-$P1"
assert_pam_rejected "$U1 with U2's password" "$U1" "$P2"
assert_pam_rejected "nonexistent account " "aimeepamnosuch$$" "$P1"
assert_pam_rejected "empty-ish password  " "$U1" "x"
# `owner` is reserved by the schema and must be refused BEFORE PAM is consulted.
assert_pam_rejected "reserved name owner " "owner" "$P1"
# A username outside the bare-username grammar must never reach the host's stack.
assert_pam_rejected "ungrammatical name  " "oidc:iss:alice" "$P1"

step "The refusals are indistinguishable (no account-enumeration oracle)"
c1=$(login "$U1" "not-$P1"); b1=$(head -c200 "$work/out")
c2=$(login "aimeepamnosuch$$" "whatever"); b2=$(head -c200 "$work/out")
if [ "$c1" = "$c2" ] && [ "$b1" = "$b2" ]; then
  pass "wrong password and unknown account are byte-identical ($c1)"
else
  fail "a caller can tell a wrong password ($c1 $b1) from an unknown account ($c2 $b2)"
fi

step "An explicitly installed /etc/pam.d/aimee works too"
# The host fallback is what the assertions above exercised. A deployment that ships
# its own service file must work as well, and on a distribution whose /etc/pam.d/other
# is pam_deny.so it is the ONLY thing that works -- which is the portability risk
# this rig exists to surface.
if [ -f /etc/pam.d/aimee ]; then
  pass "skipped: this host already ships /etc/pam.d/aimee (tested above)"
else
  cat > /etc/pam.d/aimee <<'PAMFILE'
# Installed by run-pam-login-live.sh. Self-contained on purpose, so it proves the
# service name resolves rather than proving /etc/pam.d/other happens to work.
auth     required pam_unix.so
account  required pam_unix.so
PAMFILE
  INSTALLED_PAM=1
  assert_pam_accepted "$U1 with an explicit service file" "$U1" "$P1"
  assert_pam_rejected "$U1 wrong password, explicit file" "$U1" "not-$P1"
  rm -f /etc/pam.d/aimee; INSTALLED_PAM=0
fi

step "OIDC and PAM are mutually exclusive"
# With a usable OIDC profile the PAM route must refuse with 409 and never consult a
# password -- otherwise an IdP's MFA and lockout policy is bypassable by anyone with
# a local account.
kill "$kb_pid" 2>/dev/null; sleep 1; kill -9 "$kb_pid" 2>/dev/null; wait "$kb_pid" 2>/dev/null
# The full profile kb_oidc_login_config_from_env requires. A PARTIAL profile is
# KB_OIDC_LOGIN_INVALID, which deliberately falls back to PAM -- so a rig that set
# only some of these would test the fallback and report mutual exclusion as proven.
# The names are taken from run-oidc-login-live.sh, which is known to produce a
# usable profile; guessing them is how this assertion silently becomes vacuous.
export AIMEE_KB_OIDC_ISSUER="https://idp.aimee.test"
export AIMEE_KB_OIDC_LOGIN_CLIENT_ID="aimee-kb"
export AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL="https://idp.aimee.test/authorize"
export AIMEE_KB_OIDC_LOGIN_TOKEN_URL="https://idp.aimee.test/token"
export AIMEE_KB_OIDC_LOGIN_REDIRECT_URI="https://kb.aimee.test/v1/identity/login/callback"
export AIMEE_KB_OIDC_LOGIN_SCOPE="openid profile"
./aimee-kb --http-port="$KB_PORT" >"$work/kb-oidc.log" 2>&1 &
kb_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "Authorization: Bearer $KB_BEARER" "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 && break
  sleep 1
done
mode=$(curl -s -H "Authorization: Bearer $KB_BEARER" "http://127.0.0.1:$KB_PORT/v1/identity/auth-mode")
code=$(login "$U1" "$P1")
if [ "$code" = "409" ]; then
  pass "with OIDC configured, the PAM route refuses -> 409 (auth-mode: $mode)"
elif printf '%s' "$mode" | grep -q '"mode":"pam"'; then
  # An INVALID profile deliberately falls back to PAM, and auth-mode says so. That is
  # documented behaviour, not a failure -- but it means this assertion did not test
  # what it set out to, so it must say so rather than quietly pass.
  fail "the OIDC profile was not usable (auth-mode: $mode), so mutual exclusion went UNTESTED; got $code"
else
  fail "with OIDC configured the PAM route returned $code, expected 409 (auth-mode: $mode)"
fi

step "Result"
if [ "$FAILED" = "0" ]; then
  echo "== PASSED — real host accounts authenticate through kb's PAM route"
  echo "LIVE_EXIT=0"
  exit 0
fi
echo "== FAILED — $FAILED assertion(s)"
echo "kb log:"; tail -30 "$kb_log" 2>/dev/null
echo "LIVE_EXIT=1"
exit 1
