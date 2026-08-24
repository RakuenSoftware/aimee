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
# pinned by a real root-owned 0644 trust bundle, and real RS256 tokens. Nothing is
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
# requires the bundle to be a root-owned, single-link regular file with no
# group/world write bits. The public bundle is 0644 so a non-root container server
# can read it; the loader still refuses an untrusted owner or writable file.
#
# Usage: run-write-tier-enforce-live.sh [--keep] [postgres://superuser@host:port/db]
#   With no URL it uses the local cluster via `su postgres` (peer auth).
set -uo pipefail
export LC_ALL=C

# Uses the shared environment in scripts/lib/aimee-live-env.sh for the parts every
# live rig needs — the two ways in to Postgres, the disposable database, aimee-kb,
# and an aimee-server with a TCP listener. The trust chain below stays here: it is
# what THIS rig is about, not boilerplate.
LIVE_KB_PORT=18841
LIVE_SRV_PORT=18843
LIVE_SERVER_ID="enforce-srv"
LIVE_KB_BEARER="enforce-kb-token"
LIVE_SRV_BEARER="enforce-tcp-bearer"

. "$(cd "$(dirname "$0")" && pwd)/lib/aimee-live-env.sh"

BUNDLE=/root/aimee-enforce-trust-$$.pem
# The trust bundle is this rig's own state. It goes through the extra-cleanup hook
# because the helper installs the only EXIT trap.
LIVE_EXTRA_CLEANUP='rm -f "$BUNDLE"'

live_env_init "enforce" "$@"

for b in ./aimee ./aimee-server ./aimee-kb ./write-tier-enforce-live; do
  [ -x "$b" ] || { echo "enforce-live: $b not built" >&2; exit 2; }
done
# kb FORKS this beside its own executable; a missing resolver fails later as
# something else entirely. Learned on this branch's first real CI run.
[ -x ./aimee-kb-resolver ] || { echo "enforce-live: ./aimee-kb-resolver not built" >&2; exit 2; }

live_env_pg_create

# The names this rig's body already uses, mapped onto the shared environment, so
# the assertions below read exactly as they did before the migration.
work="$LIVE_WORK"
db="$LIVE_DB"
owner="$LIVE_OWNER"
kb_log="$LIVE_KB_LOG"; srv_log="$LIVE_SRV_LOG"
KB_PORT="$LIVE_KB_PORT"
SRV_TCP_PORT="$LIVE_SRV_PORT"
SERVER_ID="$LIVE_SERVER_ID"
TEAM_ID="$LIVE_TEAM"
SRV_BEARER="$LIVE_SRV_BEARER"
KB_BEARER="$LIVE_KB_BEARER"
TOKEN_KEY="$work/token.pem"
pg_indb()     { pg_db -c "$1"; }
pg_indb_val() { pg_val "$1"; }

# --- kb --------------------------------------------------------------------
# The boot recipe (config shape, TCP DSN, health poll) is the shared one; only the
# trust chain above is this rig's own.
live_env_start_kb

# --- server ----------------------------------------------------------------
# AIMEE_SERVER_ID is the token AUDIENCE and AIMEE_SERVER_TEAM_ID the single team
# this server is enrolled in; both are read by server_write_tier_db1's
# build_config, and either being unset denies every token for a reason that has
# nothing to do with the token.
export AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE="$BUNDLE"
live_env_start_server

