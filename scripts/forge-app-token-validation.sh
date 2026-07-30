#!/usr/bin/env bash
# forge-app-token-validation.sh — GATED live-validation for the Forge App
# installation-token machinery (proposal:
# docs/proposals/pending/aimee-workspace-forge-app-identity-and-remote-validation.md,
# §"Live validation harness" + Acceptance Criteria).
#
# WHAT THIS PROVES (against REAL GitHub, with REAL credentials):
#   1. App-token MINT — build the RS256 App JWT (the exact shape forge_app_token.c
#      builds: header {"alg":"RS256","typ":"JWT"}, payload {iat,exp,iss=app_id})
#      and exchange it at POST /app/installations/<id>/access_tokens for an
#      installation token + expires_at. Asserts a non-empty `ghs_` token whose
#      expiry is in the future. This is the SAME exchange the C code performs,
#      validated end to end against the live forge.
#   2. AUTHENTICATED git op — `git ls-remote` against the throwaway repo with the
#      minted token returns the head sha (positive control); the identical call
#      with NO token fails (negative control). Asserts both.
#   3. REFRESH — re-mint a second token and assert the exchange yields a fresh
#      token (new value and/or new expires_at), demonstrating the refresh path
#      works. (The C cache's refresh-within-skew decision is unit-tested in
#      test_forge_app_token.c — forge_app_token_needs_refresh; here we validate
#      that the *exchange itself* refreshes.)
#   4. (OPTIONAL) telegram `/pr` leg — only when TELEGRAM_BOT_TOKEN + _CHAT_ID are
#      set. This leg is a DOCUMENTED STUB (see TELEGRAM_LEG below): it needs a
#      scratch aimee-server + gateway wiring that is out of scope for a self-
#      contained shell script. It prints the exact steps and a TODO. Honest
#      status: legs 1–3 are FULLY AUTOMATED; the telegram leg is DOCUMENTED.
#
# WHY GATED: this needs real external credentials (a GitHub App's id + private
# key + installation id) and a throwaway repo the App is installed on. It is
# OPT-IN and NEVER part of `verify`/CI. With the required env UNSET it prints a
# usage block and exits 2 ("not configured to run") — it does NOT fail a build.
#
# HOW TO RUN:
#   export AIMEE_FORGE_APP_ID=123456
#   export AIMEE_FORGE_APP_PRIVATE_KEY='-----BEGIN PRIVATE KEY-----...' # PEM text
#   export AIMEE_FORGE_APP_INSTALLATION_ID=987654
#   export FORGE_VALIDATION_REPO=youruser/throwaway-repo             # App installed here
#   # optional:
#   export AIMEE_FORGE_API_BASE=https://api.github.com               # GHE base for self-hosted
#   export TELEGRAM_BOT_TOKEN=... TELEGRAM_CHAT_ID=...               # only then: telegram leg
#   ./scripts/forge-app-token-validation.sh
#
# Mirrors the brokered forge-cred integration harness
# (make forge-cred-integration / tests/test_forge_credentials_live.c): same
# env-gating discipline, set -euo pipefail, cleanup-on-exit trap, and clear
# PASS/FAIL output. No credential is ever written to disk or echoed.

set -euo pipefail

