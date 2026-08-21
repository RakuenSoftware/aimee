#!/bin/bash
# Push binaries, module, configs and every validation script into the container,
# then bring the whole stack up. Run ON THE .252 HOST after provision-host.sh.
#
# Everything it needs must already be in /tmp on the host: bins.tgz, mm.tgz, the
# two yaml configs, and the fact-authority scripts.
# Usage: deploy-all.sh [CTID]
set -u
CT="${1:-9078}"
p() { pct push "$CT" "$1" "$2"; }
x() { pct exec "$CT" -- bash -lc "$1"; }

x 'mkdir -p /root/.config/aimee /usr/local/libexec/aimee-modules /root/pgtests'

p /tmp/bins.tgz /tmp/bins.tgz
p /tmp/mm.tgz   /tmp/mm.tgz
x 'cd /usr/local/bin && tar xzf /tmp/bins.tgz && chmod +x aimee aimee-server aimee-kb'
x 'cd /usr/local/libexec/aimee-modules && tar xzf /tmp/mm.tgz && chmod +x aimee-module-memory'

p /tmp/kb-aimee.yaml     /root/.config/aimee/aimee.yaml
p /tmp/server-aimee.yaml /root/aimee.yaml

for s in psql provision start-kb start-server reset-kb seed-facts \
         install-memory-module install-memory-module-server start-memory-module \
         restore-stack install-chat-provider \
         test-retract test-server-retract test-context-block test-provenance \
         test-memory-delete test-drain-supersede run-pg-tests; do
  [ -f "/tmp/${s}.sh" ] && p "/tmp/${s}.sh" "/root/${s}.sh"
done
x 'chmod +x /root/*.sh'

# Short aliases the earlier runs used, kept so the scripts call each other.
x 'cd /root && ln -sf install-memory-module.sh imm.sh; ln -sf install-memory-module-server.sh imms.sh;
   ln -sf start-memory-module.sh smm.sh; ln -sf restore-stack.sh restore.sh;
   ln -sf test-server-retract.sh t.sh; ln -sf test-context-block.sh tcb.sh;
   ln -sf test-provenance.sh tp.sh; ln -sf test-memory-delete.sh td.sh;
   ln -sf test-drain-supersede.sh tds.sh; ln -sf install-chat-provider.sh icp.sh'

x 'bash /root/provision.sh' 2>&1 | tail -2

# BOTH grants before EITHER daemon. A daemon reads modules.d once at startup, so
# a grant written afterwards is not seen and the module's attach is denied --
# which reads as a broken module rather than a stale policy.
x 'bash /root/install-memory-module.sh'        >/dev/null 2>&1
x 'bash /root/install-memory-module-server.sh' >/dev/null 2>&1

x 'bash /root/start-kb.sh'                   2>&1 | tail -1
x 'bash /root/start-memory-module.sh'        2>&1 | grep -E 'state:'
x 'bash /root/start-server.sh'               2>&1 | tail -1
x 'bash /root/install-memory-module-server.sh' 2>&1 | grep -E 'state:'
echo
x 'echo "daemons: kb=$(pgrep -cf aimee-kb) server=$(pgrep -cf aimee-server) modules=$(pgrep -cf aimee-module-memory)"'
