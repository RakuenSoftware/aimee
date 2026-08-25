#!/bin/bash
# The DB3 path against a REAL vector database.
#
# Installs Qdrant if it is not already running, then runs the two suites that
# need it:
#
#   1. the Qdrant backend's own live tests, which check the three places its
#      semantics do not line up with the DB3 contract -- scope filters,
#      tombstones, and the direction Euclid scores in. A fake happily confirms
#      whatever the client believes about those; only the server can settle them.
#
#   2. the shipped provider binary, as its own process, on a real bus, backed by
#      that Qdrant. This is the only path in the tree where a search leaves the
#      process, crosses the bus, is answered out of a real vector database, and
#      comes back.
#
# Run AS ROOT inside the verification container, after pg-setup.sh is not
# required -- this one needs no PostgreSQL.
set -uo pipefail
export LC_ALL=C LANG=C
TREE="${TREE:-/work/aimee}"
QDRANT_VERSION="${QDRANT_VERSION:-v1.12.4}"
QDRANT_URL="${QDRANT_URL:-http://127.0.0.1:6333}"
export GOCACHE="${GOCACHE:-/work/.gocache}"
export GOPATH="${GOPATH:-/work/.gopath}"

if ! curl -sf -o /dev/null "$QDRANT_URL/healthz"; then
   echo "--- installing Qdrant $QDRANT_VERSION ---"
   archive=/tmp/qdrant.tar.gz
   url="https://github.com/qdrant/qdrant/releases/download/$QDRANT_VERSION/qdrant-x86_64-unknown-linux-gnu.tar.gz"
   curl -sSL -o "$archive" "$url" || { echo "could not download $url"; exit 1; }
   tar xzf "$archive" -C /tmp
   install -m 0755 /tmp/qdrant /usr/local/bin/qdrant
   mkdir -p /var/lib/qdrant
   # Started from its storage directory: Qdrant writes ./storage relative to the
   # working directory, and a run from /tmp would lose its data on a reboot in a
   # way that looks like an empty corpus rather than a misplaced one.
   ( cd /var/lib/qdrant && nohup /usr/local/bin/qdrant >/var/log/qdrant.log 2>&1 & )
   for _ in $(seq 1 30); do
      curl -sf -o /dev/null "$QDRANT_URL/healthz" && break
      sleep 1
   done
fi

if ! curl -sf -o /dev/null "$QDRANT_URL/healthz"; then
   echo "Qdrant did not come up at $QDRANT_URL; see /var/log/qdrant.log"
   exit 1
fi
echo "--- Qdrant: $(curl -s "$QDRANT_URL" | head -c 120) ---"

# The C host harness is what gives the provider a real bus with a real grant.
make -C "$TREE/src" --no-print-directory db3-go-host >/dev/null 2>&1
harness="$TREE/src/build/obj/tests/db3-go-host"
[ -x "$harness" ] || { echo "the DB3 C host harness did not build"; exit 1; }

failed=0
cd "$TREE/server-go" || exit 1

echo "=== Qdrant backend, live ==="
AIMEE_TEST_QDRANT_URL="$QDRANT_URL" go test ./modules/vectordb/qdrant/ -count=1 -v -run Live
[ $? -eq 0 ] || failed=1

echo "=== shipped provider process, over the bus, backed by Qdrant ==="
DB3_GO_HOST="$harness" AIMEE_TEST_QDRANT_URL="$QDRANT_URL" \
   go test ./modules/db2 -count=1 -v -run TestTheShippedProviderBinary
[ $? -eq 0 ] || failed=1

echo "=== the postgres module routing over the bus, backed by Qdrant ==="
DB3_GO_HOST="$harness" AIMEE_TEST_QDRANT_URL="$QDRANT_URL" \
   go test ./modules/postgres -count=1 -v -run TestThePostgresModuleRoutes
[ $? -eq 0 ] || failed=1

echo "QDRANT-E2E-FAILED=$failed"
exit $failed