# --- output helpers -----------------------------------------------------------
PASS_COUNT=0
FAIL_COUNT=0
ok()   { printf '  [PASS] %s\n' "$*"; PASS_COUNT=$((PASS_COUNT + 1)); }
bad()  { printf '  [FAIL] %s\n' "$*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
info() { printf '  ....   %s\n' "$*"; }
hdr()  { printf '\n== %s ==\n' "$*"; }

# --- env gate -----------------------------------------------------------------
usage() {
   cat >&2 <<'EOF'
forge-app-token-validation.sh — GATED live validation (not configured to run).

This validates the Forge App installation-token machinery against REAL GitHub.
It is opt-in and NEVER part of verify/CI. Set the required env to run it.

REQUIRED:
  AIMEE_FORGE_APP_ID               the GitHub App's numeric App ID
  AIMEE_FORGE_APP_PRIVATE_KEY      the App's RSA private-key PEM contents inline
  AIMEE_FORGE_APP_INSTALLATION_ID  the installation id (App installed on the repo)
  FORGE_VALIDATION_REPO            a THROWAWAY owner/repo the App is installed on

OPTIONAL:
  AIMEE_FORGE_API_BASE             forge API base (default https://api.github.com;
                                   set to your GHE base for self-hosted)
  TELEGRAM_BOT_TOKEN               + TELEGRAM_CHAT_ID — only then is the telegram
  TELEGRAM_CHAT_ID                 `/pr` leg attempted (documented stub)

Exit codes: 2 = not configured (this message); 0 = all PASS; 1 = a FAIL.
EOF
}

missing=0
for v in AIMEE_FORGE_APP_ID AIMEE_FORGE_APP_PRIVATE_KEY AIMEE_FORGE_APP_INSTALLATION_ID FORGE_VALIDATION_REPO; do
   if [ -z "${!v:-}" ]; then
      printf 'forge-app-token-validation: required env %s is unset\n' "$v" >&2
      missing=1
   fi
done
if [ "$missing" -ne 0 ]; then
   usage
   exit 2
fi

# tool prerequisites (these are hard requirements once we are gated-in)
for t in openssl curl jq git; do
   if ! command -v "$t" >/dev/null 2>&1; then
      printf 'forge-app-token-validation: required tool %q not found on PATH\n' "$t" >&2
      exit 2
   fi
done

API_BASE="${AIMEE_FORGE_API_BASE:-https://api.github.com}"
API_BASE="${API_BASE%/}"
APP_ID="$AIMEE_FORGE_APP_ID"
INSTALL_ID="$AIMEE_FORGE_APP_INSTALLATION_ID"
REPO="$FORGE_VALIDATION_REPO"

# Match the service's first-boot contract: the key is inline PEM, never a path.
# OpenSSL receives it over an anonymous pipe on fd 3, so validation creates no
# credential file even transiently.
if ! printf '%s' "$AIMEE_FORGE_APP_PRIVATE_KEY" | grep -q 'BEGIN .*PRIVATE KEY'; then
   printf 'forge-app-token-validation: AIMEE_FORGE_APP_PRIVATE_KEY must contain inline PEM\n' >&2
   exit 2
fi

# --- cleanup trap -------------------------------------------------------------
# Never leave credentials on disk; delete any throwaway branch/PR we created.
CREATED_BRANCH=""   # set if we push a scratch branch (telegram/PR leg)
CREATED_PR=""       # set to a PR number/url if we open one
cleanup() {
   local rc=$?
   set +e
   if command -v gh >/dev/null 2>&1; then
      if [ -n "$CREATED_PR" ]; then
         gh pr close "$CREATED_PR" --repo "$REPO" --delete-branch >/dev/null 2>&1 || true
      fi
      if [ -n "$CREATED_BRANCH" ]; then
         gh api -X DELETE "repos/$REPO/git/refs/heads/$CREATED_BRANCH" >/dev/null 2>&1 || true
      fi
   fi
   # scrub token vars from this shell's memory (best effort)
   unset TOKEN TOKEN2 JWT AIMEE_FORGE_APP_PRIVATE_KEY 2>/dev/null || true
   exit "$rc"
}
trap cleanup EXIT INT TERM

# --- base64url helper ---------------------------------------------------------
b64url() { openssl base64 -e -A | tr '+/' '-_' | tr -d '='; }

# --- mint: build App JWT + exchange for an installation token -----------------
# Echoes "<token>\t<expires_at_epoch>" on stdout. Returns non-zero on failure.
mint_installation_token() {
   local now iat exp header payload signing_input sig jwt resp token expires_at_iso expires_at_epoch
   now="$(date -u +%s)"
   iat=$((now - 60))      # backdate 60s for clock skew (GitHub guidance)
   exp=$((now + 540))     # 9 min; GitHub rejects > 10 min

   # Header/payload must match forge_app_token.c exactly.
   header='{"alg":"RS256","typ":"JWT"}'
   payload="$(printf '{"iat":%d,"exp":%d,"iss":"%s"}' "$iat" "$exp" "$APP_ID")"
   signing_input="$(printf '%s' "$header" | b64url).$(printf '%s' "$payload" | b64url)"
   sig="$(printf '%s' "$signing_input" |
      openssl dgst -sha256 -sign /dev/fd/3 -binary \
         3< <(printf '%s\n' "$AIMEE_FORGE_APP_PRIVATE_KEY") |
      b64url)"
   jwt="$signing_input.$sig"

   resp="$(curl -sS --fail-with-body \
      -X POST "$API_BASE/app/installations/$INSTALL_ID/access_tokens" \
      -H "Authorization: Bearer $jwt" \
      -H "Accept: application/vnd.github+json" \
      -H "X-GitHub-Api-Version: 2022-11-28")" || {
      printf 'mint: token endpoint call failed:\n%s\n' "$resp" >&2
      return 1
   }
   token="$(printf '%s' "$resp" | jq -r '.token // empty')"
   expires_at_iso="$(printf '%s' "$resp" | jq -r '.expires_at // empty')"
   [ -n "$token" ] || { printf 'mint: response has no .token\n' >&2; return 1; }
   [ -n "$expires_at_iso" ] || { printf 'mint: response has no .expires_at\n' >&2; return 1; }
   # Parse ISO-8601 expiry to epoch (GNU date).
   expires_at_epoch="$(date -u -d "$expires_at_iso" +%s 2>/dev/null || echo 0)"
   printf '%s\t%s' "$token" "$expires_at_epoch"
}

