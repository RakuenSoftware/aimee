#!/bin/bash
# Bring a FRESH container all the way up, from `pct create` to a running stack.
#
# WHY THIS EXISTS. Everything in this directory was validated against one
# container that accumulated state across a long session: schema migrations
# already applied, certificates issued, grants written, an enrollment claimed,
# modules attached, facts seeded. "It works" on that box is a claim about a
# hand-built deployment, not about a clean install -- and given how many ordering
# traps this suite already documents (grants before daemons, db1 before the mTLS
# ramp, the embedder before the kb), assuming a fresh bring-up is smooth would be
# exactly the kind of inference this record keeps having to retract.
#
# So this runs the whole path from nothing and reports where it stops.
#
# Usage: bootstrap-fresh.sh <CTID> [SOURCE_CTID]
#   SOURCE_CTID supplies the two aimee.yaml files, which deploy-all.sh expects to
#   already exist in /tmp on the host. They are configuration, not state.
# Run ON THE PROXMOX HOST.
set -u
export LC_ALL=C
CT="${1:?usage: bootstrap-fresh.sh <CTID> [SOURCE_CTID]}"
SRC="${2:-9078}"

echo "=== configs from CT $SRC (configuration, not accumulated state) ==="
pct exec "$SRC" -- cat /root/.config/aimee/aimee.yaml > /tmp/kb-aimee.yaml 2>/dev/null
pct exec "$SRC" -- cat /root/aimee.yaml > /tmp/server-aimee.yaml 2>/dev/null
for f in /tmp/kb-aimee.yaml /tmp/server-aimee.yaml; do
  [ -s "$f" ] || { echo "FAIL: $f is empty; deploy-all.sh needs both" >&2; exit 1; }
  echo "  $f ($(wc -l < "$f") lines)"
done

echo
echo "=== deploy ==="
bash /tmp/deploy-all.sh "$CT" 2>&1 | tail -12

echo
echo "=== what came up ==="
pct exec "$CT" -- bash -lc '
  export LC_ALL=C
  echo "  daemons: kb=$(pgrep -cf /usr/local/bin/aimee-kb) server=$(pgrep -cf /usr/local/bin/aimee-server)"
  echo "  kb-bus modules:     $(pgrep -cf "aimee-modules/aimee-module-.* /root/.config/aimee/kb-module-bus.sock")"
  echo "  server-bus modules: $(pgrep -cf "aimee-modules/aimee-module-.* /root/server-module-bus.sock")"
  ls /root/aimee-http.sock >/dev/null 2>&1 && echo "  server /v1 socket: present" || echo "  server /v1 socket: ABSENT"
  curl -s -m 5 http://127.0.0.1:8741/v1/health >/dev/null 2>&1 && echo "  kb http: answering" || echo "  kb http: NOT answering"
'
