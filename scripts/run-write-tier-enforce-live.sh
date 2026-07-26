#!/bin/bash
# run-write-tier-enforce-live.sh — does a minted identity token's tier actually
# GATE A REAL WRITE?
#
# WHY THIS EXISTS. This is the one question the feature exists to answer, and until
# now nothing asked it. Everything else on this branch tests grant ADMINISTRATION —
# writing a grant row and auditing it — which is the setup, not the point.
# Acceptance §11 opens with: "OIDC: subjects with tiers data/off -> memory.store
# 2xx/403, both reads 2xx." No test, live rig or CI job covered that sentence:
#
#   - identity-mint-e2e mints a token and validates its STRUCTURE, then throws it
#     away. It never presents it to a server.
#   - test_server_http.c covers management-config PARSING, not the gate.
#   - test_server_write_tier*.c cover verification in isolation, with the JWKS
#     handed to them directly.
#   - run-grant-cli*-live.sh drive the grant ADMIN routes over UDS, which is
#     structurally exempt from the gate (resolve_write_tier is a no-op when !is_tcp).
#
# So the chain
#
#     minted token(tier) -> HTTP over TCP -> resolve_write_tier -> route gate -> write
#
# had never been run end to end. The last three defects found on this branch were
# all composition defects at exactly this kind of seam, and every one of them was
# invisible to the layer tests because each layer supplied its own stub.
#
# WHAT IS REAL HERE. A real aimee-server with a real TCP listener, a real
# aimee-kb, real Postgres, a real Ed25519-signed JWKS publication envelope in db1
# pinned by a real root-owned 0600 trust bundle, and real RS256 tokens. Nothing is
# stubbed and no check is weakened to make it pass.
#
# WHAT IS STOOD IN FOR, deliberately: the RSA token key is generated locally rather
# than custodied in the vault. That is not a gap in THIS rig — key custody is what
# run-identity-mint-e2e.sh proves against a real KMS helper. What that rig cannot
# tell you, and this one can, is what a server does with a token once it has one.
#
# HOW A CALLER PRESENTS A TOKEN, which is not obvious and is worth recording: over
# TCP the server runs two independent checks against the SAME request.
# server_http_authorize demands a credential equal to the configured bearer, and
# server_http_resolve_write_tier reads the identity token out of `Authorization`.
# Both must be satisfied at once, so the identity token goes in `Authorization`
# and the server bearer goes in `x-api-key`. Putting the identity token in
# `Authorization` ALONE gets a 401 from the bearer check before the gate is ever
# consulted.
#
# MUST RUN AS ROOT on a host with Postgres: server_mgmt_jwks_trust_bundle_load
# requires the bundle to be a root-owned, single-link, 0600 regular file, and
# refuses anything else. That refusal is a security property worth keeping, so the
# rig complies with it rather than relaxing it.
#
# Usage: run-write-tier-enforce-live.sh [--keep]
set -uo pipefail
export LC_ALL=C

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
keep=0
[ "${1:-}" = "--keep" ] && keep=1

[ "$(id -u)" = "0" ] || { echo "enforce-live: must run as root (the trust bundle must be root-owned)" >&2; exit 2; }
for b in ./aimee ./aimee-server ./aimee-kb ./write-tier-enforce-live; do
  [ -x "$b" ] || { echo "enforce-live: $b not built" >&2; exit 2; }
done
# kb FORKS this beside its own executable; a missing resolver fails later as
# something else entirely. Learned on this branch's first real CI run.
[ -x ./aimee-kb-resolver ] || { echo "enforce-live: ./aimee-kb-resolver not built" >&2; exit 2; }

work=$(mktemp -d /tmp/aimee-enforce-live-XXXXXX)
export AIMEE_HOME="$work/home"
mkdir -p "$AIMEE_HOME"
kb_log="$work/kb.log"; srv_log="$work/server.log"
db="aimee_enforce_$$"
kbpw="enf$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
KB_PORT=18841
SRV_TCP_PORT=18843
SERVER_ID="enforce-srv"
TEAM_ID=990001
SRV_BEARER="enforce-tcp-bearer"
KB_BEARER="enforce-kb-token"
BUNDLE=/root/aimee-enforce-trust-$$.pem
TOKEN_KEY="$work/token.pem"

