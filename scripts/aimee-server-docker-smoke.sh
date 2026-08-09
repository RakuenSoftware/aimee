#!/usr/bin/env bash
#
# aimee-server-docker-smoke.sh — prove the full aimee-server + aimee-kb Docker
# stack spins up and that the server actually talks to the kb container.
#
# Brings up compose.server.yaml (self-contained aimee-kb + aimee-server; the kb
# embeds in-container, so there is no separate inference service),
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
#   SERVER_URL    base URL of the server /v1 API  (default https://localhost:8743)
#   KB_URL        base URL of the kb /v1 API      (default http://localhost:8741)
#   BEARER        server bearer token (required for an existing stack; generated
#                 and supplied as a first-boot secret when --up is used)
#   COMPOSE_FILE  compose file(s), space-separated (default compose.server.yaml);
#                 list several to layer an override, e.g. to remap host ports
#                 on a host where 8740/8741 are already taken
#   WAIT_SECONDS  health wait budget on --up      (default 300)
#
# Exit code: 0 = all checks passed, non-zero otherwise.

set -euo pipefail

SERVER_URL="${SERVER_URL:-https://localhost:8743}"
KB_URL="${KB_URL:-http://localhost:8741}"
BEARER="${BEARER:-}"
COMPOSE_FILE="${COMPOSE_FILE:-compose.server.yaml}"
WAIT_SECONDS="${WAIT_SECONDS:-300}"

# The kb embeds in-container from weights baked into the image: no tier to pick and
# nothing to download. Select the bundled model (the image pre-selects nothing) and
# let EMBEDDER_DIMS default from config, so the schema width and the model's
# output width come from one source rather than being pinned apart here.
: "${EMBEDDER_MODEL:=bekko-a25m}"
export EMBEDDER_MODEL
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

# Scope every fresh smoke stack explicitly. This keeps Vault bootstrap, startup,
# logs, and teardown on the same project even when other projects use this file.
if [[ "$DO_UP" == 1 && -z "${COMPOSE_PROJECT_NAME:-}" ]]; then
  export COMPOSE_PROJECT_NAME="aimee-e2e-server-$$"
fi

if [[ -z "$BEARER" ]]; then
  if [[ "$DO_UP" == 1 ]]; then
    BEARER="$(openssl rand -hex 32)"
  else
    echo "BEARER is required when testing an already-running stack" >&2
    exit 2
  fi
fi
if [[ "$DO_UP" == 1 ]]; then
  export AIMEE_API_BEARER_TOKEN="$BEARER"
fi

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
  # -k: the server's /v1 TLS uses an auto-provisioned self-signed cert (harmless for the kb's http URL).
  if body="$(curl -fsS -k --max-time 20 "${AUTH[@]}" "$@" 2>/dev/null)" && [[ "$body" == *"$expect"* ]]; then
    green "  PASS  $name"
    PASS=$((PASS + 1))
  else
    red   "  FAIL  $name"
    printf '        expected substring: %s\n' "$expect"
    printf '        got: %s\n' "${body:-<no response / curl error>}"
    FAIL=$((FAIL + 1))
  fi
}

check_absent() {
  # check_absent <name> <substring-that-must-NOT-appear> <curl-args...>
  # A field that must stay OFF the wire needs its own assertion: check() only
  # proves presence, and "no verdict was emitted" is exactly what distinguishes
  # "you did not ask" from "the answer is no".
  local name="$1" forbidden="$2"; shift 2
  local body
  if ! body="$(curl -fsS -k --max-time 20 "${AUTH[@]}" "$@" 2>/dev/null)"; then
    red "  FAIL  $name (no response / curl error)"
    FAIL=$((FAIL + 1))
  elif [[ "$body" == *"$forbidden"* ]]; then
    red   "  FAIL  $name"
    printf '        must NOT contain: %s\n' "$forbidden"
    printf '        got: %s\n' "$body"
    FAIL=$((FAIL + 1))
  else
    green "  PASS  $name"
    PASS=$((PASS + 1))
  fi
}

