#!/usr/bin/env bash
#
# aimee-thinclient-adoption-e2e.sh — E2E for Vault-backed thin-client
# enrollment. A random first-boot primary is transported by env exactly once:
#
#   1. pin the server's self-signed TLS cert (remote-ca.pem) and verify,
#   2. explicitly add a fresh individual bearer (POST /v1/api/enroll_bearer),
#   3. persist the individual token to the client's remote.conf,
#      while the server seals the same token in its encrypted Vault,
#   4. enroll the client's mTLS identity and promote the server to required mTLS,
#   5. leave bearer-only requests locked out while the enrolled client survives
#      a server restart without its first-boot environment input.
#
# This drives the real `aimee` client binary against a real local aimee-server
# over the TLS /v1 listener — the exact flow a fresh appliance install runs on
# first connect (no docker, Linux only).
#
# Env:
#   TLS_PORT      server /v1 TLS port      (default 28743)
#   HTTP_PORT     server plaintext port    (default 28740, loopback-only)
#   WAIT_SECONDS  health wait budget       (default 60)
#   AIMEE_BIN / AIMEE_SERVER_BIN
#                 prebuilt binaries to drive instead of building from src/
#                 (e.g. run this script inside the server container image)
#
# Exit code: 0 = all checks passed.

set -uo pipefail

TLS_PORT="${TLS_PORT:-28743}"
HTTP_PORT="${HTTP_PORT:-28740}"
WAIT_SECONDS="${WAIT_SECONDS:-60}"
PRIMARY="$(openssl rand -hex 32)"

cd "$(dirname "$0")/.."
REPO="$(pwd)"
AIMEE_BIN="${AIMEE_BIN:-$REPO/aimee}"
AIMEE_SERVER_BIN="${AIMEE_SERVER_BIN:-$REPO/aimee-server}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

