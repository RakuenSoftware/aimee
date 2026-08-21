#!/bin/bash
# Remove the bus-probe grants and bring both daemons + both module instances
# back to the verified working state.
#
# The probe's client grant was refused, and an unloadable grant does not merely
# get skipped: the whole module endpoint fails and the daemon exits ("obs_bus:
# module endpoint failed", then "server: shut down"). Worth knowing on its own --
# one bad file in modules.d takes the daemon with it.
# Run AS ROOT in the container.
set -u
rm -f /root/.config/aimee/modules.d/kb/bus-probe.grant
rm -f /root/modules.d/server/bus-probe.grant
echo "probe grants removed"

pkill -f aimee-module-memory 2>/dev/null
sleep 1
bash /root/start-kb.sh   >/dev/null 2>&1
bash /root/smm.sh        2>&1 | grep -E "state:"
bash /root/start-server.sh >/dev/null 2>&1
bash /root/imms.sh       2>&1 | grep -E "state:"

echo
echo "daemons:  kb=$(pgrep -cf aimee-kb) server=$(pgrep -cf aimee-server) modules=$(pgrep -cf aimee-module-memory)"
ls -la /root/.config/aimee/kb-module-bus.sock /root/server-module-bus.sock 2>&1 | tail -2
