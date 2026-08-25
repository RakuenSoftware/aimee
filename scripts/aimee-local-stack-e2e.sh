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
#   AIMEE_E2E_HOLD_SECONDS keep a green scratch stack alive for exploratory
#                 probes before cleanup           (default 0)
#   AIMEE_E2E_PROBE_SCRIPT executable invoked after the built-in checks while
#                 every service and enrolled identity remains live. The probe
#                 receives scratch paths, endpoints, credentials, and PIDs in
#                 its environment.                         (optional)
#   AIMEE_E2E_TURN_INTEGRITY_MCP=1 configures the repository's deterministic
#                 timeout MCP fixture as a server-owned client. (default 0)
#   AIMEE_E2E_RESTART_COMPONENTS=1 restart both daemons after all probes and
#                 prove enrolled/persisted service recovery.       (default 0)
#   AIMEE_E2E_KEEP_RUN_ROOT=1 retain the scratch tree after cleanup for failed
#                 live-run diagnosis; the printed path is operator-removable.
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
bold "==> Building aimee client + server + kb + required modules"
make -C src ../aimee ../aimee-server ../aimee-kb \
  build/obj/aimee-module build/obj/aimee-module-config \
  build/obj/aimee-module >/dev/null
cp src/build/obj/aimee-module src/build/obj/aimee-module-postgres
RUN_ROOT="$(mktemp -d)"
BUNDLE="$RUN_ROOT/module-bundle"
cleanup_run_root() {
  if [[ "${AIMEE_E2E_KEEP_RUN_ROOT:-0}" == "1" ]]; then
    yellow "retaining E2E scratch tree for inspection: $RUN_ROOT"
  else
    rm -rf "$RUN_ROOT"
  fi
}
trap cleanup_run_root EXIT INT TERM
python3 scripts/export_c_repositories.py \
  --runtime-bundle "$BUNDLE" >/dev/null

# --- scratch home ---------------------------------------------------------
SCRATCH="$RUN_ROOT/stack"
mkdir -p "$SCRATCH"
export AIMEE_HOME="$SCRATCH"
mkdir -p "$AIMEE_HOME/.config/aimee"
# Server config: move both /v1 listeners to the requested scratch ports. The
# managed policy requests a client certificate and keeps the retired global
# write switch off; the wizard creates the first user's explicit grant. The
# bearer is injected only into the server's first-boot process below.
sed "s/8740/${SERVER_PORT}/; s/8743/${SERVER_TLS_PORT}/" \
    deploy/container/aimee-server.yaml > "$AIMEE_HOME/aimee.yaml"
# The config provider owns one document for this installation. Append the kb's
# disjoint sections to that document rather than putting a second file at the
# retired in-process loader path. Both daemons read the same document through
# their own config-module connection.
sed "s#/opt/aimee/scripts/#${REPO}/scripts/#g" \
    deploy/container/aimee.yaml >> "$AIMEE_HOME/aimee.yaml"
chmod 0600 "$AIMEE_HOME/aimee.yaml"
if [[ "${AIMEE_E2E_TURN_INTEGRITY_MCP:-0}" == "1" ]]; then
  cat >>"$AIMEE_HOME/aimee.yaml" <<YAML

mcp_clients:
  - name: ti_remote
    transport: stdio
    install: server
    command:
      - python3
      - ${REPO}/scripts/fixtures/turn-integrity-mcp.py
