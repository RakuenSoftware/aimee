#!/usr/bin/env bash
#
# aimee-combined-docker-smoke.sh — prove the COMBINED aimee-server+kb image
# (compose.combined.yaml / Dockerfile.combined) boots and that the server reaches
# the kb that runs CO-LOCATED IN THE SAME CONTAINER.
#
# Unlike the split stack (compose.server.yaml, separate kb container), here one
# `aimee-server-kb` container runs both binaries: the kb on loopback :8741 and
# the server fronting :8740 with AIMEE_KB_API_URL=http://127.0.0.1:8741. The
# server->kb checks therefore prove the in-container co-location wiring.
#
# Exercises (T4 in docs/proposals/pending/aimee-e2e-deploy-matrix.md):
#   1. GET  /v1/health           — server up (server-native)
#   2. GET  /v1/version          — build identifies itself
#   3. GET  /v1/kb/status -> kb  — server reached the in-container kb (vector)
#   4. POST /v1/kb/search -> kb  — query -> embed -> pgvector through the server
#   5. GET  /v1/health (no bearer) rejected (401)
#   6. kb /v1/health (published :8741) — the in-container kb is directly healthy
#
# Usage / Env mirror aimee-server-docker-smoke.sh.
#   scripts/aimee-combined-docker-smoke.sh --up --down
#
# Exit code: 0 = all checks passed.

set -euo pipefail

SERVER_URL="${SERVER_URL:-http://localhost:8740}"
KB_URL="${KB_URL:-http://localhost:8741}"
BEARER="${BEARER:-aimee-local-dev}"
COMPOSE_FILE="${COMPOSE_FILE:-compose.combined.yaml}"
SERVICE="${SERVICE:-aimee-server-kb}"

# Smoke tests validate the retrieval pipeline, not a specific model. Default to
# the light 0.6b embedder (1024-dim) so `up --build` is fast and fits CI runners;
# the shipped default is the 4b (2560-dim). Pre-set either var to override.
: "${EMBEDDER_MODEL:=perplexity-ai/pplx-embed-v1-0.6b}"
: "${AIMEE_EMBEDDING_DIM:=1024}"
export EMBEDDER_MODEL AIMEE_EMBEDDING_DIM
WAIT_SECONDS="${WAIT_SECONDS:-300}"
DO_UP=0
DO_DOWN=0

for arg in "$@"; do
  case "$arg" in
    --up)   DO_UP=1 ;;
    --down) DO_DOWN=1 ;;
    -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

DC=(docker compose)
for f in $COMPOSE_FILE; do DC+=(-f "$f"); done
AUTH=(-H "Authorization: Bearer ${BEARER}")
PASS=0
FAIL=0

check() {
  local name="$1" expect="$2"; shift 2
  local body
  if body="$(curl -fsS --max-time 20 "${AUTH[@]}" "$@" 2>/dev/null)" && [[ "$body" == *"$expect"* ]]; then
    green "  PASS  $name"; PASS=$((PASS + 1))
  else
    red   "  FAIL  $name"
    printf '        expected substring: %s\n        got: %s\n' "$expect" "${body:-<no response / curl error>}"
    FAIL=$((FAIL + 1))
  fi
}

check_status() {
  local name="$1" want="$2"; shift 2
  local code
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 15 "$@" 2>/dev/null || true)"
  if [[ "$code" == "$want" ]]; then
    green "  PASS  $name (HTTP $code)"; PASS=$((PASS + 1))
  else
    red   "  FAIL  $name (got HTTP ${code:-none}, want $want)"; FAIL=$((FAIL + 1))
  fi
}

cleanup() { [[ "$DO_DOWN" == 1 ]] && { bold "==> Tearing down (--down)"; "${DC[@]}" down -v || true; }; }
trap cleanup EXIT

if [[ "$DO_UP" == 1 ]]; then
  bold "==> Building + starting combined stack ($COMPOSE_FILE)"
  "${DC[@]}" up -d --build
  bold "==> Waiting up to ${WAIT_SECONDS}s for ${SERVICE} to report healthy"
  deadline=$((SECONDS + WAIT_SECONDS))
  while true; do
    state="$("${DC[@]}" ps --format '{{.Service}} {{.Health}}' 2>/dev/null | awk -v s="$SERVICE" '$1==s{print $2}')"
    [[ "$state" == "healthy" ]] && { green "    ${SERVICE} is healthy"; break; }
    if (( SECONDS >= deadline )); then
      red "    ${SERVICE} did not become healthy within ${WAIT_SECONDS}s (state: ${state:-unknown})"
      "${DC[@]}" ps; "${DC[@]}" logs --tail=60 "$SERVICE" || true; exit 1
    fi
    sleep 3
  done
fi

bold "==> Server-native surface at ${SERVER_URL}"
check "GET /v1/health"  '"service":"aimee-server"' "${SERVER_URL}/v1/health"
check "GET /v1/version" 'version'                  "${SERVER_URL}/v1/version"

bold "==> In-container co-location: server -> kb (same container)"
check "GET /v1/kb/status -> kb"  '"vector"' "${SERVER_URL}/v1/kb/status"
check "POST /v1/kb/search -> kb" '"hits"'   -X POST -H 'content-type: application/json' \
                                            -d '{"query":"docker smoke test","max_results":3}' \
                                            "${SERVER_URL}/v1/kb/search"

bold "==> Auth is enforced"
check_status "GET /v1/health (no bearer) rejected" 401 "${SERVER_URL}/v1/health"

bold "==> Sanity: in-container kb is directly healthy at ${KB_URL}"
check "GET /v1/health (kb direct)" '"status"' "${KB_URL}/v1/health"

# Optional real write→read round-trip. Requires the stack to be up with data
# writes enabled over TCP — layer compose.remote-writes.combined.yaml into
# COMPOSE_FILE (which flips aimee.api.remote_writes to "data"):
#   COMPOSE_FILE="compose.combined.yaml compose.remote-writes.combined.yaml" \
#     WRITE_READ=1 scripts/aimee-combined-docker-smoke.sh --up --down
if [[ "${WRITE_READ:-0}" == 1 ]]; then
  bold "==> Write→read round-trip (store a memory, read it back)"
  if SERVER_URL="$SERVER_URL" BEARER="$BEARER" "$(dirname "$0")/aimee-write-read-e2e.sh"; then
    green "  PASS  write→read round-trip"; PASS=$((PASS + 1))
  else
    red   "  FAIL  write→read round-trip"; FAIL=$((FAIL + 1))
  fi
fi

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-server+kb combined image is up; server and kb are co-located and talking."