FAILED=0
pass() { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; FAILED=$((FAILED+1)); }
step() { echo; echo "== $*"; }

cleanup() {
  [ -n "${srv_pid:-}" ] && kill "$srv_pid" 2>/dev/null
  [ -n "${kb_pid:-}" ] && kill "$kb_pid" 2>/dev/null
  sleep 1
  [ -n "${srv_pid:-}" ] && kill -9 "$srv_pid" 2>/dev/null
  [ -n "${kb_pid:-}" ] && kill -9 "$kb_pid" 2>/dev/null
  # Reap them, so the shell does not print "Killed" job notices after the verdict.
  # In CI output a stray "Killed" reads as a crash in the thing being tested.
  [ -n "${srv_pid:-}" ] && wait "$srv_pid" 2>/dev/null
  [ -n "${kb_pid:-}" ] && wait "$kb_pid" 2>/dev/null
  rm -f "$BUNDLE"
  su postgres -c "dropdb --if-exists $db" >/dev/null 2>&1
  su postgres -c "psql -qc \"DROP ROLE IF EXISTS aimee_kb_owner_$$\"" >/dev/null 2>&1
  if [ "$keep" = "1" ]; then echo "kept: $work"; else rm -rf "$work"; fi
}
trap cleanup EXIT

# --- database -------------------------------------------------------------
step "Provisioning a disposable database"
owner="aimee_kb_owner_$$"
su postgres -c "psql -qc \"CREATE ROLE $owner LOGIN PASSWORD '$kbpw'\"" >/dev/null 2>&1 \
  || { echo "enforce-live: could not create the owner role" >&2; exit 2; }
su postgres -c "createdb -O $owner $db" >/dev/null 2>&1 \
  || { echo "enforce-live: could not create the database" >&2; exit 2; }
# PG15+ : the database owner still needs CREATE on public explicitly.
su postgres -c "psql -q -d $db -c \"GRANT CREATE ON SCHEMA public TO $owner\"" >/dev/null 2>&1
echo "database $db owned by $owner"

# --- the management trust chain -------------------------------------------
# Seeded BEFORE the server starts, into the very db1 file the server will open
# ($AIMEE_HOME/aimee.db), because the server reads the envelope at request time
# and an absent one denies every token as INVALID — which looks identical to a
# forged token and would make this rig "pass" for the wrong reason.
step "Provisioning the JWKS trust chain (real envelope, real signature)"
kid=$(./write-tier-enforce-live provision \
        --db1 "$AIMEE_HOME/aimee.db" --bundle "$BUNDLE" --key "$TOKEN_KEY") \
  || { echo "enforce-live: trust chain provisioning failed" >&2; exit 2; }
echo "${kid}   trust bundle $BUNDLE (root-owned 0600)"

# --- kb --------------------------------------------------------------------
step "Starting aimee-kb"
cat > "$AIMEE_HOME/aimee.yaml" <<YAML
embedding_dim: 1024
kb:
  api:
    bearer_token: $KB_BEARER
aimee:
  api:
    http_port: $SRV_TCP_PORT
    bearer_token: $SRV_BEARER
    remote_writes: "off"
YAML
export AIMEE_DB2_URL="postgres://$owner:$kbpw@127.0.0.1:5432/$db"
export AIMEE_KB_API_BEARER_TOKEN="$KB_BEARER"
./aimee-kb --http-port="$KB_PORT" >"$kb_log" 2>&1 &
kb_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "Authorization: Bearer $KB_BEARER" \
    "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 && break
  kill -0 "$kb_pid" 2>/dev/null || { echo "enforce-live: aimee-kb exited; see $kb_log" >&2; tail -20 "$kb_log" >&2; exit 2; }
  sleep 1
done
curl -sf -H "Authorization: Bearer $KB_BEARER" "http://127.0.0.1:$KB_PORT/v1/health" >/dev/null 2>&1 \
  || { echo "enforce-live: aimee-kb never became healthy" >&2; tail -20 "$kb_log" >&2; exit 2; }
echo "aimee-kb healthy on $KB_PORT"