# --- the management trust chain -------------------------------------------
# AFTER the server, which is what changed. This used to be seeded before it,
# into a SQLite file ($AIMEE_HOME/aimee.db) the server would later open
# directly. The store is a Go module now and is reached over the bus, and the
# bus socket is the SERVER's -- so the store cannot be running until the server
# is, and this step cannot run before either of them. Seeding it earlier is what
# produced "DB1 mgmt jwks is unreachable" followed by "the store refused the
# signed JWKS envelope": nothing had refused anything, there was no store yet.
#
# Still before the first token is minted, which is the ordering that actually
# matters. The server reads the envelope at REQUEST time, so what must hold is
# that it is stored before a request needs it -- an absent one denies every
# token as INVALID, which looks identical to a forged token and would make this
# rig "pass" for the wrong reason.
#
# The server logs one startup error about the trust bundle being unreadable,
# and it is accurate: the file does not exist yet, and provision writes it a
# second later. Nothing consumes the bundle until a request does, and the
# management surface validates only the SHAPE of the path at startup.
#
# One provision call, not two. cmd_provision generates a fresh RSA authority key
# every time it runs, so splitting it into "write the bundle" and "seed the
# store" would leave the bundle pinning a key the stored envelope does not
# carry.
step "Provisioning the JWKS trust chain (real envelope, real signature)"
kid=$(./write-tier-enforce-live provision \
        --bundle "$BUNDLE" --key "$TOKEN_KEY") \
  || { echo "enforce-live: trust chain provisioning failed" >&2; exit 2; }
echo "${kid}   trust bundle $BUNDLE (root-owned 0644)"


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
# --no-store is what the throwaway --db1 "$work/other.db" was really buying: a
# key the server has NEVER been taught. Seeding it into the shared store would
# teach the server this key and invert the assertion below.
./write-tier-enforce-live provision --no-store --bundle "$work/other.pem" --key "$work/other-key.pem" >/dev/null 2>&1
deny "signed by a foreign key" "$(./write-tier-enforce-live mint --key "$work/other-key.pem" --aud "$SERVER_ID" --team $TEAM_ID --sub 'oidc:test:alice' --tier full --jti enf-$$-key)"
# The mutation must be GUARANTEED to change the token. `sed 's/.$/A/'` did not:
# when the signature already ended in 'A' it rewrote 'A' as 'A', left the token
# byte-identical, and the server correctly accepted it -- so the rig reported
# "tampered signature -> 200 (expected a refusal)" and a green build failed
# claiming the signature check was broken. Base64url gives that a ~1-in-64 chance
# per run; observed on CI. Flip the last character to a DIFFERENT one instead.
orig_tok=$(mint full)
case "$orig_tok" in
  *A) tampered="${orig_tok%?}B" ;;
  *)  tampered="${orig_tok%?}A" ;;
esac
if [ "$tampered" = "$orig_tok" ]; then
  fail "tampered signature: the mutation left the token unchanged, so this proves nothing"
else
  deny "tampered signature    " "$tampered"