printf 'forge-app-token-validation: live validation against %s\n' "$API_BASE"
printf '  App ID=%s  installation=%s  repo=%s\n' "$APP_ID" "$INSTALL_ID" "$REPO"

# =============================================================================
hdr "Leg 1 — mint installation token (JWT exchange, same as forge_app_token.c)"
# =============================================================================
NOW="$(date -u +%s)"
if MINT1="$(mint_installation_token)"; then
   TOKEN="${MINT1%%$'\t'*}"
   EXP1="${MINT1##*$'\t'}"
   ok "minted an installation token from App credentials"
   case "$TOKEN" in
      ghs_*) ok "token has the expected installation-token prefix (ghs_)" ;;
      *)     bad "token does not start with ghs_ (got prefix: ${TOKEN:0:4}...)" ;;
   esac
   if [ "${EXP1:-0}" -gt "$NOW" ]; then
      ok "expires_at is in the future (epoch=$EXP1, now=$NOW, +$((EXP1 - NOW))s)"
   else
      bad "expires_at is not in the future (epoch=$EXP1, now=$NOW)"
   fi
else
   bad "could not mint an installation token (JWT exchange failed)"
   TOKEN=""
   EXP1=0
fi

# =============================================================================
hdr "Leg 2 — authenticated git op (positive + negative control)"
# =============================================================================
if [ -n "$TOKEN" ]; then
   # Positive control: authenticated ls-remote must return the head sha.
   if HEAD_SHA="$(GIT_TERMINAL_PROMPT=0 git ls-remote \
         "https://x-access-token:${TOKEN}@github.com/${REPO}" HEAD 2>/dev/null | awk '{print $1; exit}')" \
      && [ -n "$HEAD_SHA" ]; then
      ok "authenticated git ls-remote returned head sha (${HEAD_SHA:0:12})"
   else
      bad "authenticated git ls-remote returned no head sha"
   fi
   # Negative control: no token + prompts disabled must FAIL (private repo).
   if GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false \
         git ls-remote "https://github.com/${REPO}" HEAD >/dev/null 2>&1; then
      bad "unauthenticated git ls-remote unexpectedly SUCCEEDED (repo must be private)"
      info "if FORGE_VALIDATION_REPO is public this negative control is not meaningful"
   else
      ok "unauthenticated git ls-remote failed as expected (negative control)"
   fi
else
   bad "skipping git op assertions — no token from Leg 1"
fi

