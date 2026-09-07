#!/bin/sh
# KB composition resource host. Shared entrypoint already validated identity,
# ingested Vault inputs and unlocked the standardized PostgreSQL service.
set -eu
ulimit -c 0 2>/dev/null || true
ulimit -s 65536 2>/dev/null || true
: "${AIMEE_HOME:?}"
export AIMEE_MODULE_BUS_SOCKET="${AIMEE_MODULE_BUS_SOCKET:-$AIMEE_HOME/kb-module-bus.sock}"
export AIMEE_EGRESS_CREDENTIAL_HELPER=/usr/local/bin/aimee-server
. /usr/local/bin/optional-modules-lib.sh
# >>> kb-module-grant-seeding
AIMEE_MODULE_GRANT_SRC="${AIMEE_MODULE_GRANT_SRC:-/opt/aimee/module-grants/kb}"
mkdir -p "$AIMEE_HOME/modules.d/kb/.seeded"
# The published 0.4.1 memory grant predates seed records and the read stage.
# Match its entire policy, and never treat a changed recorded grant as a default.
kb_historical_memory_grant() {
    [ ! -e "$3" ] || return 1
    [ "$(basename "$1")" = memory.grant ] || return 1
    [ "$(grep '^serve=' "$1" || true)" = 'serve=5889,5890,5891,5892,5893,5894' ] || return 1
    [ "$(sed '/^serve=/d' "$1")" = "$(sed '/^serve=/d' "$2")" ]
}
for source in "$AIMEE_MODULE_GRANT_SRC"/*.grant; do
    [ -f "$source" ] || continue
    name=$(basename "$source")
    dest="$AIMEE_HOME/modules.d/kb/$name"
    record="$AIMEE_HOME/modules.d/kb/.seeded/$name.sha256"
    if [ ! -e "$dest" ] || cmp -s "$source" "$dest" ||
       { [ -r "$record" ] && [ "$(sha256sum "$dest" | cut -d' ' -f1)" = "$(cat "$record")" ]; } ||
       kb_historical_memory_grant "$dest" "$source" "$record"; then
        cp "$source" "$dest"
        chmod 0600 "$dest"
        sha256sum "$dest" | cut -d' ' -f1 > "$record"
        chmod 0600 "$record"
    else
        printf '[kb-entrypoint] preserving operator policy in %s; it differs from the shipped grant\n' "$dest" >&2
    fi
done
# <<< kb-module-grant-seeding
manifest=$(apply_optional_modules kb "${AIMEE_MODULE_MANIFEST:-/opt/aimee/module-grants/kb.modules}" "$AIMEE_HOME")
module-supervisor.sh kb "$AIMEE_MODULE_BUS_SOCKET" "$manifest" &
modules=$!
aimee-kb --http-port="${AIMEE_KB_HTTP_PORT:-8741}" &
resource=$!
stop() {
    trap - TERM INT HUP
    kill -TERM "$resource" "$modules" 2>/dev/null || true
    attempts=0
    while { kill -0 "$resource" 2>/dev/null || kill -0 "$modules" 2>/dev/null; } && [ "$attempts" -lt 100 ]; do
        sleep 0.1
        attempts=$((attempts + 1))
    done
    kill -KILL "$resource" "$modules" 2>/dev/null || true
    wait "$resource" 2>/dev/null || true
    wait "$modules" 2>/dev/null || true
}
trap 'stop; exit 0' TERM INT HUP
while kill -0 "$resource" 2>/dev/null && kill -0 "$modules" 2>/dev/null; do sleep 0.2; done
status=0
if ! kill -0 "$resource" 2>/dev/null; then wait "$resource" || status=$?; else status=1; fi
stop
exit "$status"
