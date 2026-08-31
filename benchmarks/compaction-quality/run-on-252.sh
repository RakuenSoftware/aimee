#!/bin/sh
# Ship the retention probe + corpus to the test host and run them there.
#
# The probe is built locally and shipped as a binary: .252 has gcc, but building the
# whole tree there just to run one measurement is not worth it. Both hosts are x86_64
# Linux; if the loader complains, build on .252 instead.
#
# Scoped cleanup only. .252 runs a live aimee deployment of its own — never a blanket
# pkill, and never touch anything outside this run's directory.
set -eu

HOST="${HOST:-root@192.168.1.252}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/compaction-retention-run}"
PROBE="src/build/obj/tests/compaction-retention-probe"
CORPUS="benchmarks/compaction-quality/corpus.json"

[ -x "$PROBE" ] || { echo "missing $PROBE — run: make -C src compaction-retention-probe" >&2; exit 2; }
[ -f "$CORPUS" ] || { echo "missing $CORPUS" >&2; exit 2; }

echo "== shipping to $HOST:$REMOTE_DIR =="
tar czf - "$PROBE" "$CORPUS" | ssh "$HOST" "mkdir -p $REMOTE_DIR && tar xzf - -C $REMOTE_DIR"

echo "== host =="
ssh "$HOST" "hostname; uname -m; ldd --version | head -1"

echo "== run =="
ssh "$HOST" "cd $REMOTE_DIR && ./$PROBE $CORPUS"

echo "== cleanup =="
ssh "$HOST" "rm -rf $REMOTE_DIR"
ssh "$HOST" "ls -d $REMOTE_DIR 2>/dev/null && echo 'WARNING: cleanup failed' || echo 'cleanup ok'"