fi

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
# The Vault migration canonicalizes aimee.yaml before this point and omits a
# default-valued `remote_writes: off` line. A sed replacement therefore became
# a silent no-op while the test claimed it had flipped the setting. Use the
# documented deployment override so the live server is unambiguously at full;
# config parsing/round-tripping is covered by unit-test-config.
export AIMEE_API_REMOTE_WRITES=full
live_env_restart_server full
sock=$(ls "$AIMEE_HOME"/*.sock 2>/dev/null | head -1)
curl -s --unix-socket "$sock" "http://localhost/v1/api/status" \
  -o "$work/pre-refusal-status" 2>/dev/null
if ! grep -q 'aimee.api.remote_writes NO LONGER AUTHORIZES' \
      "$work/pre-refusal-status" 2>/dev/null; then
  fail "running server did not start with AIMEE_API_REMOTE_WRITES=full"
fi
code=$(call POST "$STORE" "$(mint off)" "$store_body")
if [ "$code" = "403" ]; then pass "remote_writes=full + tier=off  -> 403 (the global does not widen)"
else fail "remote_writes=full + tier=off  -> $code (expected 403; the global is retired)"; fi
code=$(call POST "$STORE" "" "$store_body")
if [ "$code" = "403" ]; then pass "remote_writes=full + no token   -> 403 (the global does not authorize)"
else fail "remote_writes=full + no token   -> $code (expected 403)"; fi
code=$(call POST "$STORE" "$(mint data)" "$store_body")
if stored_ok "$code"; then pass "remote_writes=full + tier=data  -> $code (unchanged)"
else fail "remote_writes=full + tier=data  -> $code (expected 2xx)"; fi

# §11 also requires the OBSERVABILITY half: "the startup warning + global_ignored
# metric fire when it is non-default". Nothing tested that, and a metric nobody
# reads is how an operator ends up inferring a cutover from user complaints. The
# refusals just above are exactly the condition that increments it -- requests the
# retired global WOULD have allowed -- so the counter must now be non-zero.
# Read over UDS: /v1/api/status is a read, and this asserts the operator-visible
# surface rather than a counter reachable only from inside the process.
if [ -n "$sock" ] && [ -S "$sock" ]; then
  curl -s --unix-socket "$sock" "http://localhost/v1/api/status" -o "$work/apistatus" 2>/dev/null
  if grep -q 'global_ignored' "$work/apistatus" 2>/dev/null; then
    n=$(sed -n 's/.*NO LONGER AUTHORIZES; \([0-9]*\) request.*/\1/p' "$work/apistatus" | head -1)
    pass "remote_writes.global_ignored reported to the operator (${n:-?} request(s) refused)"
  else
    fail "global_ignored is absent from /v1/api/status after refusals the retired global would have allowed"
  fi
else
  fail "could not reach the unix socket to read /v1/api/status"
fi

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

# --- 8. revocation lag, measured across a real TTL boundary ----------------
# §11: "after kb revokes a subject's grant, the next call past the documented lag
# is denied", the lag being bounded by ONE TOKEN TTL.
#
# The residual rig proves one half — kb refuses to MINT for a revoked subject the
# moment the grant is gone. That half says nothing about tokens already in the
# wild, and an earlier revision inferred the rest from "the request path never
# reads a grant" rather than measuring it. Inference is not measurement, and this
# is the rig with a real signing key and a real trust bundle, so the measurement
# belongs here.
#
# THREE tokens sharing one short TTL, because the jti is single-use: reusing one
# token would be refused as a REPLAY and the rig would credit revocation for a
# refusal it did not cause.
#
# ORDER MATTERS, and getting it wrong makes this section prove nothing. The mint
# tool signs directly and never consults a grant, so tokens minted BEFORE the
# grant exists have no relationship to the authorization being revoked: the
# sequence would then show independently-signed tokens surviving an unrelated
# database write and later expiring, which is a statement about expiry, not about
# revocation. The grant is created and ASSERTED first, and only then are the
# tokens issued under it.
step "Revocation lag is bounded by one token TTL"
REV_TTL=20
rev_sub="oidc:test:revsubject"

# The rest of this rig deliberately runs with NO grant rows — that is how it shows
# the request path never consults one. This group is the exception and needs the
# rows a grant lives among: the team, the subject's membership, and an admin who
# may grant. Seeded HERE rather than in the rig's setup so the earlier sections
# keep their "no grant row anywhere in the database" property.
pg_indb "INSERT INTO kb_team(id,name) VALUES ($TEAM_ID,'enforce') ON CONFLICT DO NOTHING" >/dev/null 2>&1
pg_indb "INSERT INTO kb_team_membership(identity_key,team) VALUES ('$rev_sub',$TEAM_ID),('owner',$TEAM_ID) ON CONFLICT DO NOTHING" >/dev/null 2>&1
pg_indb "INSERT INTO kb_admin_grant(identity_key,granted_by) VALUES ('owner','owner') ON CONFLICT DO NOTHING" >/dev/null 2>&1
# kb_write_tier_grant has a composite FK onto kb_server_registry(server_id,team_id):
# a grant cannot name a server the registry has never heard of. Nothing else in
# this rig registers one, because nothing else here needs a grant at all.
pg_indb "INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
                                        mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
         VALUES ('$SERVER_ID','cn','mcn',$TEAM_ID,'https://$SERVER_ID','active',
                 'CN=ca','01',repeat('c',64))
         ON CONFLICT DO NOTHING" >/dev/null 2>&1

# grant_set/grant_revoke take the actor from aimee.principal rather than an
# argument, so an audit trail cannot be written to order; both must run scoped.
rev_scoped() { pg_indb_val "BEGIN; SET LOCAL aimee.principal='owner'; SET LOCAL aimee.team='$TEAM_ID'; $1; COMMIT;"; }
rev_set_out=$(rev_scoped "SELECT kb_write_tier_grant_set('$SERVER_ID',$TEAM_ID,'$rev_sub','data','owner')" 2>&1)
rev_live=$(pg_indb_val "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='$SERVER_ID' AND team_id=$TEAM_ID AND subject='$rev_sub' AND revoked_at IS NULL" | tr -d ' ')
if [ "${rev_live:-0}" = "1" ]; then
  pass "seeded a live grant for $rev_sub"
else
  # Without a grant there is nothing to revoke, and the sequence below would
  # measure EXPIRY while reporting revocation — so say why, not just that.
  fail "could not seed the grant for $rev_sub (live=${rev_live:-?}); without it the assertions \
below would prove expiry, not revocation. psql said: $(printf '%s' "$rev_set_out" | grep -m1 '^ERROR:' | head -c200)"
fi

# ONLY NOW are the tokens issued — under an authorization that demonstrably exists.
# Minting them earlier would have made every assertion below a statement about
# expiry, since the mint tool never consults a grant.
rev_now=$(date +%s)
rev_exp=$((rev_now + REV_TTL))
rev_mint() { # rev_mint -> a token for rev_sub expiring at rev_exp
  local jti="enf-$$-rev-$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  ./write-tier-enforce-live mint --key "$TOKEN_KEY" --aud "$SERVER_ID" \
    --team "$TEAM_ID" --sub "$rev_sub" --tier data --jti "$jti" \
    --iat "$rev_now" --exp "$rev_exp"
}
t_pre=$(rev_mint); t_mid=$(rev_mint); t_post=$(rev_mint)

code=$(call POST "$STORE" "$t_pre" "$store_body")
if stored_ok "$code"; then pass "before revoke: a granted subject's token writes -> $code"
else fail "before revoke: expected a stored 2xx, got $code — the rest of this group would be meaningless"; fi

rev_scoped "SELECT kb_write_tier_grant_revoke('$SERVER_ID',$TEAM_ID,'$rev_sub')" >/dev/null 2>&1
rev_after=$(pg_indb_val "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='$SERVER_ID' AND team_id=$TEAM_ID AND subject='$rev_sub' AND revoked_at IS NULL" | tr -d ' ')
if [ "${rev_after:-x}" = "0" ]; then pass "revoke stamped revoked_at (0 live grants remain)"
else fail "revoke left ${rev_after:-?} live grant(s); the boundary below would not be a revocation boundary"; fi

# THE LAG ITSELF: a token issued before the revoke keeps working after it. This is
# the property that makes the lag exactly one TTL rather than instantaneous, and
# it is a real measurement rather than an argument about the request path.
code=$(call POST "$STORE" "$t_mid" "$store_body")
if stored_ok "$code"; then pass "during the lag: an already-issued token still writes -> $code (the lag is real)"
else fail "during the lag: an already-issued token was refused ($code) — then the lag is not one TTL"; fi

# THE BOUNDARY: past exp, the first request is denied. Waiting the remainder of
# the TTL is what makes this a measurement of the bound and not of expiry alone.
sleep_for=$((rev_exp - $(date +%s) + 2))
[ "$sleep_for" -gt 0 ] && sleep "$sleep_for"
code=$(call POST "$STORE" "$t_post" "$store_body")
if [ "$code" = "401" ] || [ "$code" = "403" ]; then
  pass "past the TTL boundary: the first request with a pre-revoke token is denied -> $code"
else
  fail "past the TTL boundary a pre-revoke token still worked ($code): the lag is NOT bounded by the TTL"
fi

# --- verdict ---------------------------------------------------------------
live_env_verdict "a minted token's tier gates a real write, end to end"
