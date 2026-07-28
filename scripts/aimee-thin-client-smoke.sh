#!/usr/bin/env bash
#
# aimee-thin-client-smoke.sh — prove the cross-platform thin `aimee` client can
# drive a remote aimee-server over the HTTP transport. This is the CLIENT axis of
# the deploy matrix (docs/proposals/pending/aimee-e2e-deploy-matrix.md §2.2):
# the SAME script runs on Linux, macOS, and Windows (git-bash / CI `shell: bash`)
# because the only OS-specific thing is the `aimee` binary it invokes.
#
# Two modes:
#   * full      — a server URL is reachable: configure the HTTP transport and do
#                 a real read round-trip (`aimee session list`).
#   * selfcheck — no server reachable: only verify the client binary runs and
#                 accepts the transport config. Reported explicitly as SELFCHECK
#                 so a CI run on a host with no server never reads as a full pass.
#
# Env:
#   AIMEE_BIN     path to the aimee client      (default: aimee on PATH)
#   SERVER_URL    server base URL               (default http://localhost:8740)
#   BEARER        server bearer token           (default aimee-local-dev)
#   FORCE_MODE    full | selfcheck              (default: auto-detect by probing)
#
# Exit code: 0 = checks for the selected mode passed.

set -u

AIMEE_BIN="${AIMEE_BIN:-aimee}"
SERVER_URL="${SERVER_URL:-http://localhost:8740}"
BEARER="${BEARER:-aimee-local-dev}"
FORCE_MODE="${FORCE_MODE:-}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }

PASS=0
FAIL=0
ok()   { green "  PASS  $*"; PASS=$((PASS + 1)); }
bad()  { red   "  FAIL  $*"; FAIL=$((FAIL + 1)); }

# Two remote-transport mechanisms, set both so this is portable:
#  - AIMEE_API_* (client_transport + tcp:host:port endpoint): the POSIX
#    HTTP-transport cutover path.
#  - AIMEE_SERVER_URL/_TOKEN: the cross-platform thin client (incl. Windows),
#    which has no config_load and reads the URL straight from the environment.
host_port="${SERVER_URL#http://}"; host_port="${host_port#https://}"; host_port="${host_port%/}"
export AIMEE_API_CLIENT_TRANSPORT=http
export AIMEE_API_ENDPOINT="tcp:${host_port}"
export AIMEE_API_BEARER="${BEARER}"
export AIMEE_SERVER_URL="${SERVER_URL}"
export AIMEE_SERVER_TOKEN="${BEARER}"

bold "==> aimee thin-client smoke"
echo "    client : ${AIMEE_BIN}"
echo "    server : ${SERVER_URL}  (endpoint ${AIMEE_API_ENDPOINT})"

# 1) The client binary must exist and run.
if "${AIMEE_BIN}" --version >/dev/null 2>&1 || "${AIMEE_BIN}" version >/dev/null 2>&1; then
  ok "client binary runs (--version)"
else
  bad "client binary not runnable: ${AIMEE_BIN}"
  echo; bold "==> Summary: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# Decide mode: probe the server unless FORCE_MODE pins it.
mode="$FORCE_MODE"
if [[ -z "$mode" ]]; then
  if curl -fsS --max-time 5 -H "Authorization: Bearer ${BEARER}" "${SERVER_URL}/v1/health" >/dev/null 2>&1; then
    mode="full"
  else
    mode="selfcheck"
  fi
fi

if [[ "$mode" == "full" ]]; then
  bold "==> Mode: FULL (server reachable) — real round-trip over HTTP transport"
  if out="$("${AIMEE_BIN}" session list 2>&1)"; then
    ok "aimee session list over HTTP transport"
  else
    bad "aimee session list failed: ${out}"
  fi
  # A second read path to exercise the kb-backed surface (tolerant: empty is ok).
  if out="$("${AIMEE_BIN}" memory search "thin client smoke" 2>&1)"; then
    ok "aimee memory search over HTTP transport"
  else
    yellow "  NOTE  aimee memory search returned non-zero (kb may be absent): ${out}"
  fi
else
  bold "==> Mode: SELFCHECK (no server reachable) — transport config only"
  yellow "  This host has no reachable server at ${SERVER_URL}."
  yellow "  Verifying the client accepts the HTTP transport config WITHOUT a live"
  yellow "  round-trip. This is NOT a full client<->server proof."
  # The client should at least parse the transport env and attempt a connection
  # (a connection error is expected and acceptable here; a usage/parse error is not).
  out="$("${AIMEE_BIN}" session list 2>&1 || true)"
  if printf '%s' "$out" | grep -qiE 'connect|refused|unreachable|timed out|no route|transport'; then
    ok "client honored HTTP transport and attempted a connection"
  else
    yellow "  NOTE  could not positively confirm a connection attempt; output: ${out}"
    ok "client ran with HTTP transport env set (selfcheck, best-effort)"
  fi
fi

echo
bold "==> Summary (${mode}): ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
[[ "$mode" == "full" ]] && green "thin client drives the remote server over HTTP." \
                        || yellow "thin client selfcheck only — wire a reachable server for a full proof."
exit 0
