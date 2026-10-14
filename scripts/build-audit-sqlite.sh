#!/bin/sh
set -eu
out=${1:?usage: build-audit-sqlite.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
out=$(cd "$out" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
curl -fsSL --retry 5 https://sqlite.org/2026/sqlite-autoconf-3510300.tar.gz -o "$build/sqlite.tar.gz"
printf '%s  %s\n' 81f5be397049b0cae1b167f2225af7646fc0f82e4a9b3c48c9ea3a533e21d77a "$build/sqlite.tar.gz" | sha256sum -c -
tar -xzf "$build/sqlite.tar.gz" -C "$build"
cc -O2 -fPIC -shared -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_THREADSAFE=1 \
   -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_DBSTAT_VTAB \
   -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_UNLOCK_NOTIFY \
   -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_USE_URI=1 \
   -Wl,-soname,libsqlite3.so.0 -o "$out/libsqlite3.so.0" \
   "$build/sqlite-autoconf-3510300/sqlite3.c" -lpthread -ldl -lm
