#!/bin/sh
# Shared deployed-fixture dependencies. Capabilities come only from the actual
# generated grants; this helper changes executable pins, never their authority.
WFF_RUNTIME_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
GRANT_DIR=${AIMEE_GRANT_DIR:-/opt/aimee/module-grants/server}
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-aimee}
PG_MODULE=${AIMEE_POSTGRES_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-postgres}
CONFIG_MODULE=${AIMEE_CONFIG_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-config}
SERVER=${AIMEE_SERVER_BIN:-/usr/local/bin/aimee-server}
WFE=${AIMEE_WFE_BIN:-/usr/local/bin/aimee-wfe}
WORKFLOWS=${AIMEE_WORKFLOWS_DIR:-${AIMEE_WORKFLOW_DIR:-/opt/aimee/defaults/workflows}}
GRANT=${AIMEE_DB1_GRANT:-$GRANT_DIR/aimee.grant}
WFE_GRANT=${AIMEE_WFE_GRANT:-$GRANT_DIR/wfe.grant}
WFF_CONFIG_PID=""
WFF_POSTGRES_PID=""

workflow_fixture_install_policy() {
   mkdir -p "$AIMEE_HOME/modules.d/server"
   for _wff_name in aimee aimee-postgres postgres config wfe workflows; do
      _wff_source="$GRANT_DIR/$_wff_name.grant"
      case "$_wff_name" in
      aimee) _wff_source="$GRANT"; _wff_exe="$MODULE" ;;
      aimee-postgres) _wff_exe="$MODULE" ;;
      postgres) _wff_source=${AIMEE_POSTGRES_GRANT:-$_wff_source}; _wff_exe="$PG_MODULE" ;;
      config) _wff_source=${AIMEE_CONFIG_GRANT:-$_wff_source}; _wff_exe="$CONFIG_MODULE" ;;
      wfe) _wff_source="$WFE_GRANT"; _wff_exe="$WFE" ;;
      workflows) _wff_source=${AIMEE_WORKFLOWS_GRANT:-$_wff_source}; _wff_exe="$WFE" ;;
      esac
      if [ ! -r "$_wff_source" ] || [ ! -x "$_wff_exe" ]; then
         echo "ABORT: missing generated $_wff_name grant or executable" >&2
         return 1
      fi
      # Same executable-pin adaptation as the canonical integration harness.
      sed "s|^executable=.*|executable=$_wff_exe|" "$_wff_source" \
         >"$AIMEE_HOME/modules.d/server/$_wff_name.grant" || return 1
   done
}

workflow_fixture_start_config() {
   _wff_i=0
   while [ "$_wff_i" -lt 300 ]; do
      [ -S "$BUS_SOCK" ] && break
      sleep 0.1
      _wff_i=$((_wff_i + 1))
   done
   [ -S "$BUS_SOCK" ] || return 1
   "$CONFIG_MODULE" "$BUS_SOCK" >"$AIMEE_HOME/config-module.log" 2>&1 &
   WFF_CONFIG_PID=$!
}

workflow_fixture_start_postgres() {
   if [ -z "${AIMEE_STORE_URL:-}" ] || [ -z "${AIMEE_STORE_MIGRATION_URL:-}" ]; then
      echo "ABORT: explicit runtime and migration store URLs are required" >&2
      return 1
   fi
   # The caller supplies a disposable database/schema and its already-scoped
   # runtime/migration credentials. This helper never grants SQL authority.
   "$PG_MODULE" "$BUS_SOCK" >"$AIMEE_HOME/postgres-module.log" 2>&1 &
   WFF_POSTGRES_PID=$!
}

workflow_fixture_stop_dependencies() {
   for _wff_pid in "$WFF_CONFIG_PID" "$WFF_POSTGRES_PID"; do
      [ -n "$_wff_pid" ] || continue
      kill "$_wff_pid" 2>/dev/null || true
      wait "$_wff_pid" 2>/dev/null || true
   done
   WFF_CONFIG_PID=""
   WFF_POSTGRES_PID=""
}

workflow_fixture_stage_ready() {
   _wff_code=$(curl -s --max-time 2 -o "$AIMEE_HOME/workflow-defs.json" -w '%{http_code}' \
      --unix-socket "$HTTP_SOCK" http://localhost/v1/workflow/defs) || return 1
   [ "$_wff_code" = 200 ] || return 1
   python3 - "$AIMEE_HOME/workflow-defs.json" "${1:-}" <<'PY'
import json, sys
with open(sys.argv[1]) as stream:
    value = json.load(stream)
rows = value.get("defs") if isinstance(value, dict) else None
assert isinstance(rows, list)
if sys.argv[2]:
    assert any(isinstance(row, dict) and row.get("name") == sys.argv[2] and row.get("valid") is True for row in rows)
PY
}

workflow_fixture_register_workspace() {
   _wff_body=$(python3 - "$1" <<'PY'
import json, sys
print(json.dumps(dict(root=sys.argv[1], provider="shared", scan=False)))
PY
   ) || return 1
   _wff_code=$(curl -s --max-time 20 -o "$AIMEE_HOME/workspace-registration.json" -w '%{http_code}' \
      --unix-socket "$HTTP_SOCK" -H 'Content-Type: application/json' -d "$_wff_body" \
      http://localhost/v1/workspaces) || return 1
   [ "$_wff_code" = 200 ] || return 1
   python3 - "$AIMEE_HOME/workspace-registration.json" <<'PY'
import json, sys
with open(sys.argv[1]) as stream:
    value = json.load(stream)
assert isinstance(value, dict) and value.get("status") == "ok"
PY
}

# Independent read-only schema oracle. This is a validation client, not a new
# application SQL owner; only the fixture schema's metadata is inspected.
workflow_fixture_schema_snapshot() {
   python3 "$WFF_RUNTIME_DIR/workflow-fixture-schema.py" "$1"
}

workflow_fixture_sql_holders() {
   _wff_port=$(python3 - <<'PYPORT'
import os, urllib.parse
print(urllib.parse.urlsplit(os.environ["AIMEE_STORE_URL"]).port or 5432)
PYPORT
   ) || return 1
   for _wff_pid in ${MPID:-} ${SPID:-} ${WPID:-} ${MODULE_PID:-} ${SERVER_PID:-} ${WFE_PID:-} \
       ${WFF_CONFIG_PID:-} ${WFF_POSTGRES_PID:-}; do
      if ss -tnp 2>/dev/null | grep ":$_wff_port " | grep -q "pid=$_wff_pid,"; then
         printf '%s(%s) ' "$(tr -d '\0' <"/proc/$_wff_pid/comm" 2>/dev/null)" "$_wff_pid"
      fi
   done
}

workflow_fixture_stop_runtime() {
   # Keep configuration and SQL owners available while the admitted core drains.
   for _wff_pid in "$1" "$3" "$2"; do
      [ -n "$_wff_pid" ] || continue
      kill "$_wff_pid" 2>/dev/null || true
      wait "$_wff_pid" 2>/dev/null || true
   done
   workflow_fixture_stop_dependencies
   WPID=""; WPID2=""; MPID=""; SPID=""
   WFE_PID=""; MODULE_PID=""; SERVER_PID=""
}
