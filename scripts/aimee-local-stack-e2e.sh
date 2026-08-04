#!/usr/bin/env bash
#
# aimee-local-stack-e2e.sh — E2E for the LOCALLY-INSTALLED aimee stack (Linux).
#
# Builds the real aimee-server / aimee-kb binaries and runs them as a scratch
# instance under a throwaway AIMEE_HOME (the proven host-validation pattern:
# ulimit -s 65536 + a baked api.yaml), then exercises the live /v1 surface. This
# covers the two local topologies from scripts/e2e-matrix.sh:
#
#   --mode full    (T5) local aimee-server + local aimee-kb (needs local
#                  Postgres+pgvector at AIMEE_DB2_URL). Server reaches the kb over
#                  a local socket; proves the full self-hosted stack.
#   --mode hybrid  (T6) local aimee-server only, pointed at an EXTERNAL kb over
#                  HTTP via AIMEE_KB_API_URL (e.g. a Docker aimee-kb on :8741).
#                  Proves install.sh's "Remote kb" (kb_client_url) path.
#
# This drives the same binaries `install.sh` installs; install.sh's own
# non-interactive contract is covered by src/tests/test_install_noninteractive.sh.
#
# Env:
#   MODE          full | hybrid                 (default full; or pass --mode)
#   AIMEE_DB2_URL Postgres URL for the kb        (full mode; default
#                 postgresql://aimee@localhost/aimee_shared via local peer auth)
#   KB_URL        external kb base URL           (hybrid mode; default
#                 http://localhost:8741)
#   EMBEDDER_URL  embedder endpoint        (full mode; optional)
#   SERVER_PORT   server loopback HTTP port       (default 8740)
#   SERVER_TLS_PORT server TLS /v1 port           (default SERVER_PORT + 3)
#   BEARER        server first-boot bearer        (default random per run)
#   WAIT_SECONDS  health wait budget             (default 90)
#
# Exit code: 0 = all checks passed.

set -euo pipefail

MODE="${MODE:-full}"
SERVER_PORT="${SERVER_PORT:-8740}"
SERVER_TLS_PORT="${SERVER_TLS_PORT:-$((SERVER_PORT + 3))}"
BEARER="${BEARER:-$(openssl rand -hex 32)}"
KB_URL="${KB_URL:-http://localhost:8741}"
WAIT_SECONDS="${WAIT_SECONDS:-90}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode) MODE="$2"; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."
REPO="$(pwd)"

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
bold()   { printf '\033[1m%s\033[0m\n' "$*"; }

SERVER_URL="https://127.0.0.1:${SERVER_TLS_PORT}"
AUTH=(-H "Authorization: Bearer ${BEARER}")
IDENTITY=()
PASS=0
FAIL=0

check() {
  local name="$1" expect="$2"; shift 2
  local body
  if body="$(curl -fksS --max-time 20 "${IDENTITY[@]}" "${AUTH[@]}" "$@" 2>/dev/null)" && [[ "$body" == *"$expect"* ]]; then
    green "  PASS  $name"; PASS=$((PASS + 1))
  else
    red   "  FAIL  $name"
    printf '        expected substring: %s\n        got: %s\n' "$expect" "${body:-<no response / curl error>}"
    FAIL=$((FAIL + 1))
  fi
}

# --- build ----------------------------------------------------------------
bold "==> Building aimee client + server + kb"
make -C src ../aimee ../aimee-server ../aimee-kb >/dev/null

# --- scratch home ---------------------------------------------------------
SCRATCH="$(mktemp -d)"
export AIMEE_HOME="$SCRATCH"
mkdir -p "$AIMEE_HOME/.config/aimee"
# Server config: move both /v1 listeners to the requested scratch ports. The
# managed policy requests a client certificate and keeps the retired global
# write switch off; the wizard creates the first user's explicit grant. The
# bearer is injected only into the server's first-boot process below.
sed "s/8740/${SERVER_PORT}/; s/8743/${SERVER_TLS_PORT}/" \
    deploy/container/aimee-server.yaml > "$AIMEE_HOME/aimee.yaml"