YAML
fi
# Optional: point memory embedding at a REAL small embedder so the semantic
# vector path is actually exercised (see scripts/test-embedder-qwen.sh, which
# serves Qwen3-Embedding-0.6B at 1024-d). Without this the kb falls back to the
# builtin hash and the embedder-fidelity gate below reports DEGRADED. An http(s)
# URL is used directly (aimee POSTs raw text to {url}/embed).
if [[ -n "${AIMEE_E2E_EMBEDDER_URL:-}" ]]; then
  bold "==> Using real embedder for memory: ${AIMEE_E2E_EMBEDDER_URL} (dim=${EMBEDDER_DIMS:-unset})"
  # `embedding_command` is the request-side hint, while EMBEDDER_URL is the
  # daemon runtime contract used by the KB's dimension probe and asynchronous
  # memory embedding path. Supply both from the one E2E input: setting only the
  # config field lets query requests embed but leaves startup reporting
  # "no embed command configured" and stores zero corpus vectors.
  export EMBEDDER_URL="$AIMEE_E2E_EMBEDDER_URL"
  # The server forwards embedding_command to the kb on memory.store / memory
  # search, and the kb reads the same installation document for direct embedding.
  set_embed_cmd() {  # $1 = config file
    if grep -qE '^embedding_command:' "$1"; then
      sed -i "s#^embedding_command:.*#embedding_command: \"${AIMEE_E2E_EMBEDDER_URL}\"#" "$1"
    else
      printf '\nembedding_command: "%s"\n' "${AIMEE_E2E_EMBEDDER_URL}" >> "$1"
    fi
  }
  set_embed_cmd "$AIMEE_HOME/aimee.yaml"                     # server config
  [[ -n "${EMBEDDER_DIMS:-}" ]] && export EMBEDDER_DIMS
fi
export AIMEE_SERVER_HTTP_BIND=1
export AIMEE_DEPLOY_ENABLED=1
export AIMEE_API_REMOTE_WRITES=off
export AIMEE_DB1_URL="sqlite://${AIMEE_HOME}/aimee.db"

DB1_MODULE="$REPO/src/build/obj/aimee-module-aimee"
install -m0755 "$REPO/src/build/obj/aimee-module" "$DB1_MODULE"
PG_MODULE="$REPO/src/build/obj/aimee-module-postgres"
install -m0755 "$REPO/src/build/obj/aimee-module" "$PG_MODULE"
CONFIG_MODULE="$REPO/src/build/obj/aimee-module-config"
POSTGRES_MODULE="$REPO/src/build/obj/aimee-module-postgres"
MODULE_BIN_DIR="$RUN_ROOT/modules"
SERVER_POLICY="$AIMEE_HOME/modules.d/server"
KB_POLICY="$AIMEE_HOME/modules.d/kb"
mkdir -p "$MODULE_BIN_DIR" "$SERVER_POLICY" "$KB_POLICY"
sed "s|^executable=.*|executable=$DB1_MODULE|" \
  "$BUNDLE/grants/server/aimee.grant" > "$SERVER_POLICY/aimee.grant"
sed "s|^executable=.*|executable=$DB1_MODULE|" \
  "$BUNDLE/grants/server/aimee-postgres.grant" > "$SERVER_POLICY/aimee-postgres.grant"
sed "s|^executable=.*|executable=$CONFIG_MODULE|" \
  "$BUNDLE/grants/server/config.grant" > "$SERVER_POLICY/config.grant"
sed "s|^executable=.*|executable=$POSTGRES_MODULE|" \
  "$BUNDLE/grants/server/postgres.grant" > "$SERVER_POLICY/postgres.grant"
sed "s|^executable=.*|executable=$CONFIG_MODULE|" \
  "$BUNDLE/grants/kb/config.grant" > "$KB_POLICY/config.grant"
sed "s|^executable=.*|executable=$POSTGRES_MODULE|" \
  "$BUNDLE/grants/kb/postgres.grant" > "$KB_POLICY/postgres.grant"

# A live chat/tool turn reaches process-owned policy in memory, routing,
# delegates, tools, workspace, git, skills, response composition, execution
# policy, runtime-web and sandbox. Starting only config + storage makes health
# and CRUD green while every real agent turn fails at its first module call.
# Reproduce the packaged server's required module manifest here, then add the
# optional modules exercised by this harness's turn-integrity probe.
feature_module_ids=()
while IFS=$'\t' read -r module_id _; do
  case "$module_id" in
    config|postgres|aimee) ;;
    *) feature_module_ids+=("$module_id") ;;
  esac
done < "$BUNDLE/server.modules"
for module_id in ${AIMEE_E2E_OPTIONAL_SERVER_MODULES:-benchmarks governance roundtable}; do
  [[ " ${feature_module_ids[*]} " == *" $module_id "* ]] || feature_module_ids+=("$module_id")
