#!/usr/bin/env bash
#
# aimee-write-read-e2e.sh — prove a real WRITE→READ round-trip against a running
# aimee-server, not just readiness. Stores a memory with a unique sentinel, then
# reads it back two ways and asserts the content survives the round-trip:
#
#   1. POST /v1/memory/store   — write a fact (id returned)
#   2. POST /v1/memory/list    — the stored content + key come back verbatim
#   3. POST /v1/memory/search  — keyword retrieval surfaces the same fact
#
# This exercises the server→kb mutation path + DB2 persistence + retrieval, the
# core of aimee that a deploy-only smoke never touches.
#
# PREREQUISITE: this direct-curl harness runs as the first wizard user and must
# present the client certificate enrolled by `aimee remote set`. A bearer by
# itself is deliberately read-only, and `aimee.api.remote_writes` cannot widen it.
# Authority-managed identity-token coverage lives in run-write-tier-enforce-live.sh.
#
# Env: SERVER_URL (default https://localhost:8743), BEARER (required),
#      CLIENT_CERT and CLIENT_KEY (the enrolled PEM files; both or neither).
# Exit code: 0 = the sentinel round-tripped through store→list→search.

set -uo pipefail

SERVER_URL="${SERVER_URL:-https://localhost:8743}"
BEARER="${BEARER:-}"
if [[ -z "$BEARER" ]]; then
  echo "BEARER is required" >&2
  exit 2
fi
AUTH=(-H "Authorization: Bearer ${BEARER}")
JSON=(-H 'content-type: application/json')
IDENTITY=()
if [[ -n "${CLIENT_CERT:-}" || -n "${CLIENT_KEY:-}" ]]; then
  if [[ ! -r "${CLIENT_CERT:-}" || ! -r "${CLIENT_KEY:-}" ]]; then
    echo "CLIENT_CERT and CLIENT_KEY must both name readable enrolled PEM files" >&2
    exit 2
  fi
  IDENTITY=(--cert "$CLIENT_CERT" --key "$CLIENT_KEY")
fi

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

# Unique sentinel so reruns against a persistent server never collide and a
# stale row can't produce a false pass.
SENT="aimeeE2E${$}x${RANDOM}zz"
KEY="e2e write-read ${SENT}"
CONTENT="The E2E canary token is ${SENT} and it tastes of orange marmalade"

PASS=0; FAIL=0
ok()  { green "  PASS  $*"; PASS=$((PASS + 1)); }
bad() { red   "  FAIL  $*"; FAIL=$((FAIL + 1)); }

bold "==> Write→read round-trip at ${SERVER_URL} (sentinel ${SENT})"

# 1) STORE -----------------------------------------------------------------
store_body="$(curl -s -k "${IDENTITY[@]}" "${AUTH[@]}" "${JSON[@]}" -X POST \
  -d "{\"key\":\"${KEY}\",\"content\":\"${CONTENT}\",\"kind\":\"fact\"}" \
  "${SERVER_URL}/v1/memory/store" 2>/dev/null)"
if [[ "$store_body" == *'"status":"ok"'* && "$store_body" == *'"id"'* ]]; then
  ok "store accepted ($store_body)"
elif [[ "$store_body" == *forbidden* || "$store_body" == *"remote write"* || "$store_body" == *cap* ]]; then
  bad "store REFUSED — complete the first-user mTLS enrollment or configure a per-user grant: $store_body"
  echo; bold "==> Summary: ${PASS} passed, ${FAIL} failed"; exit 1
else
  bad "store unexpected response: ${store_body:-<none>}"
fi

# 2) LIST reads it back verbatim ------------------------------------------
list_body="$(curl -s -k "${IDENTITY[@]}" "${AUTH[@]}" "${JSON[@]}" -X POST -d '{"limit":50}' \
  "${SERVER_URL}/v1/memory/list" 2>/dev/null)"
if [[ "$list_body" == *"$SENT"* ]]; then
  ok "list returns the stored content (round-trip persisted)"
else
  bad "stored sentinel NOT found in memory/list: ${list_body:0:300}"
fi

# 3) SEARCH retrieves it by keyword ---------------------------------------
search_body="$(curl -s -k "${IDENTITY[@]}" "${AUTH[@]}" "${JSON[@]}" -X POST \
  -d "{\"keywords\":[\"${SENT}\"],\"limit\":10}" \
  "${SERVER_URL}/v1/memory/search" 2>/dev/null)"
if [[ "$search_body" == *"$SENT"* ]]; then
  ok "keyword search surfaces the stored fact"
else
  bad "stored sentinel NOT found in memory/search facts: ${search_body:0:300}"
fi

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "write→read round-trip verified: a stored memory was read back by list AND search."
