#!/bin/bash
# Admit the validation-only live-bus probe on both daemon buses.
set -u
BIN=/root/aimee-memory-bus-probe
[ -x "$BIN" ] || { echo "memory bus probe missing: $BIN" >&2; exit 1; }
for placement in kb server; do
  case "$placement" in
    kb) dir=/root/.config/aimee/modules.d/kb ;;
    server) dir=/root/modules.d/server ;;
  esac
  mkdir -p "$dir"
  cat > "$dir/memory-e2e-probe.grant" <<EOF
version=1
principal_class=1
principal_ref=200
uid=self
executable=$BIN
publish=
subscribe=
request=5895
serve=
EOF
done
echo "memory live-bus probe grants installed"
