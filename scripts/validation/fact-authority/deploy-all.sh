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

# The Postgres-backed test binaries, when they have been staged. Without these a
# rebuilt container silently loses run-pg-tests.sh -- the one thing that
# exercises the class-rank guard on the engine that actually runs it.
if [ -f /tmp/pgt.tgz ]; then
  p /tmp/pgt.tgz /tmp/pgt.tgz
  [ -f /tmp/pgsql.tgz ] && p /tmp/pgsql.tgz /tmp/pgsql.tgz
  x 'cd /root/pgtests && tar xzf /tmp/pgt.tgz && { [ -f /tmp/pgsql.tgz ] && tar xzf /tmp/pgsql.tgz; }; chmod +x unit-test-* db2-test-template 2>/dev/null'
fi

# The memory module binary and the capture proxy, when staged.
[ -f /tmp/logging-proxy.py ] && p /tmp/logging-proxy.py /root/logging-proxy.py

p /tmp/kb-aimee.yaml     /root/.config/aimee/aimee.yaml
p /tmp/server-aimee.yaml /root/aimee.yaml

for s in psql provision start-kb start-server reset-kb seed-facts \
         install-memory-module install-memory-module-server start-memory-module \
         restore-stack install-chat-provider make-scope-repo \
         start-logging-proxy show-upstream-prompt probe-query-pollution \
         test-rerank-live test-retrieve-live test-chat-memory-stages \
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
   ln -sf test-drain-supersede.sh tds.sh; ln -sf install-chat-provider.sh icp.sh;
   ln -sf start-logging-proxy.sh slp.sh; ln -sf show-upstream-prompt.sh sup.sh;
   ln -sf test-rerank-live.sh trl.sh; ln -sf test-retrieve-live.sh trv.sh;
   ln -sf test-chat-memory-stages.sh tcms.sh; ln -sf probe-query-pollution.sh pqp.sh'

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
