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
mkdir -p "$AIMEE_HOME/modules.d/kb"
mkdir -p "$AIMEE_HOME/modules.d/kb/.seeded"
# Installed grants are owned by the image. Keep explicit operator overrides;
# existing seeded grants are refreshed only when their contents are unchanged.
for source in /opt/aimee/module-grants/kb/*.grant; do
    name=$(basename "$source")
    dest="$AIMEE_HOME/modules.d/kb/$name"
    record="$AIMEE_HOME/modules.d/kb/.seeded/$name.sha256"
    if [ ! -e "$dest" ] || { [ -r "$record" ] && [ "$(sha256sum "$dest" | cut -d' ' -f1)" = "$(cat "$record")" ]; }; then
        cp "$source" "$dest"
        chmod 0600 "$dest"
        sha256sum "$dest" | cut -d' ' -f1 > "$record"
    fi
done
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
