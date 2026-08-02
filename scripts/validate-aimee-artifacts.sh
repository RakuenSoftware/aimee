#!/usr/bin/env bash
# Runs INSIDE CT 132: artifact-level validation of the clean-room build.
#
# Source greps prove nothing CALLS a function; the linker proves the capability
# is not in the artifact. The release link strips symbols (-s), so symbol checks
# go through the unstripped objects, and behavioural strings through the binary.
set -uo pipefail
ROOT=/opt/aimee/tree
cd "$ROOT/src" || exit 90

PASS=0; FAIL=0
ck() { if [ "$2" = "1" ]; then printf '  PASS  %s\n' "$1"; PASS=$((PASS+1));
       else printf '  FAIL  %s\n     -> %s\n' "$1" "${3-}"; FAIL=$((FAIL+1)); fi; }

echo "=== object symbols (aimee-server object set) ==="

# The four kb purge client wrappers must not be defined or referenced anywhere.
hits=$(find build/obj -name "*.o" -print0 | xargs -0 nm -A 2>/dev/null | grep -cE "kb_client_purge_(project|heartbeat|finalize|cancel)_json" || true)
ck "no kb purge client wrappers in ANY object" "$([ "${hits:-0}" -eq 0 ] && echo 1 || echo 0)" "found ${hits}"

# The removed per-user workspace accessor must be gone.
hits=$(find build/obj -name "*.o" -print0 | xargs -0 nm -A 2>/dev/null | grep -c "ws_scope_webusers_base" || true)
ck "no ws_scope_webusers_base in ANY object" "$([ "${hits:-0}" -eq 0 ] && echo 1 || echo 0)" "found ${hits}"

# Its replacement must be present.
hits=$(find build/obj -name "*.o" -print0 | xargs -0 nm -A 2>/dev/null | grep -c "ws_scope_environment_root" || true)
ck "ws_scope_environment_root IS present" "$([ "${hits:-0}" -ge 1 ] && echo 1 || echo 0)" "absent"

# git_project_delete must have the new 4-arg shape: no result struct symbol left.
hits=$(find build/obj -name "*.o" -print0 | xargs -0 nm -A 2>/dev/null | grep -c "GP_ERR_KB_UNAVAILABLE" || true)
ck "no GP_ERR_KB_UNAVAILABLE symbol" "$([ "${hits:-0}" -eq 0 ] && echo 1 || echo 0)" "found ${hits}"

echo ""
echo "=== behavioural strings in the shipped binaries ==="

need() { # need BINARY NEEDLE LABEL
  if strings "$1" 2>/dev/null | grep -qF -- "$2"; then ck "$3" 1; else ck "$3" 0 "string absent from $1"; fi
}
absent() { # absent BINARY NEEDLE LABEL
  if strings "$1" 2>/dev/null | grep -qF -- "$2"; then ck "$3" 0 "string STILL PRESENT in $1"; else ck "$3" 1; fi
}

need   "$ROOT/aimee-kb" "requires the owner credential"        "aimee-kb: owner-required refusal present"
need   "$ROOT/aimee-kb" "cannot ingest all projects"           "aimee-kb: ingest-all refusal present"
need   "$ROOT/aimee-kb" "knowledge maintenance"                "aimee-kb: maintenance gate reason present"
need   "$ROOT/aimee-kb" "service"                              "aimee-kb: service scope kind present"

need   "$ROOT/aimee-server" "webuser_project_delete_audit_v1 schema_version=2 delete_id=" \
                                                                "aimee-server: delete audit is v2/delete_id"
absent "$ROOT/aimee-server" "schema_version=1 purge_id="        "aimee-server: no v1 purge_id audit line"
absent "$ROOT/aimee-server" "/v1/maintenance/purge-project"     "aimee-server: cannot address kb purge-project"
absent "$ROOT/aimee-server" "purge fence"                       "aimee-server: no fence protocol strings"

# aimee-server addresses NO destructive maintenance route. repair/clear went with
# their wrappers (their CLI commands were registered but unreachable); purge went
# with the delete-path rework. Maintenance on aimee-kb is administrative there.
absent "$ROOT/aimee" "/v1/maintenance/repair"                   "aimee CLI: cannot address kb repair"
absent "$ROOT/aimee" "/v1/maintenance/clear"                    "aimee CLI: cannot address kb clear"
absent "$ROOT/aimee" "/v1/maintenance/purge-project"            "aimee CLI: cannot address kb purge-project"
# kb_client_reconcile_json is retained in SOURCE by choice, but its only caller
# (`aimee memory vector reconcile`) is unreachable through the live dispatcher,
# so the linker discards it too — no binary can address the route. Asserted so
# the day someone wires that command back up, this check fails and says so.
absent "$ROOT/aimee-server" "/v1/maintenance/reconcile"         "reconcile GC'd (caller unreachable)"
absent "$ROOT/aimee" "/v1/maintenance/reconcile"                "reconcile GC'd from the CLI too"

echo ""
echo "binaries: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
