#!/usr/bin/env bash
# Prove the managed single-user remote quickstart over the real TLS listener.
#
# This is intentionally a transport test, not another pure policy matrix. It
# starts the shipped server with the managed image defaults, enrolls the shipped
# thin client, and drives a real working-memory write through handle_conn. It then proves
# the two fail-closed boundaries on the same route:
#   1. the deployment bearer without the enrolled certificate is denied; and
#   2. configuring strict per-user authority disables the compatibility tier.
# The strict-authority positive path (real signed token + matching grant + real
# write) remains covered by scripts/run-write-tier-enforce-live.sh; this focused
# harness proves the managed quickstart and its cutoff without requiring Postgres.

set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
  echo "aimee-managed-remote-auth-e2e: SKIP (automatic CSR enrollment is Linux-only)"
  exit 0
fi

for tool in curl openssl python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "aimee-managed-remote-auth-e2e: missing required tool: $tool" >&2
    exit 2
  }
done

REPO="$(cd "$(dirname "$0")/.." && pwd)"
[[ -x "$REPO/aimee" && -x "$REPO/aimee-server" ]] || {
  echo "aimee-managed-remote-auth-e2e: build aimee and aimee-server first" >&2
  exit 2
}

WORK="$(mktemp -d /tmp/aimee-managed-remote-auth-XXXXXX)"
SERVER_HOME="$WORK/server"
CLIENT_HOME="$WORK/client"
SERVER_LOG="$SERVER_HOME/server.log"
SERVER_STDIO="$WORK/server.stdio"
mkdir -p "$SERVER_HOME" "$CLIENT_HOME"

SERVER_PID=""
cleanup() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ "${AIMEE_E2E_KEEP_WORK:-0}" == 1 ]]; then
    echo "aimee-managed-remote-auth-e2e: retained diagnostics in $WORK" >&2
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

TLS_PORT="${SERVER_TLS_PORT:-$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')}"
SERVER_URL="https://127.0.0.1:${TLS_PORT}"
BOOTSTRAP="aimee-local-dev"

cat >"$SERVER_HOME/aimee.yaml" <<YAML
aimee:
  api:
    http_port: 0
    tls_port: ${TLS_PORT}
    bearer_token: "${BOOTSTRAP}"
    mtls: optional
    remote_writes: full
YAML

start_server() {
  local strict="${1:-0}"
  local tier="${2:-full}"
  local -a authority=()
  local -a probe_identity=()
  if [[ "$strict" == 1 ]]; then
    authority=(
      AIMEE_SERVER_TEAM_ID=7
      AIMEE_SERVER_ID=managed-remote-auth-e2e
      AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE="$WORK/strict-authority.json"
    )
  fi
  env AIMEE_HOME="$SERVER_HOME" \
      AIMEE_SERVER_HTTP_BIND=1 \
      AIMEE_API_MTLS=optional \
      AIMEE_API_REMOTE_WRITES="$tier" \
      "${authority[@]}" \
      "$REPO/aimee-server" >"$SERVER_STDIO" 2>&1 &
  SERVER_PID=$!

  if [[ -s "$CLIENT_HOME/tls/client.crt" && -s "$CLIENT_HOME/tls/client.key" ]]; then
    probe_identity=(--cert "$CLIENT_HOME/tls/client.crt" --key "$CLIENT_HOME/tls/client.key")
  fi

  local status="000"
  for _ in $(seq 1 100); do
    status="$(curl -sk "${probe_identity[@]}" --max-time 1 -o /dev/null -w '%{http_code}' \
      "$SERVER_URL/v1/health" || true)"
    [[ "$status" != 000 ]] && return 0
    kill -0 "$SERVER_PID" 2>/dev/null || {
      echo "aimee-managed-remote-auth-e2e: server exited during startup" >&2
      tail -40 "$SERVER_LOG" >&2 || true
      tail -40 "$SERVER_STDIO" >&2 || true
      exit 1
    }
    sleep 0.1
  done
  echo "aimee-managed-remote-auth-e2e: TLS listener never became ready" >&2
  tail -40 "$SERVER_LOG" >&2 || true
  tail -40 "$SERVER_STDIO" >&2 || true
  exit 1
}

stop_server() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
}

start_server 0

