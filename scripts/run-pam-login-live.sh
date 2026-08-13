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
# and no PAM service file is shipped by the runtime-web image. On Debian a missing
# service file falls through to /etc/pam.d/other, which
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

# Uses the shared environment in scripts/lib/aimee-live-env.sh: the disposable
# database, the kb boot recipe and the throwaway host accounts were all duplicated
# here, and each copy was a place for the same traps to be rediscovered.
LIVE_KB_PORT=18861
LIVE_KB_BEARER="pam-live-token"
LIVE_SERVER_ID="pam-srv"

. "$(cd "$(dirname "$0")" && pwd)/lib/aimee-live-env.sh"

# The PAM service file is this rig's own state, not the shared environment's, so
# it is removed through the helper's extra-cleanup hook rather than a second EXIT
# trap (the helper installs the only one).
INSTALLED_PAM=0
LIVE_EXTRA_CLEANUP='[ "$INSTALLED_PAM" = "1" ] && rm -f /etc/pam.d/aimee'

live_env_init "pam-login" "$@"

[ -x ./aimee-kb-resolver ] || { echo "pam-live: ./aimee-kb-resolver not built" >&2; exit 2; }
# Read from the header rather than hard-coded, so the rig and the control cannot
# drift apart silently: change the budget and this assertion follows it.
KB_BUDGET=$(sed -nE 's/^#define[[:space:]]+KB_LOGIN_THROTTLE_BUDGET[[:space:]]+([0-9]+).*/\1/p' \
              src/kb/kb_login_throttle.h)
[ -n "$KB_BUDGET" ] || { echo "pam-live: could not read KB_LOGIN_THROTTLE_BUDGET" >&2; exit 2; }

live_env_pg_create

# --- the two host accounts --------------------------------------------------
# TWO, as §11 asks for, with distinct passwords so a rig that mixed them up shows
# as a failure rather than a pass.
step "Creating two real host accounts"
live_env_add_host_account "aimeepamt1$$" "Correct-Horse-$$-one"
U1="$LIVE_PAM_USER"; P1="$LIVE_PAM_PASS"
live_env_add_host_account "aimeepamt2$$" "Correct-Horse-$$-two"
U2="$LIVE_PAM_USER"; P2="$LIVE_PAM_PASS"
if [ -f /etc/pam.d/aimee ]; then
  echo "  /etc/pam.d/aimee EXISTS on this host — testing the host's own stack"
else
  echo "  /etc/pam.d/aimee ABSENT — pam_start(\"aimee\") will fall through to /etc/pam.d/other"
fi

# NOTE: the identity fixture is deliberately NOT seeded. These assertions turn on
# whether PAM accepted the password, and a request that authenticates is then
# refused for want of a grant -- which is exactly what assert_pam_accepted looks
# for. Seeding grants here would change what a non-401 means.
live_env_start_kb

TEAM_ID="$LIVE_TEAM"
SERVER_ID="$LIVE_SERVER_ID"
KB_PORT="$LIVE_KB_PORT"
KB_BEARER="$LIVE_KB_BEARER"
work="$LIVE_WORK"

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
# Restart kb to clear the login throttle between assertion groups.
#
# NOT a workaround: the throttle is in-memory and per-process, so a restart is the
# honest way to give a group a fresh budget. Several groups below deliberately
# spend more than the budget -- six wrong passwords in a row IS over the limit --
# and without a reset they would collide with the very control this rig asserts,
# reporting 429 where they expect 401. Each group therefore starts from a known
# empty budget, and the throttle group spends it on purpose.
restart_kb() { live_env_restart_kb; }

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

restart_kb
step "Negatives: every bad credential gets the SAME 401, with no enumeration"
assert_pam_rejected "$U1 wrong password  " "$U1" "not-$P1"
assert_pam_rejected "$U1 with U2's password" "$U1" "$P2"
# This assertion is about credential classification, not the peer budget spent
# above. Start it from a known-empty throttle so it deterministically reaches PAM.
restart_kb
assert_pam_rejected "nonexistent account " "aimeepamnosuch$$" "$P1"
# This group deliberately spends more than one budget, so it resets midway. That
# it has to is the control working: six wrong passwords in a row from one peer IS
# over the limit, which is the entire point of the group that follows.
restart_kb
assert_pam_rejected "empty-ish password  " "$U1" "x"
# `owner` is reserved by the schema and must be refused BEFORE PAM is consulted.
assert_pam_rejected "reserved name owner " "owner" "$P1"
# A username outside the bare-username grammar must never reach the host's stack.
assert_pam_rejected "ungrammatical name  " "oidc:iss:alice" "$P1"