# kb config: the baked container config points sidecar commands at the in-image
# /opt/aimee/scripts/ path; for a NATIVE local run rewrite them to the repo's
# scripts/ so the kb can actually popen embed-remote.py etc.
sed "s#/opt/aimee/scripts/#${REPO}/scripts/#g" \
    deploy/container/aimee.yaml > "$AIMEE_HOME/.config/aimee/aimee.yaml"
# Optional: point memory embedding at a REAL small embedder so the semantic
# vector path is actually exercised (see scripts/test-embedder-qwen.sh, which
# serves Qwen3-Embedding-0.6B at 1024-d). Without this the kb falls back to the
# builtin hash and the embedder-fidelity gate below reports DEGRADED. An http(s)
# URL is used directly (aimee POSTs raw text to {url}/embed).
if [[ -n "${AIMEE_E2E_EMBEDDER_URL:-}" ]]; then
  bold "==> Using real embedder for memory: ${AIMEE_E2E_EMBEDDER_URL} (dim=${EMBEDDER_DIMS:-unset})"
  # The SERVER forwards its own embedding_command to the kb on memory.store /
  # memory search (server/server_api.c); the kb also reads its own for direct
  # embedding. Set it in BOTH configs (replace an existing line, else append).
  set_embed_cmd() {  # $1 = config file
    if grep -qE '^embedding_command:' "$1"; then
      sed -i "s#^embedding_command:.*#embedding_command: \"${AIMEE_E2E_EMBEDDER_URL}\"#" "$1"
    else
      printf '\nembedding_command: "%s"\n' "${AIMEE_E2E_EMBEDDER_URL}" >> "$1"
    fi
  }
  set_embed_cmd "$AIMEE_HOME/aimee.yaml"                     # server config
  set_embed_cmd "$AIMEE_HOME/.config/aimee/aimee.yaml"      # kb config
  [[ -n "${EMBEDDER_DIMS:-}" ]] && export EMBEDDER_DIMS
fi
export AIMEE_SERVER_HTTP_BIND=1
export AIMEE_DEPLOY_ENABLED=1
export AIMEE_API_REMOTE_WRITES=off
export AIMEE_DB1_URL="sqlite://${AIMEE_HOME}/aimee.db"

