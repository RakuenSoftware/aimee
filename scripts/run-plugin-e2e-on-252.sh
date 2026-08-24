#!/bin/sh
# Ship the plugin-module e2e binaries to the test host and run them there.
#
# The binaries are built locally and shipped: both hosts are x86_64 Linux. If the
# loader complains, build on the target instead.
#
# Scoped cleanup only. .252 runs a live aimee deployment of its own — never a
# blanket pkill, and never touch anything outside this run's directory.
set -eu

HOST="${HOST:-root@192.168.1.252}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/aimee-plugin-e2e}"
PKG=src/build/plugin-e2e.tgz

PROC=src/build/obj/tests/unit-test-bus-plugin-process
SCALE=src/build/obj/tests/unit-test-bus-plugin-scale
MODULE=src/build/obj/aimee-module

for f in "$PROC" "$SCALE" "$MODULE"; do
  [ -x "$f" ] || {
    echo "missing $f — run:" >&2
    echo "  make -C src build/obj/tests/unit-test-bus-plugin-process" >&2
    echo "  make -C src build/obj/tests/unit-test-bus-plugin-scale" >&2
    echo "  (cd server-go && go build -o ../src/build/obj/aimee-module ./cmd/aimee-module)" >&2
    exit 2
  }
done

# The pluggy leg needs pluggy itself plus the fixture modules. pluggy is pure
# Python, so it is VENDORED into the package rather than installed on the target:
# .252 hosts a live aimee deployment and this must not touch its system packages.
PLUGGYLIB=src/build/pluggylib
PLUGGY_EXTRA=""
if [ -d "$PLUGGYLIB" ]; then
  PLUGGY_EXTRA="$PLUGGYLIB src/tests/fixtures/pluggy scripts/aimee-pluggy-host.py src/tests/test_pluggy_host.py"
else
  echo "note: $PLUGGYLIB absent — the pluggy leg will be skipped on the target" >&2
fi

echo "== packaging =="
tar czf "$PKG" "$PROC" "$SCALE" "$MODULE" $PLUGGY_EXTRA

echo "== shipping to $HOST:$REMOTE_DIR =="
scp -q "$PKG" "$HOST:/tmp/aimee-plugin-e2e.tgz"
ssh "$HOST" "rm -rf $REMOTE_DIR; mkdir -p $REMOTE_DIR; tar xzf /tmp/aimee-plugin-e2e.tgz -C $REMOTE_DIR"

echo "== host =="
ssh "$HOST" "hostname; uname -m; python3 -V"

PLUGGY_ENV=""
if [ -n "$PLUGGY_EXTRA" ]; then
  PLUGGY_ENV="AIMEE_PLUGGY_HOST=$REMOTE_DIR/scripts/aimee-pluggy-host.py \
AIMEE_PLUGGY_PYTHONPATH=$REMOTE_DIR/$PLUGGYLIB:$REMOTE_DIR/src/tests/fixtures/pluggy"

  echo "== pluggy host (direct) =="
  ssh "$HOST" "PYTHONPATH=$REMOTE_DIR/$PLUGGYLIB python3 $REMOTE_DIR/src/tests/test_pluggy_host.py" \
    2>&1 | grep -E '^  (ok|FAIL)|^all |failure'
fi

echo "== process e2e =="
ssh "$HOST" "$PLUGGY_ENV $REMOTE_DIR/$PROC $REMOTE_DIR/$MODULE" 2>&1 | grep -E '^e2e:|^all '

echo "== scale / exploratory =="
ssh "$HOST" "$REMOTE_DIR/$SCALE $REMOTE_DIR/$MODULE" 2>&1 | grep -E '^scale:|^all '

echo "== leftover processes from this run =="
# Match the module executables themselves, not the run directory: a pattern
# containing $REMOTE_DIR also matches the shell running this very pgrep, which
# reads as a leak that is not one.
ssh "$HOST" "pgrep -fa 'aimee-module-mcp-' | grep -v 'pgrep' || echo none"

echo "== cleanup =="
ssh "$HOST" "rm -rf $REMOTE_DIR /tmp/aimee-plugin-e2e.tgz"
ssh "$HOST" "test -d $REMOTE_DIR && echo 'WARNING: cleanup failed' || echo 'cleanup ok'"
rm -f "$PKG"