# --- server ----------------------------------------------------------------
# AIMEE_SERVER_ID is the token AUDIENCE and AIMEE_SERVER_TEAM_ID the single team
# this server is enrolled in; both are read by server_write_tier_db1's
# build_config, and either being unset denies every token for a reason that has
# nothing to do with the token.
step "Starting aimee-server with a TCP listener and the trust bundle"
export AIMEE_KB_API_URL="http://127.0.0.1:$KB_PORT"
export AIMEE_SERVER_ID="$SERVER_ID"
export AIMEE_SERVER_TEAM_ID="$TEAM_ID"
export AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE="$BUNDLE"
./aimee-server >"$srv_log" 2>&1 &
srv_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "x-api-key: $SRV_BEARER" "http://127.0.0.1:$SRV_TCP_PORT/v1/health" >/dev/null 2>&1 && break
  kill -0 "$srv_pid" 2>/dev/null || { echo "enforce-live: aimee-server exited; see $srv_log" >&2; tail -20 "$srv_log" >&2; exit 2; }
  sleep 1
done
health=$(curl -s -o /dev/null -w '%{http_code}' -H "x-api-key: $SRV_BEARER" \
           "http://127.0.0.1:$SRV_TCP_PORT/v1/health")
[ "$health" = "200" ] || { echo "enforce-live: TCP listener not usable (health=$health)" >&2; tail -20 "$srv_log" >&2; exit 2; }
echo "aimee-server TCP listener healthy on $SRV_TCP_PORT"

# --- helpers ---------------------------------------------------------------
# A FRESH jti per call, from entropy rather than a counter. `t=$(mint data)` runs
# mint in a COMMAND-SUBSTITUTION SUBSHELL, so a `jti_n=$((jti_n+1))` inside it is
# lost to the parent and every token comes out with the same jti. Each one is then
# a genuine replay of the first, and the rig reports the gate refusing valid tokens
# when the gate is behaving exactly as designed. Entropy has no such coupling.
mint() { # mint <tier> [extra args...]
  local tier=$1; shift
  local jti="enf-$$-$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  ./write-tier-enforce-live mint --key "$TOKEN_KEY" --aud "$SERVER_ID" \
    --team "$TEAM_ID" --sub "oidc:test:alice" --tier "$tier" --jti "$jti" "$@"
}
# POST a request carrying an identity token. The server bearer rides in x-api-key
# so that `Authorization` is free to carry the token -- see the header note.
#
# The curl arguments are built in an ARRAY. An unquoted ${token:+-H "Authorization:
# Bearer $token"} word-splits into four arguments, so curl receives a malformed
# header and treats the rest as extra URLs -- which silently produces the status of
# a DIFFERENT request. That cost a full debugging cycle here: it made a working gate
# look like it refused every token after the first.
call() { # call <method> <path> <token> [body] -> prints status, body in $work/out
  local method=$1 path=$2 token=$3 body=${4:-'{}'}
  local args=(-s -o "$work/out" -w '%{http_code}' -X "$method"
              -H "x-api-key: $SRV_BEARER" -H 'Content-Type: application/json')
  [ -n "$token" ] && args+=(-H "Authorization: Bearer $token")
  args+=(--data "$body" "http://127.0.0.1:$SRV_TCP_PORT$path")
  curl "${args[@]}"
}
STORE=/v1/memory/store
SEARCH=/v1/memory/search
CRON=/v1/cron/add
# The real field contract: handle_memory_store needs BOTH key and content, and
# handle_memory_search needs a non-empty `keywords` ARRAY. A payload the handler
# rejects still returns HTTP 200, so asserting on the status alone would pass while
# writing nothing -- which is why the store assertion also checks the body.
store_body='{"key":"enforce-rig-probe","content":"enforcement rig probe"}'
search_body='{"keywords":["enforce"]}'
cron_body='{"name":"enf-probe","schedule":"* * * * *","command":"true"}'

is2xx() { [ "$1" -ge 200 ] && [ "$1" -lt 300 ]; }
# A 200 whose body is an application-level error is NOT a successful write. The gate
# is what this rig tests, but an assertion that cannot tell "allowed and stored" from
# "allowed and rejected downstream" would hide a gate that opened onto nothing.
stored_ok() { is2xx "$1" && ! grep -q '"status":"error"' "$work/out" 2>/dev/null; }

