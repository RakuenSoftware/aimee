#!/bin/sh
# Compatibility launcher for the unified application image. PostgreSQL is a
# supervised sibling service; this launcher never initializes an embedded DB.
set -eu
: "${AIMEE_INSTANCE_ROLE:=kb}"
if [ "$AIMEE_INSTANCE_ROLE" != kb ]; then
    printf 'KB launcher requires the immutable KB instance role\n' >&2
    exit 2
fi
export AIMEE_INSTANCE_ROLE
# Replace this process immediately so Vault ingestion and environment erasure
# happen in the canonical entrypoint before any unrelated child is started.
exec /usr/local/bin/aimee-server-entrypoint "$@"
