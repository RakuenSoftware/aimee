#!/bin/bash
# Does the coord dispatcher start, and if not, whose fault is it?
#
# A sweep turned up one line in server.log:
#
#   ERROR coord_dispatcher: failed to persist boot claim owner; dispatcher not started
#
# server_coord_dispatcher_init() is called ONCE from server.c and has no retry:
# if db1_runtime_state_set() fails at that instant, the dispatcher is down for
# the entire process lifetime and this single line is the only evidence. So the
# question is whether DB1 was reachable when the server initialised.
#
# The suspicion is bring-up ORDER, not the product: run-suite's bring_up_server
# starts aimee-server and only then attaches its modules, so a server that needs
# the db1 module during init can lose the race. This distinguishes the two by
# restarting with the modules ALREADY attached and re-reading the log.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
rc=0

echo "=== A. restart with modules attached FIRST ==="
bash /root/install-config-module.sh grants >/dev/null 2>&1
bash /root/install-db1-module.sh grants    >/dev/null 2>&1
: > /root/server.log
bash /root/start-server.sh >/dev/null 2>&1
bash /root/imms.sh >/dev/null 2>&1
sleep 6
a_err="$(grep -ac 'failed to persist boot claim owner' /root/server.log 2>/dev/null | head -1)"
a_ok="$(grep -ac 'coord_dispatcher: started' /root/server.log 2>/dev/null | head -1)"
echo "  boot-claim errors: ${a_err:-0}    'started' lines: ${a_ok:-0}"
grep -a 'coord_dispatcher' /root/server.log 2>/dev/null | tail -3 | sed 's/^/    /'

echo
echo "=== B. the state the dispatcher tried to write ==="
sqlite3 /root/aimee.db \
  "SELECT key, substr(value,1,28) FROM runtime_state WHERE key='coord_dispatcher_boot_owner'" \
  2>/dev/null | sed 's/^/    /'

echo
if [ "${a_ok:-0}" -gt 0 ] && [ "${a_err:-0}" -eq 0 ]; then
  echo "PASS: with modules attached the dispatcher starts, so the earlier error was"
  echo "      a bring-up ORDER artifact -- aimee-server was initialising before the"
  echo "      db1 module it needs had attached -- and not a product defect."
elif [ "${a_err:-0}" -gt 0 ]; then
  echo "FAIL: the dispatcher still cannot persist its boot owner with modules up."
  echo "      That is the product, not the ordering, and it is silent after one line:"
  echo "      server_coord_dispatcher_init() is called once and never retried."
  rc=1
else
  echo "NOTE: no error and no 'started' line either -- the dispatcher may be"
  echo "      disabled in this configuration; nothing is claimed."
fi
exit $rc
