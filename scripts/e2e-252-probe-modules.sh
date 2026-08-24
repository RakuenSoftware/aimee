#!/bin/sh
# Does the daemon spawn its modules itself, and does the store module attach?
#
# The store module answered "bus: attach denied" when started BY HAND against
# the daemon's socket. Two explanations fit that equally well and they need
# different responses:
#
#   the daemon already spawned its own aimee-module-aimee, so a second one is a
#   duplicate holder of principal ref 30 and is correctly denied  -- in which
#   case the module is already running and the manual start was the mistake
#
#   nothing spawned it and the grant does not admit it  -- a real failure
#
# So look at what is running rather than inferring from the refusal.
set -u

HOME_DIR=/var/lib/aimee-e2e-probe
STORE_URL=postgresql:///aimee_e2e_store?host=/var/run/postgresql

rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR/modules.d/server"
cp /opt/aimee/module-grants/server/*.grant "$HOME_DIR/modules.d/server/"
chmod 0700 "$HOME_DIR/modules.d" "$HOME_DIR/modules.d/server"
chmod 0600 "$HOME_DIR/modules.d/server/"*.grant

# The manifest is what tells the daemon which modules to SPAWN. Grants admit a
# module; the manifest starts it. Without it the daemon comes up and every store
# call reports the module unreachable, which is correct degraded behaviour and
# not a working system.
AIMEE_HOME="$HOME_DIR" AIMEE_STORE_URL="$STORE_URL" \
   AIMEE_MODULE_MANIFEST=/opt/aimee/module-grants/server.modules \
   /usr/local/bin/aimee-server >"$HOME_DIR/server.log" 2>&1 &
SRV=$!
sleep 14

echo "== module processes the daemon spawned =="
ps -eo pid,args | grep "[a]imee-module" | head -20
echo "        count: $(ps -eo args | grep -c '[a]imee-module')"

echo
echo "== is the store module among them =="
if ps -eo args | grep -q "[a]imee-module-aimee"; then
   echo "YES: the daemon spawned aimee-module-aimee"
else
   echo "NO: nothing is running aimee-module-aimee"
   echo "    what the server said about modules:"
   grep -iE "module|grant|spawn" "$HOME_DIR/server.log" | tail -12 | sed 's/^/      /'
fi

echo
echo "== store tables =="
su postgres -c "psql -tAc \"SELECT count(*) FROM information_schema.tables WHERE table_schema='public'\" -d aimee_e2e_store"

kill "$SRV" 2>/dev/null
sleep 2
kill -9 "$SRV" 2>/dev/null
pkill -f aimee-module 2>/dev/null
exit 0
