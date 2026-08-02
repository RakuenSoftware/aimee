#!/usr/bin/env bash
# Runs INSIDE CT 132: exercise the project lifecycle end to end against the real
# binaries, covering the claims this branch makes that nothing has actually run.
#
# Specifically: that a delete is LOCAL — it must succeed with aimee-kb entirely
# unreachable. Every prior check of that was structural (no symbol, no caller);
# this is the behavioural one.
set -uo pipefail

ROOT=/opt/aimee/tree
PASS=0; FAIL=0
ck() { if [ "$2" = 1 ]; then printf '  PASS  %s\n' "$1"; PASS=$((PASS+1));
       else printf '  FAIL  %s\n     -> %s\n' "$1" "${3-}"; FAIL=$((FAIL+1)); fi; }

HOME_DIR=$(mktemp -d /tmp/aimee-e2e-XXXX)
export AIMEE_HOME="$HOME_DIR"
export AIMEE_WORKSPACES_DIR="$HOME_DIR/ws"
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=protocol.file.allow GIT_CONFIG_VALUE_0=always
mkdir -p "$AIMEE_WORKSPACES_DIR"

# A local source repo to clone from — no network, no credentials.
SRC="$HOME_DIR/srcrepo"
mkdir -p "$SRC"
( cd "$SRC" && git init -q && git config user.email t@t && git config user.name t \
  && echo hello > README.md && git add . && git commit -qm init ) >/dev/null 2>&1

echo "=== 1. environment root is shared by every actor ==="
A=$("$ROOT/src/build/obj/tests/unit-test-workspace-scope" >/dev/null 2>&1; echo $?)
ck "workspace scope suite passes against this tree" "$([ "$A" = 0 ] && echo 1 || echo 0)" "exit $A"

echo ""
echo "=== 2. legacy migration moves real data (no aimee-kb involved) ==="
B=$("$ROOT/src/build/obj/tests/unit-test-workspace-migration" >/dev/null 2>&1; echo $?)
ck "migration suite passes" "$([ "$B" = 0 ] && echo 1 || echo 0)" "exit $B"

echo ""
echo "=== 3. project lifecycle with aimee-kb DOWN (nothing listening on 8741) ==="
if curl -s -m 2 http://127.0.0.1:8741/v1/health >/dev/null 2>&1; then
  echo "  (aimee-kb is up; stopping it so the delete path is proven offline)"
  pkill -f 'aimee-kb' 2>/dev/null
  sleep 2
fi
if curl -s -m 2 http://127.0.0.1:8741/v1/health >/dev/null 2>&1; then
  ck "aimee-kb is unreachable" 0 "still responding"
else
  ck "aimee-kb is unreachable" 1
fi

export AIMEE_KB_API_URL="http://127.0.0.1:8741"   # points at nothing
C=$("$ROOT/src/build/obj/tests/unit-test-git-project" >/dev/null 2>&1; echo $?)
ck "clone + delete lifecycle passes with kb down" "$([ "$C" = 0 ] && echo 1 || echo 0)" "exit $C"

echo ""
echo "=== 4. the delete audit really emits schema_version=2 ==="
OUT=$("$ROOT/src/build/obj/tests/unit-test-git-project" 2>&1 | grep -c 'webuser_project_delete_audit_v1 schema_version=2 delete_id=')
ck "v2 audit lines emitted at runtime ($OUT seen)" "$([ "${OUT:-0}" -ge 1 ] && echo 1 || echo 0)" "none seen"
OLD=$("$ROOT/src/build/obj/tests/unit-test-git-project" 2>&1 | grep -c 'schema_version=1 purge_id=')
ck "no v1 purge_id audit lines at runtime" "$([ "${OLD:-0}" -eq 0 ] && echo 1 || echo 0)" "$OLD seen"

echo ""
echo "=== 5. registry entry format on disk is v2 ==="
REG=$(find "$AIMEE_WORKSPACES_DIR" -path '*/.registry/*' -type f 2>/dev/null | head -1)
if [ -n "$REG" ]; then
  head -c 2 "$REG" | grep -q '^2 ' && ck "registry entry starts '2 '" 1 || ck "registry entry starts '2 '" 0 "$(head -c 40 "$REG")"
else
  echo "  SKIP  no registry entry left after the suite's own cleanup"
fi

rm -rf "$HOME_DIR"
echo ""
echo "e2e: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