# This is the user-facing quickstart command. It pins the server leaf, rotates
# the one-shot bearer, and installs a server-signed client certificate.
AIMEE_HOME="$CLIENT_HOME" "$REPO/aimee" remote set "$SERVER_URL" "$BOOTSTRAP" >/dev/null
[[ -s "$CLIENT_HOME/tls/client.crt" && -s "$CLIENT_HOME/tls/client.key" ]] || {
  echo "FAIL: remote set did not install the client certificate" >&2
  exit 1
}

# Reuse the enrolled deployment bearer but deliberately omit client.crt/key.
# `remote set` rotates the bearer, submits the CSR without a certificate, then
# installs client.crt/key only after the signing response; it sends no subsequent
# request. This probe is therefore the first opportunity to present the new cert,
# and deliberately does not. A fresh one-client roster promotes optional mTLS to
# required only after the later enrolled CLI call presents it. Keeping the secret
# in a mode-0600 curl config avoids putting it in argv.
BEARER="$(sed -n '2p' "$CLIENT_HOME/remote.conf")"
[[ -n "$BEARER" && "$BEARER" != "$BOOTSTRAP" ]] || {
  echo "FAIL: remote set did not rotate the bootstrap bearer" >&2
  exit 1
}
CURL_CONFIG="$WORK/curl.conf"
umask 077
printf 'header = "Authorization: Bearer %s"\n' "$BEARER" >"$CURL_CONFIG"
code="$(curl -sk --config "$CURL_CONFIG" --max-time 10 -o "$WORK/bearer-only.json" \
  -w '%{http_code}' -H 'content-type: application/json' -X POST \
  -d '{"session_id":"managed-e2e","key":"bearer-only","value":"must be denied","category":"general"}' \
  "$SERVER_URL/v1/wm/set")"
[[ "$code" == 403 ]] || {
  echo "FAIL: bearer-only write returned HTTP $code, expected 403" >&2
  exit 1
}
echo "PASS: the deployment bearer without mTLS is read-only"

SENTINEL="managed remote auth ${$} ${RANDOM}"
if ! store_out="$(AIMEE_HOME="$CLIENT_HOME" "$REPO/aimee" wm set quickstart "$SENTINEL" 2>&1)"; then
  echo "FAIL: enrolled mTLS working-memory write failed: $store_out" >&2
  tail -40 "$SERVER_LOG" >&2 || true
  tail -40 "$SERVER_STDIO" >&2 || true
  exit 1
fi
readback="$(AIMEE_HOME="$CLIENT_HOME" "$REPO/aimee" wm get quickstart)"
[[ "$readback" == *"$SENTINEL"* ]] || {
  echo "FAIL: enrolled mTLS write did not round-trip through working memory" >&2
  exit 1
}
echo "PASS: managed defaults allow a real enrolled-mTLS working-memory write"

# The same enrolled identity must not bypass an operator lowering the deployment
# posture. The durable roster is now required, so readiness and the denied write
# both present the already-enrolled certificate.
stop_server
start_server 0 off
if AIMEE_HOME="$CLIENT_HOME" "$REPO/aimee" wm set quickstart \
     "off tier must deny ${SENTINEL}" >"$WORK/off.out" 2>"$WORK/off.err"; then
  echo "FAIL: remote_writes=off unexpectedly allowed the enrolled client" >&2
  exit 1
fi
grep -Eq 'enrolled mTLS|remote_writes|requires capabilities' "$WORK/off.err" || {
  echo "FAIL: remote_writes=off failed without the expected authorization error" >&2
  sed -n '1,8p' "$WORK/off.err" >&2
  exit 1
}
echo "PASS: remote_writes=off keeps an enrolled mTLS client read-only"

# Merely configuring the three authority inputs is the deliberate strict-mode
# cutoff. No identity token is available in this harness, so the previously
# successful enrolled client must now fail closed on the same real route.
stop_server
start_server 1 full
if AIMEE_HOME="$CLIENT_HOME" "$REPO/aimee" wm set quickstart \
     "strict cutoff must deny ${SENTINEL}" >"$WORK/strict.out" 2>"$WORK/strict.err"; then
  echo "FAIL: strict authority unexpectedly fell back to the deployment tier" >&2
  exit 1
fi
grep -Eq 'Strict per-user|write-tier grant|requires capabilities' "$WORK/strict.err" || {
  echo "FAIL: strict cutoff failed without the expected authorization error" >&2
  sed -n '1,8p' "$WORK/strict.err" >&2
  exit 1
}
echo "PASS: configured per-user authority disables the compatibility tier"
echo "aimee-managed-remote-auth-e2e: OK"