PASS=0
FAIL=0
ok()   { green "  PASS  $1"; PASS=$((PASS + 1)); }
bad()  { red   "  FAIL  $1"; [[ $# -gt 1 ]] && printf '        %s\n' "$2"; FAIL=$((FAIL + 1)); }

# --- build (skipped when both binaries already exist / are supplied) --------
if [[ -x "$AIMEE_BIN" && -x "$AIMEE_SERVER_BIN" ]]; then
  bold "==> Using binaries: $AIMEE_BIN / $AIMEE_SERVER_BIN"
else
  bold "==> Building aimee (thin client) + aimee-server"
  make -C src ../aimee ../aimee-server >/dev/null
fi

# --- the DB1 module ---------------------------------------------------------
# The daemon holds no store: it reaches DB1 over the module bus, so without the
# module every family is unreachable. That is not a degraded run -- the pki
# family failing makes the mTLS ramp self-test fail, TLS is then disabled, and
# this harness times out waiting for a listener that was never coming.
# `make all` does not build the module, so build it here.
# The multicall binary; the store and postgres are two names for it.
DB1_MODULE_BUILT="$REPO/src/build/obj/aimee-module"
CONFIG_MODULE_BUILT="$REPO/src/build/obj/aimee-module-config"
if [[ ! -x "$DB1_MODULE_BUILT" || ! -x "$CONFIG_MODULE_BUILT" ]]; then
  bold "==> Building the store and config modules"
  make -C src build/obj/aimee-module build/obj/aimee-module-config >/dev/null
fi

# --- server: scratch home, TLS listener, first-boot Vault bearer ------------
SERVER_HOME="$(mktemp -d)"
CLIENT_HOME="$(mktemp -d)"

# Baked non-secret listener policy remapped to test ports.
sed "s/8740/${HTTP_PORT}/; s/8743/${TLS_PORT}/" \
    deploy/container/aimee-server.yaml > "$SERVER_HOME/aimee.yaml"

# The grant the supervisor would write, taken from the generated bundle so the
# served kinds cannot drift from what the module actually serves.
DB1_MODULE="$SERVER_HOME/aimee-module-aimee"
PG_MODULE="$SERVER_HOME/aimee-module-postgres"
CONFIG_MODULE="$SERVER_HOME/aimee-module-config"
MODULE_BUS_SOCK="$SERVER_HOME/server-module-bus.sock"
module_pid=""
config_module_pid=""
install -m0755 "$DB1_MODULE_BUILT" "$DB1_MODULE"
install -m0755 "$CONFIG_MODULE_BUILT" "$CONFIG_MODULE"
mkdir -p "$SERVER_HOME/modules.d/server"
DB1_GRANT="$REPO/src/build/obj/module-bundle/grants/server/aimee.grant"
if [[ ! -r "$DB1_GRANT" ]]; then
  python3 "$REPO/scripts/export_c_repositories.py" \
    --runtime-bundle "$REPO/src/build/obj/module-bundle" >/dev/null 2>&1 || true
fi
if [[ ! -r "$DB1_GRANT" ]]; then
  red "no generated store grant at $DB1_GRANT; run scripts/export_c_repositories.py"
  exit 1
fi
sed "s|^executable=.*|executable=$DB1_MODULE|" "$DB1_GRANT" \
  >"$SERVER_HOME/modules.d/server/aimee.grant"
install -m0755 "$DB1_MODULE_BUILT" "$PG_MODULE"
# Both grants come from the SAME generated bundle the store's grant above
# does, rather than being written out here: the refs and kinds are derived from
# src/modules/process-contracts.json, and a copy transcribed into this file
# goes stale silently. The store's outbound ref moved from 68 to 69 in a merge;
# a heredoc here would still say 68 while every other site said 69.
#
# The postgres module serves the SQL stage the store calls; the store's OUTBOUND
# principal is what is allowed to call it. A serve grant admits what a module
# answers, not what it asks for, so the second is not implied by the first --
# without it the store attaches and then finds no backend, which reads exactly
# like a broken store.
BUNDLE_GRANTS="$REPO/src/build/obj/module-bundle/grants/server"
for grant_name in postgres aimee-postgres; do
  if [[ "$grant_name" == postgres ]]; then
    grant_exe="$PG_MODULE"
  else
    grant_exe="$DB1_MODULE"
  fi
  if [[ ! -r "$BUNDLE_GRANTS/$grant_name.grant" ]]; then
    red "no generated $grant_name grant at $BUNDLE_GRANTS/$grant_name.grant"
    exit 1
  fi
  sed "s|^executable=.*|executable=$grant_exe|" \
    "$BUNDLE_GRANTS/$grant_name.grant" \
    >"$SERVER_HOME/modules.d/server/$grant_name.grant"
done
CONFIG_GRANT="$REPO/src/build/obj/module-bundle/grants/server/config.grant"
if [[ ! -r "$CONFIG_GRANT" ]]; then
  red "no generated config grant at $CONFIG_GRANT"
  exit 1
fi
sed "s|^executable=.*|executable=$CONFIG_MODULE|" "$CONFIG_GRANT" \
  >"$SERVER_HOME/modules.d/server/config.grant"

# Armed BEFORE the daemon, waiting for the socket the daemon is about to create.
# The daemon runs its mTLS ramp self-test once, at startup, and that needs the
# pki family; a module attaching afterwards is already too late.
start_module() {
  stop_module
  # Migrations and ordinary operations are separate capabilities. Refuse an
  # incomplete fixture here, before the module can turn it into an opaque
  # "store unavailable" startup failure.
  if [[ -z "${AIMEE_STORE_URL:-}" || -z "${AIMEE_STORE_MIGRATION_URL:-}" ]]; then
    red "AIMEE_STORE_URL and AIMEE_STORE_MIGRATION_URL are both required"
    exit 1
  fi
  local migration_url="$AIMEE_STORE_MIGRATION_URL"
  (
    deadline=$((SECONDS + WAIT_SECONDS))
    while (( SECONDS < deadline )); do
      if [[ -S "$MODULE_BUS_SOCK" ]]; then
        AIMEE_STORE_URL="${AIMEE_STORE_URL:-}" \
          AIMEE_STORE_MIGRATION_URL="$migration_url" "$PG_MODULE" "$MODULE_BUS_SOCK" &
        AIMEE_STORE_URL="${AIMEE_STORE_URL:-}" \
          AIMEE_STORE_MIGRATION_URL="$migration_url" exec "$DB1_MODULE" "$MODULE_BUS_SOCK"
      fi
      sleep 0.1
    done
    echo "module: the bus socket never appeared" >&2
  ) >>"$SERVER_HOME/module.log" 2>&1 &
  module_pid=$!
  (
    deadline=$((SECONDS + WAIT_SECONDS))
    while (( SECONDS < deadline )); do
      if [[ -S "$MODULE_BUS_SOCK" ]]; then
        AIMEE_HOME="$SERVER_HOME" exec "$CONFIG_MODULE" "$MODULE_BUS_SOCK"
      fi
      sleep 0.1
    done
    echo "config module: the bus socket never appeared" >&2
  ) >>"$SERVER_HOME/config-module.log" 2>&1 &
  config_module_pid=$!
}
stop_module() {
  if [[ -n "$module_pid" ]]; then
    kill "$module_pid" 2>/dev/null || true
    wait "$module_pid" 2>/dev/null || true
    module_pid=""
  fi
  if [[ -n "$config_module_pid" ]]; then
    kill "$config_module_pid" 2>/dev/null || true
    wait "$config_module_pid" 2>/dev/null || true
    config_module_pid=""
  fi
}

server_pid=""
first_start=1
cleanup() {
  stop_module
  [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
  if [[ "${AIMEE_E2E_KEEP:-0}" == 1 ]]; then
    printf 'kept server home: %s\nkept client home: %s\n' "$SERVER_HOME" "$CLIENT_HOME"
  else
    rm -rf "$SERVER_HOME" "$CLIENT_HOME"
  fi
}
trap cleanup EXIT

ulimit -S -s 65536 || true # server worker threads need a 64 MB stack

bold "==> Starting aimee-server (TLS :${TLS_PORT}, Vault first-boot bearer)"
start_server() {
  start_module
  if [[ "$first_start" == 1 ]]; then
    AIMEE_HOME="$SERVER_HOME" AIMEE_DB1_URL="sqlite://${SERVER_HOME}/aimee.db" \
      AIMEE_SERVER_HTTP_BIND=1 AIMEE_API_BEARER_TOKEN="$PRIMARY" \
      "$AIMEE_SERVER_BIN" >>"$SERVER_HOME/server.log" 2>&1 &
    first_start=0
  else
    AIMEE_HOME="$SERVER_HOME" AIMEE_DB1_URL="sqlite://${SERVER_HOME}/aimee.db" \
      AIMEE_SERVER_HTTP_BIND=1 "$AIMEE_SERVER_BIN" >>"$SERVER_HOME/server.log" 2>&1 &
  fi
  server_pid=$!
  local deadline=$((SECONDS + WAIT_SECONDS))
  until curl -sk --max-time 3 "https://127.0.0.1:${TLS_PORT}/v1/health" >/dev/null 2>&1; do
    if (( SECONDS >= deadline )) || ! kill -0 "$server_pid" 2>/dev/null; then
      red "server did not serve TLS /v1 within ${WAIT_SECONDS}s"
      tail -20 "$SERVER_HOME/server.log" || true
      exit 1
    fi
    sleep 1
  done
}
start_server
green "    TLS /v1 listener is up"

if tr '\0' '\n' <"/proc/$server_pid/environ" 2>/dev/null | grep -q '^AIMEE_API_BEARER_TOKEN='; then
  bad "first-boot bearer removed from the long-lived server environment" "credential still present in /proc/$server_pid/environ"
else
  ok "first-boot bearer removed from the long-lived server environment"
fi
[[ ! -e "$SERVER_HOME/tls/server.key" ]] \
  && ok "server TLS private key exists only in Vault" \
  || bad "server TLS private key exists only in Vault" "found $SERVER_HOME/tls/server.key"
[[ ! -e "$SERVER_HOME/server.token" ]] \
  && ok "retired shared bearer file was not created" \
  || bad "retired shared bearer file was not created" "found $SERVER_HOME/server.token"

URL="https://127.0.0.1:${TLS_PORT}"

# --- adoption: first client uses the operator-known primary once ------------
bold "==> Adoption: first client pins TLS and enrolls from the first-boot primary"
set_json="$(AIMEE_HOME="$CLIENT_HOME" "$AIMEE_BIN" --json remote set "$URL" "$PRIMARY" 2>"$CLIENT_HOME/set.err")" || true
echo "    remote set -> ${set_json:-<no output>}"

[[ "$set_json" == *'"pinned":true'* || "$set_json" == *'"verified":true'* ]] \
  && ok "TLS trust established (pinned/verified)" \
  || bad "TLS trust established" "$set_json $(cat "$CLIENT_HOME/set.err")"
[[ -s "$CLIENT_HOME/remote-ca.pem" ]] \
  && ok "server cert pinned to remote-ca.pem" \
  || bad "server cert pinned to remote-ca.pem"
if AIMEE_HOME="$CLIENT_HOME" "$AIMEE_BIN" remote enroll >"$CLIENT_HOME/enroll.out" 2>"$CLIENT_HOME/enroll.err"; then
  ok "explicit additive enrollment completed"
else
  bad "explicit additive enrollment completed" "$(cat "$CLIENT_HOME/enroll.err")"
fi

# The client must now hold a new individual token, never the primary.
client_token="$(sed -n 2p "$CLIENT_HOME/remote.conf")"
if [[ "$client_token" != "$PRIMARY" && "$client_token" =~ ^[0-9a-f]{64}$ ]]; then
  ok "remote.conf updated to a generated 256-bit bearer"
else
  bad "remote.conf updated to a generated 256-bit bearer" "token: '${client_token:-<empty>}'"
fi

# The server must authenticate with the new token without ever persisting it in
# plaintext configuration. Its restart-survival contract is the encrypted Vault.
if grep -Fq "$client_token" "$SERVER_HOME/aimee.yaml" || grep -Fq "$PRIMARY" "$SERVER_HOME/aimee.yaml"; then
  bad "server kept the generated bearer out of aimee.yaml" "plaintext token found"
else
  ok "server kept the generated bearer out of aimee.yaml (Vault-only)"
fi

# The adopted client transacts real work with the new token.
AIMEE_HOME="$CLIENT_HOME" "$AIMEE_BIN" remote status >/dev/null 2>&1 \
  && ok "adopted client reaches /v1 with the new bearer" \
  || bad "adopted client reaches /v1 with the new bearer"

# Restart from the same persistent home. The only server-side copy capable of
# authenticating the client is now the encrypted Vault entry.
# The module attaches to a socket the daemon owns, so a daemon restart takes its
# attachment with it. Stop it and clear the stale socket; start_server arms a
# fresh one against the socket the new daemon creates.
stop_module
kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=""
rm -f "$MODULE_BUS_SOCK"
start_server
AIMEE_HOME="$CLIENT_HOME" "$AIMEE_BIN" remote status >/dev/null 2>&1 \
  && ok "Vault-backed bearer and enrolled client survive server restart" \
  || bad "Vault-backed bearer and enrolled client survive server restart"

# --- mTLS promotion leaves every bare bearer locked out --------------------
bold "==> Required mTLS rejects bearer-only requests after enrollment"
st="$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' \
      -H "Authorization: Bearer ${PRIMARY}" "${URL}/v1/health")"
[[ "$st" == 000 || "$st" == 401 ]] \
  && ok "raw request with the primary rejected without enrolled mTLS" \
  || bad "raw request with the primary rejected without enrolled mTLS" "got HTTP $st"

st="$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' \
      -H "Authorization: Bearer ${client_token}" "${URL}/v1/health")"
[[ "$st" == 000 || "$st" == 401 ]] \
  && ok "raw request with the individual bearer also needs the enrolled certificate" \
  || bad "raw request with the individual bearer also needs the enrolled certificate" "got HTTP $st"

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
if [[ "$FAIL" != 0 ]]; then
  printf '%s\n' '--- server log ---'
  tail -40 "$SERVER_HOME/server.log" 2>/dev/null || true
  printf '%s\n' '--- DB1 module log ---'
  tail -20 "$SERVER_HOME/module.log" 2>/dev/null || true
  printf '%s\n' '--- config module log ---'
  tail -20 "$SERVER_HOME/config-module.log" 2>/dev/null || true
  exit 1
fi
green "thin-client Vault enrollment works end to end."