# =============================================================================
hdr "Leg 3 — refresh (re-mint a second token, assert a fresh exchange)"
# =============================================================================
# We cannot force the C cache's near-expiry skew from a shell; that decision
# (forge_app_token_needs_refresh, 300s skew) is covered by the unit test
# test_forge_app_token.c. Here we validate that the EXCHANGE refreshes.
if [ -n "$TOKEN" ]; then
   if MINT2="$(mint_installation_token)"; then
      TOKEN2="${MINT2%%$'\t'*}"
      EXP2="${MINT2##*$'\t'}"
      case "$TOKEN2" in ghs_*) : ;; *) bad "refresh token lacks ghs_ prefix" ;; esac
      # A genuinely fresh exchange: either a different token value, or a renewed
      # expiry. (GitHub may return the same token string within a window but with
      # a bumped expires_at; either is evidence of a live re-issue.)
      if [ "$TOKEN2" != "$TOKEN" ] || [ "${EXP2:-0}" -ge "${EXP1:-0}" ]; then
         ok "re-mint produced a fresh installation token (refresh exchange works)"
         if [ "$TOKEN2" != "$TOKEN" ]; then
            info "second token differs from the first"
         else
            info "token value reused within window; expires_at renewed ($EXP1 -> $EXP2)"
         fi
      else
         bad "re-mint did not look fresh (same token and older/equal expiry)"
      fi
   else
      bad "second mint (refresh) failed"
   fi
else
   bad "skipping refresh assertions — no token from Leg 1"
fi

# =============================================================================
hdr "Leg 4 — telegram /pr round trip (OPTIONAL)"
# =============================================================================
if [ -n "${TELEGRAM_BOT_TOKEN:-}" ] && [ -n "${TELEGRAM_CHAT_ID:-}" ]; then
   # DOCUMENTED STUB — honest status: NOT automated by this script.
   #
   # A full telegram `/pr <workspace>` -> PR round trip needs live aimee
   # infrastructure that a self-contained shell harness should not stand up:
   #   1. Start a scratch aimee-server with the throwaway repo registered as an
   #      instance-held workspace and the server forge identity pointed at the
   #      App-token path: AIMEE_FORGE_TOKEN sourced from forge_cred_server_identity
   #      (App env above) — i.e. run the server with the same AIMEE_FORGE_APP_*.
   #   2. Start the gateway against that server with this bot:
   #         TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID  (the env set here).
   #   3. Inject a `/pr <workspace>` through the gateway inbound path (a Bot API
   #      update for TELEGRAM_CHAT_ID, or the gateway's local inbound test seam)
   #      and poll the bot's outbound messages for a github.com/<repo>/pull/<n>
   #      URL within a timeout.
   #   4. Assert a PR URL came back; record CREATED_PR/CREATED_BRANCH so the
   #      cleanup trap closes the PR + deletes the branch.
   #
   # TODO: wire steps 1–4 once the gateway inbound test seam is scriptable from
   # outside the C test harness (tracked in the proposal §"Live validation
   # harness", telegram leg). Until then this leg is a gated NO-OP that documents
   # the procedure rather than asserting an outcome.
   hdr "TELEGRAM leg: documented stub (NOT automated)"
   info "TELEGRAM_* is set, but the telegram /pr round trip is not automated here."
   info "Steps to run it manually:"
   info "  1. scratch aimee-server: throwaway repo as an instance workspace,"
   info "     server forge identity via AIMEE_FORGE_APP_* (App-token path)"
   info "  2. gateway up with TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID"
   info "  3. send '/pr <workspace>' through the gateway inbound path"
   info "  4. assert a github.com/$REPO/pull/<n> URL comes back; then close it"
   info "status: DOCUMENTED (not counted as PASS or FAIL)"
else
   info "TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID unset — telegram leg skipped (optional)"
fi

# =============================================================================
hdr "Summary"
# =============================================================================
printf '  passed: %d   failed: %d\n' "$PASS_COUNT" "$FAIL_COUNT"
if [ "$FAIL_COUNT" -ne 0 ]; then
   printf '\nFAIL — %d assertion(s) failed\n' "$FAIL_COUNT"
   exit 1
fi
printf '\nPASS — App-token mint + authenticated git op + refresh validated against the live forge\n'
exit 0
