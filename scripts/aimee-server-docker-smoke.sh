#!/usr/bin/env bash
#
# aimee-server-docker-smoke.sh — prove the full aimee-server + aimee-kb Docker
# stack spins up and that the server actually talks to the kb container.
#
# Brings up compose.server.yaml (postgres + embedder + aimee-kb + aimee-server),
# waits for aimee-server to report healthy, then exercises the server /v1 API —
# including the two endpoints that PROXY THROUGH to the kb over HTTP
# (AIMEE_KB_API_URL), which is the cross-container wiring under test:
#
#   1. GET  /v1/health        — server is up (server-native)
#   2. GET  /v1/version       — server identifies its build
#   3. GET  /v1/kb/status     — server -> kb /v1/health?status=1 (DB2 + pgvector)
#   4. POST /v1/kb/search      — server -> kb /v1/search (query -> embed -> pgvector)
#   5. auth enforcement        — /v1/health with no bearer must be rejected (401)
#   6. kb direct on :8741      — sanity that the kb container itself is healthy
#
# Checks 3 and 4 only pass if the server reached the kb container, so a green
# run proves the AIMEE_KB_API_URL wiring end to end.
#
# Usage:
#   scripts/aimee-server-docker-smoke.sh             # assume stack already up
#   scripts/aimee-server-docker-smoke.sh --up        # build + bring the stack up
#   scripts/aimee-server-docker-smoke.sh --up --down # also tear down after
#
# Env:
#   SERVER_URL    base URL of the server /v1 API  (default http://localhost:8740)
#   KB_URL        base URL of the kb /v1 API      (default http://localhost:8741)
#   BEARER        server bearer token             (default aimee-local-dev)
#   COMPOSE_FILE  compose file(s), space-separated (default compose.server.yaml);
#                 list several to layer an override, e.g. to remap host ports
#                 on a host where 8740/8741 are already taken
#   WAIT_SECONDS  health wait budget on --up      (default 300)
#
# Exit code: 0 = all checks passed, non-zero otherwise.

set -euo pipefail

SERVER_URL="${SERVER_URL:-http://localhost:8740}"
KB_URL="${KB_URL:-http://localhost:8741}"
BEARER="${BEARER:-aimee-local-dev}"
COMPOSE_FILE="${COMPOSE_FILE:-compose.server.yaml}"
WAIT_SECONDS="${WAIT_SECONDS:-300}"

# Smoke tests validate the retrieval pipeline, not a specific model. Default to
# the light 0.6b embedder (1024-dim) so `up --build` is fast and fits CI runners;
# the shipped default is the 4b (2560-dim). Pre-set either var to override.
: "${EMBEDDER_MODEL:=perplexity-ai/pplx-embed-v1-0.6b}"
: "${AIMEE_EMBEDDING_DIM:=1024}"
export EMBEDDER_MODEL AIMEE_EMBEDDING_DIM
DO_UP=0
DO_DOWN=0

for arg in "$@"; do
  case "$arg" in
    --up)   DO_UP=1 ;;
    --down) DO_DOWN=1 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

# COMPOSE_FILE may name several files (space-separated) so an override can
# layer on top of the base stack (e.g. remap published ports).
DC=(docker compose)
for f in $COMPOSE_FILE; do DC+=(-f "$f"); done
AUTH=(-H "Authorization: Bearer ${BEARER}")
PASS=0
FAIL=0

check() {
  # check <name> <expected-substring> <curl-args...>
  local name="$1" expect="$2"; shift 2
  local body
  if body="$(curl -fsS --max-time 20 "${AUTH[@]}" "$@" 2>/dev/null)" && [[ "$body" == *"$expect"* ]]; then
    green "  PASS  $name"
    PASS=$((PASS + 1))
  else
    red   "  FAIL  $name"
    printf '        expected substring: %s\n' "$expect"
    printf '        got: %s\n' "${body:-<no response / curl error>}"
    FAIL=$((FAIL + 1))
  fi
}

check_status() {
  # check_status <name> <expected-http-code> <curl-args...>  (no auth header added)
  local name="$1" want="$2"; shift 2
  local code
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 15 "$@" 2>/dev/null || true)"
  if [[ "$code" == "$want" ]]; then
    green "  PASS  $name (HTTP $code)"
    PASS=$((PASS + 1))
  else
    red   "  FAIL  $name (got HTTP ${code:-none}, want $want)"
    FAIL=$((FAIL + 1))
  fi
}

cleanup() {
  if [[ "$DO_DOWN" == 1 ]]; then
    bold "==> Tearing down the stack (--down)"
    "${DC[@]}" down -v || true
  fi
}
trap cleanup EXIT

if [[ "$DO_UP" == 1 ]]; then
  bold "==> Building + starting the full stack ($COMPOSE_FILE)"
  "${DC[@]}" up -d --build

  bold "==> Waiting up to ${WAIT_SECONDS}s for aimee-server to report healthy"
  deadline=$((SECONDS + WAIT_SECONDS))
  while true; do
    state="$("${DC[@]}" ps --format '{{.Service}} {{.Health}}' 2>/dev/null | awk '$1=="aimee-server"{print $2}')"
    [[ "$state" == "healthy" ]] && { green "    aimee-server is healthy"; break; }
    if (( SECONDS >= deadline )); then
      red "    aimee-server did not become healthy within ${WAIT_SECONDS}s (state: ${state:-unknown})"
      "${DC[@]}" ps
      "${DC[@]}" logs --tail=40 aimee-server || true
      exit 1
    fi
    sleep 3
  done
fi

bold "==> Server-native surface at ${SERVER_URL}"
check "GET /v1/health"   '"service":"aimee-server"' "${SERVER_URL}/v1/health"
check "GET /v1/version"  'version'                  "${SERVER_URL}/v1/version"

bold "==> Cross-container: server -> kb (proves AIMEE_KB_API_URL wiring)"
# /v1/kb/status relays the kb's /v1/health?status=1 verbatim; "vector" only
# appears when the server reached the kb and the kb queried its pgvector store.
check "GET /v1/kb/status -> kb"  '"vector"'  "${SERVER_URL}/v1/kb/status"
# /v1/kb/search proxies to the kb's ranked search (query -> embed -> pgvector).
check "POST /v1/kb/search -> kb" '"hits"'   -X POST -H 'content-type: application/json' \
                                            -d '{"query":"docker smoke test","max_results":3}' \
                                            "${SERVER_URL}/v1/kb/search"

bold "==> Auth is enforced"
check_status "GET /v1/health (no bearer) rejected" 401 "${SERVER_URL}/v1/health"

bold "==> Sanity: kb container is directly healthy at ${KB_URL}"
check "GET /v1/health (kb direct)" '"status"' "${KB_URL}/v1/health"

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-server is up and talking to the aimee-kb container."