kb_pid=""; server_pid=""
cleanup() {
  [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
  [[ -n "$kb_pid" ]] && kill "$kb_pid" 2>/dev/null || true
  rm -rf "$SCRATCH"
}
trap cleanup EXIT

# The server's worker threads need a 64 MB stack.
ulimit -S -s 65536 || true

if [[ "$MODE" == "full" ]]; then
  bold "==> Mode FULL (T5): local server + local kb"
  export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgresql:///aimee_shared}"
  [[ -n "${EMBEDDER_URL:-}" ]] && export EMBEDDER_URL
  export AIMEE_KB_HTTP_BIND=1
  echo "    DB2: ${AIMEE_DB2_URL}"
  # Capture kb output so the embedder-fidelity gate below can see whether pgvec
  # accepted the memory vectors or refused them on a dim mismatch.
  "$REPO/aimee-kb" --http-port=8741 >"$AIMEE_HOME/kb.log" 2>&1 &
  kb_pid=$!
  export AIMEE_KB_API_URL="http://127.0.0.1:8741"
elif [[ "$MODE" == "hybrid" ]]; then
  bold "==> Mode HYBRID (T6): local server + external kb at ${KB_URL}"
  export AIMEE_KB_API_URL="$KB_URL"
  # Sanity: the external kb must be reachable before we lean on it.
  if curl -fsS --max-time 5 "${KB_URL}/v1/health" >/dev/null 2>&1; then
    green "    external kb is reachable"
  else
    red "    external kb at ${KB_URL} is NOT reachable — start it first (e.g. compose.yaml)"; exit 1
  fi
else
  red "unknown --mode: $MODE (want full|hybrid)"; exit 2
fi

bold "==> Starting aimee-server"
AIMEE_API_BEARER_TOKEN="$BEARER" \
  "$REPO/aimee-server" --socket="$AIMEE_HOME/aimee-server.sock" &
server_pid=$!

# The enrollment claim is issued over the server's operator UDS, while the client
# uses the public TLS listener. Wait for both halves of that real wizard path.
bold "==> Waiting for the operator socket and TLS listener (up to ${WAIT_SECONDS}s)"
deadline=$((SECONDS + WAIT_SECONDS))
while true; do
  status="$(curl -sk --max-time 3 -o /dev/null -w '%{http_code}' \
    "${SERVER_URL}/v1/health" 2>/dev/null || true)"
  [[ -S "$AIMEE_HOME/aimee-server.sock" && "$status" != 000 ]] && break
  if ! kill -0 "$server_pid" 2>/dev/null; then red "    aimee-server exited during startup"; exit 1; fi
  if (( SECONDS >= deadline )); then red "    server listeners did not start within ${WAIT_SECONDS}s"; exit 1; fi
  sleep 2
done

# Exercise the same path as the setup UI: Deploy claims the immutable first
# wizard user and returns an enrollment-only bearer. The displayed `remote set`
# command then pins the leaf, generates a local key, submits its CSR, and installs
# the signed client certificate. Keep client state separate from server state.
CLIENT_HOME="$SCRATCH/client"
mkdir -p "$CLIENT_HOME"
bold "==> Claiming the first wizard user"
deploy_status="$(curl -sS --unix-socket "$AIMEE_HOME/aimee-server.sock" \
  -H 'X-Aimee-Webuser: local-stack-e2e' \
  -H 'content-type: application/json' -X POST -d '{}' -o "$SCRATCH/deploy-apply.json" \
  -w '%{http_code}' http://localhost/v1/deploy/apply)"
[[ "$deploy_status" == 200 ]] || {
  red "    Deploy returned HTTP $deploy_status"; sed -n '1,8p' "$SCRATCH/deploy-apply.json" >&2; exit 1
}
ENROLL_TOKEN="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["enrollment"]["bearer_token"])' \
  "$SCRATCH/deploy-apply.json")"
[[ ${#ENROLL_TOKEN} == 64 ]] || { red "    Deploy did not return an enrollment bearer"; exit 1; }

bold "==> Enrolling the thin client"
if ! AIMEE_HOME="$CLIENT_HOME" AIMEE_NO_CLIENT_INTEGRATIONS=1 \
     "$REPO/aimee" remote set "$SERVER_URL" "$ENROLL_TOKEN" \
     >"$SCRATCH/remote-set.out" 2>"$SCRATCH/remote-set.err"; then
  red "    remote set failed"
  sed -n '1,8p' "$SCRATCH/remote-set.err" >&2
  exit 1
fi
BEARER="$(sed -n '2p' "$CLIENT_HOME/remote.conf")"
[[ -n "$BEARER" ]] || { red "    remote set did not persist a bearer"; exit 1; }
AUTH=(-H "Authorization: Bearer ${BEARER}")
CLIENT_CERT="$CLIENT_HOME/tls/client.crt"
CLIENT_KEY="$CLIENT_HOME/tls/client.key"
[[ -s "$CLIENT_CERT" && -s "$CLIENT_KEY" ]] || {
  red "    remote set did not install the client certificate"; exit 1
}

# Before the first certificate presentation can promote the one-client roster to
# required mTLS, prove the enrollment bearer alone reaches the route gate but has
# no write authority.
bearer_only_code="$(curl -sk --max-time 10 -o "$SCRATCH/bearer-only.json" -w '%{http_code}' \
  "${AUTH[@]}" -H 'content-type: application/json' -X POST \
  -d '{"session_id":"local-stack-e2e","key":"bearer-only","value":"deny","category":"general"}' \
  "$SERVER_URL/v1/wm/set")"
[[ "$bearer_only_code" == 403 ]] || {
  red "    bearer-only write returned HTTP $bearer_only_code, expected 403"; exit 1
}
IDENTITY=(--cert "$CLIENT_CERT" --key "$CLIENT_KEY")
green "    enrolled: bound the first-user grant to mTLS; bearer-only write denied"

bold "==> Waiting for /v1/health"
if curl -fksS --max-time 5 "${IDENTITY[@]}" "${AUTH[@]}" \
     "${SERVER_URL}/v1/health" >/dev/null 2>&1; then
  green "    server is up"
else
  red "    server up but /v1/health failed with the enrolled client"; exit 1
fi

bold "==> Core contract"
check "GET /v1/health"  '"service":"aimee-server"' "${SERVER_URL}/v1/health"
check "GET /v1/version" 'version'                  "${SERVER_URL}/v1/version"

bold "==> kb-backed contract (server -> kb)"
check "GET /v1/kb/status -> vector" '"vector"' "${SERVER_URL}/v1/kb/status"
check "POST /v1/kb/search -> hits"  '"hits"'   -X POST -H 'content-type: application/json' \
                                               -d '{"query":"local e2e","max_results":3}' \
                                               "${SERVER_URL}/v1/kb/search"

bold "==> Write→read round-trip (store a memory, read it back)"
if SERVER_URL="$SERVER_URL" BEARER="$BEARER" CLIENT_CERT="$CLIENT_CERT" CLIENT_KEY="$CLIENT_KEY" \
   "$REPO/scripts/aimee-write-read-e2e.sh"; then
  green "  PASS  write→read round-trip"; PASS=$((PASS + 1))
else
  red   "  FAIL  write→read round-trip"; FAIL=$((FAIL + 1))
fi

# Embedder fidelity: the round-trip above passes on list + KEYWORD retrieval even
# when no real embedder is wired — the memory embedding silently falls back to the
# builtin hash (a vestigial 384-d stand-in) whose vectors pgvec then REFUSES on a
# dim mismatch against a corpus built by the real embedder (Qwen3-Embedding: 1024-d
# CPU / 2560-d GPU). That makes the semantic/vector path a no-op while the run still
# reports green. Surface it: if kb refused the memory vector, the semantic path was
# NOT exercised — announce it loudly, and hard-fail under AIMEE_E2E_REQUIRE_REAL_EMBEDDER=1.
bold "==> Embedder fidelity (semantic vector path)"
mm="$(grep -aoE 'memory embedding dim mismatch: got [0-9]+, expected [0-9]+' "$AIMEE_HOME/kb.log" 2>/dev/null | tail -1 || true)"
if [[ -n "$mm" ]]; then
  yellow "  DEGRADED  ${mm}; vectors refused — semantic search NOT exercised (list/keyword only)."
  yellow "            Wire a real embedder: point EMBEDDER_URL / SYNTHESIS_ENDPOINT at a"
  yellow "            Qwen3-Embedding endpoint whose dim matches the corpus (1024 CPU / 2560 GPU)."
  if [[ "${AIMEE_E2E_REQUIRE_REAL_EMBEDDER:-0}" == "1" ]]; then
    red "  FAIL  real embedder required (AIMEE_E2E_REQUIRE_REAL_EMBEDDER=1) but the run degraded to the builtin embedder"
    FAIL=$((FAIL + 1))
  fi
else
  green "  PASS  memory vectors accepted (no dim mismatch) — real semantic path exercised"
  PASS=$((PASS + 1))
fi

echo
bold "==> Summary (${MODE}): ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "local ${MODE} stack is up and serving."
