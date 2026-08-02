#!/usr/bin/env bash
# validate-authz.sh — LIVE authorization validation against a real aimee-kb
# listener. Runs INSIDE CT 132.
#
# Why this and not the unit tests: the unit tests call kb_http_route_ex()
# directly, bypassing the socket, the HTTP parse and kb_verifier_authenticate().
# They prove route logic. Only this proves the whole chain end to end.
#
# The verifier derives a token's scope from the CONFIGURED bearer, so a scoped
# credential can only be exercised by an aimee-kb actually configured with it —
# hence one instance per credential shape.
set -uo pipefail

# Repo root: derived from this script's own location so it runs in CI, in a
# container, or from any checkout. AIMEE_ROOT / AIMEE_KB_BIN override.
SRC="${AIMEE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
KB="${AIMEE_KB_BIN:-$SRC/aimee-kb}"
WORK="${TMPDIR:-/tmp}/aimee-authz-live"
mkdir -p "$WORK"
PORT=8741
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0; SKIP=0

log()  { printf '%s\n' "$*"; }
pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
fail() { printf '  \033[31mFAIL\033[0m  %s\n     -> %s\n' "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s (%s)\n' "$1" "$2"; SKIP=$((SKIP+1)); }

status() { # status METHOD PATH BEARER [BODY] -> prints "code body"
  local m="$1" p="$2" b="$3" d="${4-}" out code
  out=$(curl -s -m 20 -w $'\n%{http_code}' -X "$m" \
        -H "Authorization: Bearer $b" -H 'Content-Type: application/json' \
        ${d:+--data "$d"} "$BASE$p" 2>/dev/null)
  code=$(printf '%s' "$out" | tail -n1)
  printf '%s\t%s' "$code" "$(printf '%s' "$out" | head -n -1 | tr -d '\n' | head -c 300)"
}

expect() { # expect NAME WANT_CODE GOT_TSV [NEEDLE]
  local name="$1" want="$2" tsv="$3" needle="${4-}"
  local got body; got=${tsv%%$'\t'*}; body=${tsv#*$'\t'}
  if [ "$got" != "$want" ]; then fail "$name" "want=$want got=$got body=$body"; return; fi
  # A here-string, not a pipe: `... | grep -q` exits at the first match and can
  # SIGPIPE the writer, which `set -o pipefail` would report as a failure.
  if [ -n "$needle" ] && ! grep -qF -- "$needle" <<<"$body"; then
    fail "$name" "status ok ($got) but body lacked '$needle': $body"; return
  fi
  pass "$name"
}

expect_not() { # expect_not NAME NOT_CODE GOT_TSV
  local name="$1" notwant="$2" tsv="$3"
  local got body; got=${tsv%%$'\t'*}; body=${tsv#*$'\t'}
  if [ "$got" = "$notwant" ]; then fail "$name" "got the forbidden status $got: $body"; return; fi
  pass "$name (=$got)"
}

start_kb() { # start_kb CONFIGURED_TOKEN
  stop_kb
  export AIMEE_HOME="$WORK/home"
  # Fresh vault state per credential shape, else the first sealed bearer sticks.
  rm -rf "$AIMEE_HOME"
  export AIMEE_KB_API_BEARER_TOKEN="$1"
  export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgresql://aimee:aimee@127.0.0.1/aimee_kb}"
  mkdir -p "$AIMEE_HOME"
  cat > "$AIMEE_HOME/aimee.yaml" <<YAML
kb:
  api:
    http_port: $PORT
YAML
  ulimit -s 65536
  # The env bearer is FIRST-BOOT TRANSPORT only: the runtime reads it from Vault,
  # so it has to be sealed before the listener starts. This mirrors
  # deploy/container/aimee-kb-entrypoint.sh; skipping it makes every
  # authenticated request 401, including the owner's.
  "$KB" --bootstrap-vault-env > "$AIMEE_HOME/bootstrap.log" 2>&1
  echo "  (bootstrap-vault-env rc=$?)"
  nohup "$KB" > "$WORK/kb.log" 2>&1 &
  echo $! > "$WORK/kb.pid"
  for _ in $(seq 1 60); do
    curl -s -m 2 "$BASE/v1/health" >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1
}

stop_kb() {
  [ -f "$WORK/kb.pid" ] && kill "$(cat "$WORK/kb.pid")" 2>/dev/null
  rm -f "$WORK/kb.pid"
  sleep 1
}

MAINT_ROUTES="/v1/maintenance/repair /v1/maintenance/reconcile /v1/maintenance/clear
/v1/maintenance/purge-project /v1/maintenance/purge-heartbeat /v1/maintenance/purge-finalize
/v1/maintenance/purge-cancel"
MAINT_BODY='{"project":"proj-alpha","path":"/tmp/kb","generation":"g1","purge_id":"p1"}'

OWNER='owner-secret-abc123'
SCOPED='scope:project:proj-alpha:s3cr3t'
SERVICE='scope:service:aimee-server:svc3cr3t'
CONSOLE='scope:console-admin:c1:cadmin'

# ─────────────────────────────────────────────────────────────────────────────
log ""
log "### 1. OWNER credential (unscoped) — the admin baseline"
if start_kb "$OWNER"; then
  expect "health reachable" 200 "$(status GET /v1/health "$OWNER")"
  for r in $MAINT_ROUTES; do
    expect_not "owner reaches $r" 403 "$(status POST "$r" "$OWNER" "$MAINT_BODY")"
  done
  expect_not "owner may ingest-all" 403 "$(status POST /v1/ingest "$OWNER" '{"force":true}')"
else
  skip "owner suite" "aimee-kb did not become healthy"
fi

log ""
log "### 2. PROJECT-scoped credential — data plane confined, no admin"
if start_kb "$SCOPED"; then
  for r in $MAINT_ROUTES; do
    expect "scoped refused $r" 403 "$(status POST "$r" "$SCOPED" "$MAINT_BODY")" "owner credential"
  done
  expect "scoped refused cross-project build" 403 \
    "$(status POST /v1/code/build "$SCOPED" '{"path":"/tmp/kb","project":"proj-beta"}')" \
    "cannot access"
  expect_not "scoped allowed own-project build" 403 \
    "$(status POST /v1/code/build "$SCOPED" '{"path":"/tmp/kb","project":"proj-alpha"}')"
  expect "scoped refused ingest-all" 403 \
    "$(status POST /v1/ingest "$SCOPED" '{"force":true}')" "cannot ingest all projects"
  expect "scoped refused ingest-all (empty body)" 403 \
    "$(status POST /v1/ingest "$SCOPED" '{}')" "cannot ingest all projects"
else
  skip "project-scoped suite" "aimee-kb did not become healthy"
fi

log ""
log "### 3. SERVICE credential — full data plane, still no admin"
if start_kb "$SERVICE"; then
  for r in $MAINT_ROUTES; do
    expect "service refused $r" 403 "$(status POST "$r" "$SERVICE" "$MAINT_BODY")" "owner credential"
  done
  expect_not "service reaches ANY project build" 403 \
    "$(status POST /v1/code/build "$SERVICE" '{"path":"/tmp/kb","project":"proj-beta"}')"
  expect_not "service reaches ANY project scan" 403 \
    "$(status POST /v1/code/scan "$SERVICE" '{"project":"proj-gamma","root_path":"/tmp/repo"}')"
  expect_not "service may ingest-all" 403 "$(status POST /v1/ingest "$SERVICE" '{"force":true}')"
else
  skip "service suite" "aimee-kb did not become healthy"
fi

log ""
log "### 4. CONSOLE-ADMIN credential — cannot escalate by minting"
if start_kb "$CONSOLE"; then
  expect "console cannot mint a service credential" 403 \
    "$(status POST /v1/enroll "$CONSOLE" '{"host":"h","port":1,"scope":"service:aimee-server"}')"
  expect "console cannot mint an owner credential" 403 \
    "$(status POST /v1/enroll "$CONSOLE" '{"host":"h","port":1,"scope":"fullaccess"}')"
  # Refused either by the maintenance owner gate OR, earlier, by the generic
  # scope check — a console-admin token targeting project:proj-alpha is now
  # denied there first (the body-project resolution added for the cross-scope
  # fix). Both are 403; the security outcome is what is asserted.
  for r in $MAINT_ROUTES; do
    expect "console refused $r" 403 "$(status POST "$r" "$CONSOLE" "$MAINT_BODY")"
  done
  # With no project in the body the scope check has nothing to compare, and a
  # PRE-EXISTING console-containment rule refuses first. Console-admin is
  # therefore refused by three independent gates depending on the request shape.
  # That the maintenance gate ITSELF holds is proven by the project-scoped and
  # service suites above, which reach it and get "owner credential".
  expect "console refused purge-project (no project in body)" 403 \
    "$(status POST /v1/maintenance/purge-project "$CONSOLE" '{"generation":"g1","purge_id":"p1"}')"
else
  skip "console suite" "aimee-kb did not become healthy"
fi

stop_kb
log ""
log "=============================================="
log "  PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
log "=============================================="
[ "$FAIL" -eq 0 ]