done

for module_id in "${feature_module_ids[@]}"; do
  module_bin="$MODULE_BIN_DIR/aimee-module-$module_id"
  install -m0755 "$REPO/src/build/obj/aimee-module" "$module_bin"
  grant="$BUNDLE/grants/server/$module_id.grant"
  [[ -r "$grant" ]] || { red "missing generated server grant: $grant"; exit 1; }
  sed "s|^executable=/usr/local/libexec/aimee-modules/|executable=$MODULE_BIN_DIR/|" \
    "$grant" > "$SERVER_POLICY/$module_id.grant"
done
if [[ " ${feature_module_ids[*]} " == *" roundtable "* ]]; then
  sed "s|^executable=/usr/local/libexec/aimee-modules/|executable=$MODULE_BIN_DIR/|" \
    "$BUNDLE/grants/server/roundtable-delegates.grant" \
    > "$SERVER_POLICY/roundtable-delegates.grant"
fi
chmod 0600 "$SERVER_POLICY"/*.grant "$KB_POLICY"/*.grant

kb_pid=""; server_pid=""
server_db1_pid=""; server_config_pid=""; server_postgres_pid=""
kb_config_pid=""; kb_postgres_pid=""
feature_module_pids=()

arm_module() { # executable socket policy log pid-variable [environment...]
  local executable="$1" socket="$2" policy="$3" log="$4" pid_var="$5"
  shift 5
  (
    local deadline=$((SECONDS + WAIT_SECONDS))
    while (( SECONDS < deadline )); do
      if [[ -S "$socket" ]]; then
        exec env AIMEE_HOME="$AIMEE_HOME" AIMEE_MODULE_POLICY_DIR="$policy" \
          "$@" "$executable" "$socket"
      fi
      sleep 0.1
    done
    echo "module: bus socket never appeared: $socket" >&2
  ) >>"$log" 2>&1 &
  printf -v "$pid_var" '%s' "$!"
}

stop_modules() {
  local pid
  for pid in "$server_db1_pid" "$server_config_pid" "$server_postgres_pid" \
             "$kb_config_pid" "$kb_postgres_pid" "${feature_module_pids[@]}"; do
    [[ -n "$pid" ]] || continue
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  server_db1_pid=""; server_config_pid=""; server_postgres_pid=""
  kb_config_pid=""; kb_postgres_pid=""
  feature_module_pids=()
}

cleanup() {
  stop_modules
  [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
  [[ -n "$kb_pid" ]] && kill "$kb_pid" 2>/dev/null || true
  cleanup_run_root
}
trap cleanup EXIT

# The server's worker threads need a 64 MB stack.
ulimit -S -s 65536 || true

if [[ "$MODE" == "full" ]]; then
  bold "==> Mode FULL (T5): local server + local kb"
  export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgresql:///aimee_shared}"
  # DB1 is a module-owned family too. Its aimee module reaches storage only
  # through the Postgres module on the SERVER bus; an unset store URL leaves
  # that declared edge present but unusable and the mTLS ramp correctly refuses.
  export AIMEE_STORE_URL="${AIMEE_STORE_URL:-$AIMEE_DB2_URL}"
  # This is an environment harness, so provision the extensions its fresh
  # database needs before migrations run. A server should not require runtime
  # CREATE EXTENSION authority, and silently running without pgvector would make
  # keyword-only retrieval look like semantic coverage.
  extension_error=""
  if command -v psql >/dev/null 2>&1; then
    pg_extension_cmd=(psql)
    if [[ "$(id -u)" == "0" && "$AIMEE_DB2_URL" =~ ^postgres(ql)?:///[^/?]+$ ]] && \
       command -v runuser >/dev/null 2>&1; then
      # A local peer-auth E2E database is normally owned by a non-superuser.
      # Use the cluster administrator only for CREATE EXTENSION; daemons and
      # every schema migration continue under AIMEE_DB2_URL's runtime role.
      pg_extension_cmd=(runuser -u postgres -- psql)
    fi
    if ! "${pg_extension_cmd[@]}" "$AIMEE_DB2_URL" -v ON_ERROR_STOP=1 \
      -c 'CREATE EXTENSION IF NOT EXISTS vector' \
      -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm' \
      >/dev/null 2>"$RUN_ROOT/extension-provision.err"; then
      extension_error="$(<"$RUN_ROOT/extension-provision.err")"
    fi
  else
    extension_error="psql is unavailable"
  fi
  if [[ -n "$extension_error" ]]; then
    if [[ "${AIMEE_E2E_REQUIRE_REAL_EMBEDDER:-0}" == "1" ]]; then
      red "    required PostgreSQL vector extensions could not be provisioned: $extension_error"
      exit 1
    fi
    yellow "    DEGRADED  PostgreSQL vector extensions were not provisioned: $extension_error"
  fi
  [[ -n "${EMBEDDER_URL:-}" ]] && export EMBEDDER_URL
  export AIMEE_KB_HTTP_BIND=1
  echo "    DB2: ${AIMEE_DB2_URL}"
  arm_module "$CONFIG_MODULE" "$AIMEE_HOME/kb-module-bus.sock" "$KB_POLICY" \
    "$AIMEE_HOME/kb-config-module.log" kb_config_pid
  arm_module "$POSTGRES_MODULE" "$AIMEE_HOME/kb-module-bus.sock" "$KB_POLICY" \
    "$AIMEE_HOME/kb-postgres-module.log" kb_postgres_pid \
    "AIMEE_DB2_URL=$AIMEE_DB2_URL"
  # Capture kb output so the embedder-fidelity gate below can see whether pgvec
  # accepted the memory vectors or refused them on a dim mismatch.
  "$REPO/aimee-kb" --http-port=8741 >"$AIMEE_HOME/kb.log" 2>&1 &
  kb_pid=$!
  export AIMEE_KB_API_URL="http://127.0.0.1:8741"
  # The server and kb deliberately share the scratch Vault in this single-host
  # topology.  Let the kb finish its first-boot credential transaction before
  # starting the server; launching both writers concurrently can make one fail
  # closed on the Vault's atomic-replace checks even though both inputs are
  # identical.
  bold "==> Waiting for aimee-kb first-boot initialization"
  deadline=$((SECONDS + WAIT_SECONDS))
  while ! curl -fsS --max-time 3 "${AIMEE_KB_API_URL}/v1/health" >/dev/null 2>&1; do
    if ! kill -0 "$kb_pid" 2>/dev/null; then
      red "    aimee-kb exited during startup"
      sed -n '1,20p' "$AIMEE_HOME/kb.log" >&2
      exit 1
    fi
    if (( SECONDS >= deadline )); then
      red "    aimee-kb did not become healthy within ${WAIT_SECONDS}s"
      sed -n '1,20p' "$AIMEE_HOME/kb.log" >&2
      exit 1
    fi
    sleep 1
  done
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
arm_module "$POSTGRES_MODULE" "$AIMEE_HOME/server-module-bus.sock" "$SERVER_POLICY" \
  "$AIMEE_HOME/server-postgres-module.log" server_postgres_pid \
  "AIMEE_STORE_URL=$AIMEE_STORE_URL"
arm_module "$DB1_MODULE" "$AIMEE_HOME/server-module-bus.sock" "$SERVER_POLICY" \
  "$AIMEE_HOME/server-db1-module.log" server_db1_pid \
  "AIMEE_STORE_URL=${AIMEE_STORE_URL:-}"
arm_module "$CONFIG_MODULE" "$AIMEE_HOME/server-module-bus.sock" "$SERVER_POLICY" \
  "$AIMEE_HOME/server-config-module.log" server_config_pid
for module_id in "${feature_module_ids[@]}"; do
  feature_pid=""
  arm_module "$MODULE_BIN_DIR/aimee-module-$module_id" \
    "$AIMEE_HOME/server-module-bus.sock" "$SERVER_POLICY" \
    "$AIMEE_HOME/server-$module_id-module.log" feature_pid
  feature_module_pids+=("$feature_pid")
done
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
  [[ -S "$AIMEE_HOME/aimee-http.sock" && "$status" != 000 ]] && break
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
deploy_status="$(curl -sS --unix-socket "$AIMEE_HOME/aimee-http.sock" \
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
                                               -d '{"query":"local e2e","scope":"all","max_results":3}' \
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
if [[ -z "${AIMEE_E2E_EMBEDDER_URL:-}" ]]; then
  yellow "  DEGRADED  no external embedder configured — builtin hash path only."
  if [[ "${AIMEE_E2E_REQUIRE_REAL_EMBEDDER:-0}" == "1" ]]; then
    red "  FAIL  real embedder required but AIMEE_E2E_EMBEDDER_URL is unset"
    FAIL=$((FAIL + 1))
  fi
elif [[ -n "$mm" ]]; then
  yellow "  DEGRADED  ${mm}; vectors refused — semantic search NOT exercised (list/keyword only)."
  yellow "            Wire a real embedder: point EMBEDDER_URL / SYNTHESIS_ENDPOINT at a"
  yellow "            Qwen3-Embedding endpoint whose dim matches the corpus (1024 CPU / 2560 GPU)."
  if [[ "${AIMEE_E2E_REQUIRE_REAL_EMBEDDER:-0}" == "1" ]]; then
    red "  FAIL  real embedder required (AIMEE_E2E_REQUIRE_REAL_EMBEDDER=1) but the run degraded to the builtin embedder"
    FAIL=$((FAIL + 1))
  fi
else
  embed_health="$(curl -fsS --max-time 5 "${AIMEE_E2E_EMBEDDER_URL%/}/health" 2>/dev/null || true)"
  if ! python3 - "$embed_health" <<'PY'
import json
import sys

try:
    health = json.loads(sys.argv[1])
except (json.JSONDecodeError, IndexError):
    raise SystemExit(1)
raise SystemExit(
    0 if health.get("status") == "ok"
    and isinstance(health.get("dim"), int) and health["dim"] > 0
    and isinstance(health.get("serving_id"), str) and health["serving_id"]
    else 1
)
PY
  then
    red "  FAIL  configured embedder has no healthy dimension-bound serving identity"
    FAIL=$((FAIL + 1))
  else
    semantic_marker="ti-semantic-${RANDOM}-${$}"
    semantic_content="At the moonless harbor, a cerulean lantern beside the eastern quay directs returning vessels. ${semantic_marker}"
    semantic_store="$(curl -fksS --max-time 30 "${IDENTITY[@]}" "${AUTH[@]}" \
      -H 'content-type: application/json' -X POST \
      -d "{\"key\":\"semantic harbor guide ${semantic_marker}\",\"content\":\"${semantic_content}\",\"kind\":\"fact\"}" \
      "$SERVER_URL/v1/memory/store" 2>/dev/null || true)"
    semantic_recall=""
    if [[ "$semantic_store" == *'"status":"ok"'* ]]; then
      for _ in $(seq 1 20); do
        # /memory/recall assembles the always-on/session context sections; it is
        # not the ranked fact-search surface. /memory/search accepts keyword
        # clusters but routes their joined natural-language query through the
        # live KB semantic ranker as well, so a lexically-disjoint query here is
        # direct evidence that the stored pgvector row was retrieved.
        semantic_recall="$(curl -fksS --max-time 60 "${IDENTITY[@]}" "${AUTH[@]}" \
          -H 'content-type: application/json' -X POST \
          -d '{"keywords":["Which colored light helps ships locate the dock after dark?"],"limit":10,"scope":"all"}' \
          "$SERVER_URL/v1/memory/search" 2>/dev/null || true)"
        [[ "$semantic_recall" == *"$semantic_marker"* ]] && break
        sleep 0.25
      done
    fi
    if [[ "$semantic_recall" == *"$semantic_marker"* ]]; then
      green "  PASS  real embedder stored and semantically recalled a lexically-disjoint fact"
      PASS=$((PASS + 1))
    else
      red "  FAIL  real embedder did not semantically recall the stored fact"
      FAIL=$((FAIL + 1))
    fi
  fi
fi

if [[ -n "${AIMEE_E2E_PROBE_SCRIPT:-}" ]]; then
  bold "==> External live-stack probe: ${AIMEE_E2E_PROBE_SCRIPT}"
  if REPO="$REPO" RUN_ROOT="$RUN_ROOT" SCRATCH="$SCRATCH" SERVER_URL="$SERVER_URL" \
       BEARER="$BEARER" CLIENT_CERT="$CLIENT_CERT" CLIENT_KEY="$CLIENT_KEY" \
       KB_PID="$kb_pid" SERVER_PID="$server_pid" \
       "$AIMEE_E2E_PROBE_SCRIPT"; then
    green "  PASS  external live-stack probe"; PASS=$((PASS + 1))
  else
    red "  FAIL  external live-stack probe"; FAIL=$((FAIL + 1))
  fi
fi

if [[ "${AIMEE_E2E_RESTART_COMPONENTS:-0}" == "1" && "$MODE" == "full" ]]; then
  bold "==> Exploratory daemon restart and persisted recovery"
  kill "$server_pid"
  wait "$server_pid" 2>/dev/null || true
  server_pid=""
  # The bootstrap bearer is a first-process transport secret. After deploy/apply
  # seals the installation, replaying an enrolled client's bearer through that
  # channel is invalid configuration and must be rejected. A real restart uses
  # only the persisted Vault/config state.
  "$REPO/aimee-server" --socket="$AIMEE_HOME/aimee-server.sock" &
  server_pid=$!
  deadline=$((SECONDS + WAIT_SECONDS))
  while ! curl -fksS --max-time 5 "${IDENTITY[@]}" "${AUTH[@]}" \
    "$SERVER_URL/v1/health" >/dev/null 2>&1; do
    kill -0 "$server_pid" 2>/dev/null || break
    (( SECONDS < deadline )) || break
    sleep 1
  done
  persisted="$(curl -fksS --max-time 10 "${IDENTITY[@]}" "${AUTH[@]}" \
    -H 'content-type: application/json' -X POST -d '{"limit":50}' \
    "$SERVER_URL/v1/memory/list" 2>/dev/null || true)"
  if [[ "$persisted" == *'aimeeE2E'* ]]; then
    green "  PASS  aimee-server restart retained mTLS identity and persisted memory"
    PASS=$((PASS + 1))
  else
    red "  FAIL  aimee-server did not recover enrolled persisted state"
    FAIL=$((FAIL + 1))
  fi

  kill "$kb_pid"
  wait "$kb_pid" 2>/dev/null || true
  kb_pid=""
  "$REPO/aimee-kb" --http-port=8741 >>"$AIMEE_HOME/kb.log" 2>&1 &
  kb_pid=$!
  deadline=$((SECONDS + WAIT_SECONDS))
  kb_recovered=""
  while (( SECONDS < deadline )); do
    kb_recovered="$(curl -fksS --max-time 10 "${IDENTITY[@]}" "${AUTH[@]}" \
      -H 'content-type: application/json' -X POST \
      -d '{"query":"restart recovery","scope":"all","max_results":1}' \
      "$SERVER_URL/v1/kb/search" 2>/dev/null || true)"
    [[ "$kb_recovered" == *'"hits"'* ]] && break
    kill -0 "$kb_pid" 2>/dev/null || break
    sleep 1
  done
  if [[ "$kb_recovered" == *'"hits"'* ]]; then
    green "  PASS  aimee-kb restart restored the live server→KB search path"
    PASS=$((PASS + 1))
  else
    red "  FAIL  aimee-kb did not restore server→KB search after restart"
    FAIL=$((FAIL + 1))
  fi
fi

echo
bold "==> Summary (${MODE}): ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "local ${MODE} stack is up and serving."
if [[ "${AIMEE_E2E_HOLD_SECONDS:-0}" =~ ^[0-9]+$ ]] &&
   (( AIMEE_E2E_HOLD_SECONDS > 0 )); then
  yellow "holding green scratch stack for ${AIMEE_E2E_HOLD_SECONDS}s (exploratory probes)"
  sleep "$AIMEE_E2E_HOLD_SECONDS"
fi
