#!/bin/bash
# The end-state check: both daemons up, modules attached, and the store answering.
#
# `aimee status` is the right instrument here because it reports the KB's own
# view rather than the shell's: a daemon whose process exists but whose module
# bus is detached still shows a process to `pgrep`, and that difference is what
# made an earlier run look healthy while stages were silently absent.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
export AIMEE_HOME=/root AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock

echo "processes:  server=$(pgrep -cf /usr/local/bin/aimee-server) kb=$(pgrep -cf /usr/local/bin/aimee-kb) modules=$(pgrep -cf /usr/local/libexec/aimee-modules/)"
echo
/usr/local/bin/aimee status 2>&1 | head -12
echo
echo "typed-fact store:"
/root/psql.sh "select count(*) || ' live semantic edges' from entity_edges
                 where edge_class='semantic' and lifecycle_state in ('persistent','promoted')
                   and superseded_at='' and invalidated_at='' and suppressed=0" 2>/dev/null | tail -1
/root/psql.sh "select count(*) || ' embedding rows' from memory_embeddings" 2>/dev/null | tail -1
echo
echo "relation schema published: $(curl -s -m 20 -H "Authorization: Bearer $(cat /root/kb-bearer.txt)" \
  -H 'content-type: application/json' -X POST --data '{}' \
  http://127.0.0.1:8741/v1/actions/relations.schema_list \
  | python3 -c 'import json,sys
try: print(len(json.load(sys.stdin).get("rows") or []))
except Exception: print(0)') rules"