# --- 1. the acceptance sentence -------------------------------------------
step "Acceptance §11: tier data/off -> memory.store 2xx/403, both reads 2xx"
t_data=$(mint data)
code=$(call POST "$STORE" "$t_data" "$store_body")
if stored_ok "$code"; then pass "tier=data  memory.store -> $code and stored"
else fail "tier=data  memory.store -> $code (expected a stored 2xx); body: $(head -c200 "$work/out")"; fi

t_off=$(mint off)
code=$(call POST "$STORE" "$t_off" "$store_body")
if [ "$code" = "403" ]; then pass "tier=off   memory.store -> 403"
else fail "tier=off   memory.store -> $code (expected 403)"; fi

t_data_r=$(mint data)
code=$(call POST "$SEARCH" "$t_data_r" "$search_body")
if is2xx "$code"; then pass "tier=data  memory.search -> $code"
else fail "tier=data  memory.search -> $code (expected 2xx)"; fi

t_off_r=$(mint off)
code=$(call POST "$SEARCH" "$t_off_r" "$search_body")
if is2xx "$code"; then pass "tier=off   memory.search -> $code (a read is not gated by tier)"
else fail "tier=off   memory.search -> $code (expected 2xx; reads must not be gated)"; fi

# --- 2. the two tiers are actually distinct -------------------------------
# If `data` also opened exec/control routes the two tiers would be one tier, and
# the §11 happy path above would still pass.
step "data and full are distinct: an exec/control route needs full"
t=$(mint data)
code=$(call POST "$CRON" "$t" "$cron_body")
if [ "$code" = "403" ]; then pass "tier=data  cron.add -> 403 (exec/control needs full)"
else fail "tier=data  cron.add -> $code (expected 403)"; fi

t=$(mint full)
code=$(call POST "$CRON" "$t" "$cron_body")
if [ "$code" != "403" ]; then pass "tier=full  cron.add -> $code (not refused by the tier gate)"
else fail "tier=full  cron.add -> 403 (full should clear the tier gate)"; fi

# --- 3. claim negatives, all fail closed ----------------------------------
step "Token/claim negatives (§11): every one must DENY the write"
deny() { # deny <label> <token>
  local label=$1 token=$2
  local code; code=$(call POST "$STORE" "$token" "$store_body")
  if [ "$code" = "403" ] || [ "$code" = "401" ]; then pass "$label -> $code"
  else fail "$label -> $code (expected a refusal)"; fi
}
deny "wrong audience        " "$(./write-tier-enforce-live mint --key "$TOKEN_KEY" --aud other-server --team $TEAM_ID --sub 'oidc:test:alice' --tier full --jti enf-$$-aud)"
deny "team not enrolled     " "$(./write-tier-enforce-live mint --key "$TOKEN_KEY" --aud "$SERVER_ID" --team 888777 --sub 'oidc:test:alice' --tier full --jti enf-$$-team)"
deny "expired               " "$(./write-tier-enforce-live mint --key "$TOKEN_KEY" --aud "$SERVER_ID" --team $TEAM_ID --sub 'oidc:test:alice' --tier full --jti enf-$$-exp --iat $(( $(date +%s) - 7200 )) --exp $(( $(date +%s) - 3600 )))"
deny "wrong issuer          " "$(./write-tier-enforce-live mint --key "$TOKEN_KEY" --aud "$SERVER_ID" --team $TEAM_ID --sub 'oidc:test:alice' --tier full --jti enf-$$-iss --issuer not-kb)"
# A token signed by a key the JWKS does not carry: the IdP-signed / rotated-away
# case. Its kid is derived from ITS OWN modulus, so it is well-formed and simply
# unknown to this server.
./write-tier-enforce-live provision --db1 "$work/other.db" --bundle "$work/other.pem" --key "$work/other-key.pem" >/dev/null 2>&1
deny "signed by a foreign key" "$(./write-tier-enforce-live mint --key "$work/other-key.pem" --aud "$SERVER_ID" --team $TEAM_ID --sub 'oidc:test:alice' --tier full --jti enf-$$-key)"
deny "tampered signature    " "$(mint full | sed 's/.$/A/')"