restart_kb
step "The refusals are indistinguishable (no account-enumeration oracle)"
c1=$(login "$U1" "not-$P1"); b1=$(head -c200 "$work/out")
c2=$(login "aimeepamnosuch$$" "whatever"); b2=$(head -c200 "$work/out")
if [ "$c1" = "$c2" ] && [ "$b1" = "$b2" ]; then
  pass "wrong password and unknown account are byte-identical ($c1)"
else
  fail "a caller can tell a wrong password ($c1 $b1) from an unknown account ($c2 $b2)"
fi

restart_kb
step "Brute force is throttled (§11), on the real route"
# The gap this closes was measured here first: twelve wrong-password attempts in a
# row each returned an immediate 401, with no delay, lockout or backoff, on a route
# reachable with no bearer at all. Asserted end to end rather than only in the unit
# test, because the unit test cannot see the wiring -- a throttle that exists but is
# never consulted by the route passes every unit test there is.
throttled=0; first_401=0; attempts=0
for i in $(seq 1 12); do
  code=$(login "$U2" "definitely-wrong-$i")
  attempts=$((attempts+1))
  case "$code" in
    401) first_401=$((first_401+1)) ;;
    429) throttled=$i; break ;;
    *)   fail "unexpected $code on attempt $i while probing the throttle"; break ;;
  esac
done
expected=$((KB_BUDGET + 1))
if [ "$throttled" = "$expected" ]; then
  pass "throttled at attempt $throttled, exactly one past the budget (was: 12 x 401, unthrottled)"
elif [ "$throttled" -gt 0 ]; then
  # Still throttled, but not where the budget says it should be. Worth a failure
  # rather than a pass: a budget that does not match the constant is a budget
  # nobody can reason about from the source.
  fail "throttled at attempt $throttled, expected $expected (budget=$KB_BUDGET)"
else
  fail "12 wrong-password attempts and never throttled -- §11 'brute-force is rate-limited' unmet"
fi
# The refusal must carry a wait the caller can act on...
if grep -q '"retry_after"' "$work/out" 2>/dev/null; then
  pass "the 429 carries retry_after: $(head -c120 "$work/out")"
else
  fail "the 429 does not tell the caller how long to wait: $(head -c120 "$work/out")"
fi
# ...and it must apply to a CORRECT password too. If a valid credential slipped
# past the throttle, an attacker who guessed right on attempt 500 would still win,
# and the throttle would be a side channel confirming the guess.
code=$(login "$U2" "$P2")
if [ "$code" = "429" ]; then
  pass "a CORRECT password is also throttled while locked out (no confirmation oracle)"
else
  fail "a correct password bypassed the lockout -> $code (the throttle is a side channel)"
fi
# A DIFFERENT username from the same peer is also refused: the per-peer budget is
# what stops one host spraying a password across an account list.
code=$(login "$U1" "$P1")
if [ "$code" = "429" ]; then
  pass "the same peer is refused for a different account (per-peer budget holds)"
else
  fail "the peer budget did not hold for a second account -> $code"
fi

restart_kb
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
kill "$LIVE_KB_PID" 2>/dev/null; sleep 1; kill -9 "$LIVE_KB_PID" 2>/dev/null; wait "$LIVE_KB_PID" 2>/dev/null
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
# Started by hand, NOT via live_env_start_kb: that helper strips every
# AIMEE_KB_OIDC_* variable (a stray one makes PAM answer 409 and every PAM
# assertion pass vacuously), and this is the one place that wants them set.
./aimee-kb --http-port="$KB_PORT" >"$work/kb-oidc.log" 2>&1 &
LIVE_KB_PID=$!
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

live_env_verdict "real host accounts authenticate through kb's PAM route"