check_status() {
  # check_status <name> <expected-http-code> <curl-args...>  (no auth header added)
  local name="$1" want="$2"; shift 2
  local code
  code="$(curl -s -k -o /dev/null -w '%{http_code}' --max-time 15 "$@" 2>/dev/null || true)"
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
  bold "==> Building + Vault-bootstrapping + starting the full stack ($COMPOSE_FILE)"
  if [[ "${AIMEE_E2E_SKIP_BUILD:-0}" != "1" ]]; then
    "${DC[@]}" build
  else
    bold "==> Using prebuilt topology images (AIMEE_E2E_SKIP_BUILD=1)"
  fi
  # Port-remap overrides do not affect the persistent Vault volumes. Bootstrap
  # both owners against the base file before creating either long-lived service.
  bootstrap_compose="${COMPOSE_FILE%% *}"
  scripts/aimee-compose-vault-bootstrap.sh -f "$bootstrap_compose" all
  "${DC[@]}" up -d --no-build

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
                                            -d '{"query":"docker smoke test","scope":"all","max_results":3}' \
                                            "${SERVER_URL}/v1/kb/search"

# `memory get --as-of` is a FIELD that has to survive this same hop, and it did
# not: the server read only the id, so the timestamp was marshalled, sent, and
# dropped here, and the client printed the row with no verdict -- indistinguishable
# from "not in force". Unit tests at both ends passed throughout, because each was
# checked against a payload hand-written to contain the field. Only a real
# cross-container call catches it, so the assertion belongs in this job.
#
# The row is seeded through the KB'S OWN API rather than the server's, because
# data-plane writes are refused on the server's TCP listener at the default
# aimee.api.remote_writes=off (server_http_authz.c). Seeding direct keeps this
# stack exactly as shipped -- turning remote writes on would test a topology
# nobody deploys. The READ still crosses server -> kb, which is the hop at issue.
#
# No `-f`: an HTTP error must leave the body readable so a failure here is
# diagnosable, and the assignment is guarded so a non-zero curl cannot trip
# `set -e` and kill the run between checks with no message.
as_of_seed="$(curl -sS -k --max-time 20 "${AUTH[@]}" -X POST -H 'content-type: application/json' \
  -d '{"key":"e2e-as-of","content":"as-of smoke value","tier":"L0","kind":"fact"}' \
  "${KB_URL}/v1/actions/memory.store" 2>&1 || true)"
mem_id="$(printf '%s' "$as_of_seed" | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1)"
if [[ -n "$mem_id" ]]; then
  check "POST /v1/memory/get --as-of -> kb echoes the timestamp" '"as_of"' \
        -X POST -H 'content-type: application/json' \
        -d "{\"id\":${mem_id},\"as_of\":\"2020-01-01T00:00:00Z\"}" "${SERVER_URL}/v1/memory/get"
  check "POST /v1/memory/get --as-of -> kb returns a verdict" '"valid_at"' \
        -X POST -H 'content-type: application/json' \
        -d "{\"id\":${mem_id},\"as_of\":\"2020-01-01T00:00:00Z\"}" "${SERVER_URL}/v1/memory/get"
  check_absent "POST /v1/memory/get without --as-of emits no verdict" '"valid_at"' \
        -X POST -H 'content-type: application/json' \
        -d "{\"id\":${mem_id}}" "${SERVER_URL}/v1/memory/get"
else
  red "  FAIL  kb memory.store did not return an id (cannot check --as-of)"
  printf '        got: %s\n' "${as_of_seed:-<no response>}"
  FAIL=$((FAIL + 1))
fi

bold "==> Auth is enforced"
check_status "GET /v1/health (no bearer) rejected" 401 "${SERVER_URL}/v1/health"

bold "==> Sanity: kb container is directly healthy at ${KB_URL}"
check "GET /v1/health (kb direct)" '"status"' "${KB_URL}/v1/health"

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-server is up and talking to the aimee-kb container."
