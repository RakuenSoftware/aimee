#!/usr/bin/env bash
#
# aimee-server-standalone-docker-smoke.sh — prove the standalone aimee-server
# Docker image (compose.server-standalone.yaml) boots and serves its DB1-backed
# /v1 surface with NO kb wired in, and that the lazy-kb path degrades gracefully
# instead of crashing.
#
# Exercises (T3 in docs/proposals/pending/aimee-e2e-deploy-matrix.md):
#   1. GET /v1/health             — server up (server-native, SQLite DB1)
#   2. GET /v1/version            — build identifies itself
#   3. GET /v1/health (no bearer) — rejected (401): auth enforced on TCP
#   4. GET /v1/kb/status          — kb absent -> well-formed "unavailable"
#                                   (proves graceful degradation, not a crash)
#
# Usage:
#   scripts/aimee-server-standalone-docker-smoke.sh             # stack already up
#   scripts/aimee-server-standalone-docker-smoke.sh --up        # build + up first
#   scripts/aimee-server-standalone-docker-smoke.sh --up --down # tear down after
#
# Env: SERVER_URL (default https://localhost:8743), BEARER (default aimee-local-dev),
#      COMPOSE_FILE (default compose.server-standalone.yaml), WAIT_SECONDS (300).
#
# Exit code: 0 = all checks passed.

set -euo pipefail

SERVER_URL="${SERVER_URL:-https://localhost:8743}"
BEARER="${BEARER:-aimee-local-dev}"
COMPOSE_FILE="${COMPOSE_FILE:-compose.server-standalone.yaml}"
WAIT_SECONDS="${WAIT_SECONDS:-300}"
DO_UP=0
DO_DOWN=0

for arg in "$@"; do
  case "$arg" in
    --up)   DO_UP=1 ;;
    --down) DO_DOWN=1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
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
  if body="$(curl -fsS -k --max-time 20 "${AUTH[@]}" "$@" 2>/dev/null)" && [[ "$body" == *"$expect"* ]]; then
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
  code="$(curl -s -k -o /dev/null -w '%{http_code}' --max-time 15 "$@" 2>/dev/null || true)"
  if [[ "$code" == "$want" ]]; then
    green "  PASS  $name (HTTP $code)"; PASS=$((PASS + 1))
  else
    red   "  FAIL  $name (got HTTP ${code:-none}, want $want)"; FAIL=$((FAIL + 1))
  fi
}

cleanup() { [[ "$DO_DOWN" == 1 ]] && { bold "==> Tearing down (--down)"; "${DC[@]}" down -v || true; }; }
trap cleanup EXIT

if [[ "$DO_UP" == 1 ]]; then
  bold "==> Building + starting standalone server ($COMPOSE_FILE)"
  "${DC[@]}" up -d --build
  bold "==> Waiting up to ${WAIT_SECONDS}s for aimee-server to report healthy"
  deadline=$((SECONDS + WAIT_SECONDS))
  while true; do
    state="$("${DC[@]}" ps --format '{{.Service}} {{.Health}}' 2>/dev/null | awk '$1=="aimee-server"{print $2}')"
    [[ "$state" == "healthy" ]] && { green "    aimee-server is healthy"; break; }
    if (( SECONDS >= deadline )); then
      red "    aimee-server did not become healthy within ${WAIT_SECONDS}s (state: ${state:-unknown})"
      "${DC[@]}" ps; "${DC[@]}" logs --tail=40 aimee-server || true; exit 1
    fi
    sleep 3
  done
fi

# Bootstrap-bearer enrollment: the image seeds `aimee-local-dev`, which the server
# now honours ONLY for POST /v1/api/rotate_bearer. Rotate once to the strong token
# and use it for every check below (mirrors a real client's first connect).
if [[ "$BEARER" == "aimee-local-dev" ]]; then
  bold "==> Enrolling: rotate the one-time bootstrap bearer"
  rotated="$(curl -fsS -k --max-time 20 "${AUTH[@]}" -X POST "${SERVER_URL}/v1/api/rotate_bearer" -d '{}' 2>/dev/null \
             | sed -n 's/.*"bearer_token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
  if [[ -n "$rotated" ]]; then
    BEARER="$rotated"; AUTH=(-H "Authorization: Bearer ${BEARER}")
    green "    enrolled: bootstrap bearer rotated to a strong per-deployment token"
  else
    red "    FAIL  enrollment: could not rotate the bootstrap bearer"; FAIL=$((FAIL + 1))
  fi
fi

bold "==> Server-native DB1 surface at ${SERVER_URL}"
check "GET /v1/health"  '"service":"aimee-server"' "${SERVER_URL}/v1/health"
check "GET /v1/version" 'version'                  "${SERVER_URL}/v1/version"

bold "==> Auth is enforced"
check_status "GET /v1/health (no bearer) rejected" 401 "${SERVER_URL}/v1/health"

bold "==> Lazy kb degrades gracefully (no kb wired in)"
# With no kb, the server must answer /v1/kb/status with a well-formed
# "unavailable" object (available:false) — NOT hang or crash.
check "GET /v1/kb/status -> unavailable" '"available":false' "${SERVER_URL}/v1/kb/status"

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-server standalone is up and serving DB1 with the kb path degrading gracefully."
