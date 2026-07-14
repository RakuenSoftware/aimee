#!/usr/bin/env bash
#
# aimee-thinclient-adoption-e2e.sh — E2E for thin-client ADOPTION (TOFU
# enrollment): a NEW thin client is pointed at an aimee-server that still holds
# the default one-time bootstrap bearer, and `aimee remote set` must
#
#   1. pin the server's self-signed TLS cert (remote-ca.pem) and verify,
#   2. automatically rotate the bootstrap to a fresh strong bearer
#      (POST /v1/api/rotate_bearer),
#   3. persist the NEW token to the client's remote.conf (never the bootstrap),
#      while the server persists the same token in its aimee.yaml,
#   4. leave the bootstrap dead: raw requests with it get 401, and a SECOND
#      fresh client presenting the bootstrap is refused enrollment.
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
BOOTSTRAP="aimee-local-dev"

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

# --- server: scratch home, TLS listener, bootstrap bearer ------------------
SERVER_HOME="$(mktemp -d)"
CLIENT_HOME="$(mktemp -d)"
CLIENT2_HOME="$(mktemp -d)"

# Baked container default (bootstrap bearer, tls_port) remapped to test ports.
sed "s/8740/${HTTP_PORT}/; s/8743/${TLS_PORT}/" \
    deploy/container/aimee-server.yaml > "$SERVER_HOME/aimee.yaml"

server_pid=""
cleanup() {
  [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
  rm -rf "$SERVER_HOME" "$CLIENT_HOME" "$CLIENT2_HOME"
}
trap cleanup EXIT

ulimit -S -s 65536 || true # server worker threads need a 64 MB stack

bold "==> Starting aimee-server (TLS :${TLS_PORT}, bootstrap bearer)"
AIMEE_HOME="$SERVER_HOME" AIMEE_DB1_URL="sqlite://${SERVER_HOME}/aimee.db" \
  AIMEE_SERVER_HTTP_BIND=1 "$AIMEE_SERVER_BIN" >"$SERVER_HOME/server.log" 2>&1 &
server_pid=$!

deadline=$((SECONDS + WAIT_SECONDS))
until curl -sk --max-time 3 "https://127.0.0.1:${TLS_PORT}/v1/health" >/dev/null 2>&1; do
  if (( SECONDS >= deadline )) || ! kill -0 "$server_pid" 2>/dev/null; then
    red "server did not serve TLS /v1 within ${WAIT_SECONDS}s"
    tail -20 "$SERVER_HOME/server.log" || true
    exit 1
  fi
  sleep 1
done
green "    TLS /v1 listener is up"

URL="https://127.0.0.1:${TLS_PORT}"

# --- adoption: fresh client, default bearer --------------------------------
bold "==> Adoption: fresh thin client runs 'aimee remote set ${URL} <bootstrap>'"
set_json="$(AIMEE_HOME="$CLIENT_HOME" "$AIMEE_BIN" --json remote set "$URL" "$BOOTSTRAP" 2>"$CLIENT_HOME/set.err")" || true
echo "    remote set -> ${set_json:-<no output>}"

[[ "$set_json" == *'"pinned":true'* || "$set_json" == *'"verified":true'* ]] \
  && ok "TLS trust established (pinned/verified)" \
  || bad "TLS trust established" "$set_json $(cat "$CLIENT_HOME/set.err")"
[[ -s "$CLIENT_HOME/remote-ca.pem" ]] \
  && ok "server cert pinned to remote-ca.pem" \
  || bad "server cert pinned to remote-ca.pem"
[[ "$set_json" == *'"enrolled":true'* ]] \
  && ok "bootstrap auto-enrolled on first connect" \
  || bad "bootstrap auto-enrolled on first connect" "$set_json"

# The client must now hold a NEW strong token — never the bootstrap.
client_token="$(sed -n 2p "$CLIENT_HOME/remote.conf")"
if [[ "$client_token" != "$BOOTSTRAP" && "$client_token" =~ ^[0-9a-f]{64}$ ]]; then
  ok "remote.conf updated to a generated 256-bit bearer"
else
  bad "remote.conf updated to a generated 256-bit bearer" "token: '${client_token:-<empty>}'"
fi

# The server must have persisted the SAME token (survives a restart).
grep -q "bearer_token.*${client_token}" "$SERVER_HOME/aimee.yaml" \
  && ok "server persisted the same bearer in its aimee.yaml" \
  || bad "server persisted the same bearer in its aimee.yaml"

# The adopted client transacts real work with the new token.
AIMEE_HOME="$CLIENT_HOME" "$AIMEE_BIN" remote status >/dev/null 2>&1 \
  && ok "adopted client reaches /v1 with the new bearer" \
  || bad "adopted client reaches /v1 with the new bearer"

# --- the bootstrap is dead -------------------------------------------------
bold "==> The consumed bootstrap no longer authorizes anything"
st="$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' \
      -H "Authorization: Bearer ${BOOTSTRAP}" "${URL}/v1/health")"
[[ "$st" == 401 ]] \
  && ok "raw request with the bootstrap -> 401" \
  || bad "raw request with the bootstrap -> 401" "got HTTP $st"

st="$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' \
      -H "Authorization: Bearer ${client_token}" "${URL}/v1/health")"
[[ "$st" == 200 ]] \
  && ok "raw request with the rotated bearer -> 200" \
  || bad "raw request with the rotated bearer -> 200" "got HTTP $st"

# --- a second fresh client cannot re-adopt with the bootstrap ---------------
bold "==> A second fresh client presenting the bootstrap is refused enrollment"
set2_json="$(AIMEE_HOME="$CLIENT2_HOME" "$AIMEE_BIN" --json remote set "$URL" "$BOOTSTRAP" 2>"$CLIENT2_HOME/set.err")" || true
[[ "$set2_json" == *'"enrolled":false'* ]] \
  && ok "second client not enrolled (one-shot TOFU)" \
  || bad "second client not enrolled (one-shot TOFU)" "$set2_json"
second_token="$(sed -n 2p "$CLIENT2_HOME/remote.conf" 2>/dev/null)"
[[ "$second_token" == "$BOOTSTRAP" ]] \
  && ok "second client's remote.conf still holds the (useless) bootstrap" \
  || bad "second client's remote.conf still holds the (useless) bootstrap" "token: '${second_token:-<empty>}'"

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "thin-client adoption (TOFU enrollment) works end to end."