# --- 4. no token at all ----------------------------------------------------
# The legacy-cutover half of §11: a valid shared bearer, and nothing else, must
# not carry a write. This is the regression that matters most if the gate is ever
# accidentally made permissive on an absent token.
step "Legacy cutover: the shared bearer ALONE must not carry a write"
code=$(call POST "$STORE" "" "$store_body")
if [ "$code" = "403" ]; then pass "bearer only, no identity token -> 403"
else fail "bearer only, no identity token -> $code (expected 403)"; fi
code=$(call POST "$SEARCH" "" "$search_body")
if is2xx "$code"; then pass "bearer only, read -> $code (reads stay open)"
else fail "bearer only, read -> $code (expected 2xx)"; fi

# --- 5. replay -------------------------------------------------------------
step "Replay: a token is single-use"
t=$(mint data)
first=$(call POST "$STORE" "$t" "$store_body")
second=$(call POST "$STORE" "$t" "$store_body")
if stored_ok "$first"; then pass "first use  -> $first and stored"
else fail "first use  -> $first (expected 2xx)"; fi
if [ "$second" = "403" ] || [ "$second" = "401" ]; then pass "replayed   -> $second"
else fail "replayed   -> $second (expected a refusal; the jti was already spent)"; fi

# --- 6. the retired global changes nothing --------------------------------
# §11: "Flipping aimee.api.remote_writes changes NO /v1 write outcome." Asserted
# by actually flipping it to the most permissive value and re-running the two
# outcomes that define the feature.
step "The retired global authorizer changes no outcome"
sed -i 's/    remote_writes: "off"/    remote_writes: full/' "$AIMEE_HOME/aimee.yaml"
kill "$srv_pid" 2>/dev/null; sleep 1; kill -9 "$srv_pid" 2>/dev/null; wait "$srv_pid" 2>/dev/null
./aimee-server >"$srv_log.full" 2>&1 &
srv_pid=$!
for i in $(seq 1 60); do
  curl -sf -H "x-api-key: $SRV_BEARER" "http://127.0.0.1:$SRV_TCP_PORT/v1/health" >/dev/null 2>&1 && break
  sleep 1
done
code=$(call POST "$STORE" "$(mint off)" "$store_body")
if [ "$code" = "403" ]; then pass "remote_writes=full + tier=off  -> 403 (the global does not widen)"
else fail "remote_writes=full + tier=off  -> $code (expected 403; the global is retired)"; fi
code=$(call POST "$STORE" "" "$store_body")
if [ "$code" = "403" ]; then pass "remote_writes=full + no token   -> 403 (the global does not authorize)"
else fail "remote_writes=full + no token   -> $code (expected 403)"; fi
code=$(call POST "$STORE" "$(mint data)" "$store_body")
if stored_ok "$code"; then pass "remote_writes=full + tier=data  -> $code (unchanged)"
else fail "remote_writes=full + tier=data  -> $code (expected 2xx)"; fi

# --- 7. UDS precedence -----------------------------------------------------
# §7/§11: the local operator is OS-attested and keeps full capability with no
# token at all. If this ever fails the local CLI is broken by the feature.
step "UDS precedence: the local operator needs no token"
if ./aimee memory store "enforcement rig uds probe" >/dev/null 2>&1; then
  pass "UDS write with no identity token -> allowed"
else
  # Not every build exposes this verb identically; fall back to the socket directly.
  sock="$AIMEE_HOME/aimee.sock"
  [ -S "$sock" ] || sock=$(ls "$AIMEE_HOME"/*.sock 2>/dev/null | head -1)
  if [ -n "$sock" ] && [ -S "$sock" ]; then
    code=$(curl -s -o /dev/null -w '%{http_code}' --unix-socket "$sock" \
             -X POST -H 'Content-Type: application/json' --data "$store_body" \
             "http://localhost$STORE")
    if is2xx "$code"; then pass "UDS write with no identity token -> $code"
    else fail "UDS write with no identity token -> $code (expected 2xx; §7 exemption)"; fi
  else
    fail "UDS write could not be attempted (no socket found)"
  fi
fi

# --- verdict ---------------------------------------------------------------
step "Result"
if [ "$FAILED" = "0" ]; then
  echo "== PASSED — a minted token's tier gates a real write, end to end"
  echo "LIVE_EXIT=0"
  exit 0
fi
echo "== FAILED — $FAILED assertion(s)"
echo "server log: $srv_log"; tail -30 "$srv_log" 2>/dev/null
echo "LIVE_EXIT=1"
exit 1
