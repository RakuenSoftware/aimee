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
# aimee-module-postgres is created here, at unpack time, because its GRANT names
# it and a grant whose executable does not resolve is invalid -- which fails the
# whole module endpoint and stops aimee-kb starting. Creating it lazily inside
# install-postgres-module.sh was enough on a container that had run before and
# not enough on a fresh one.
x 'cd /usr/local/libexec/aimee-modules && tar xzf /tmp/mm.tgz && chmod +x aimee-module-memory aimee-module-config aimee-module-db1 2>/dev/null; \
   [ -x aimee-module-postgres ] || cp -f aimee-module-memory aimee-module-postgres; \
   chmod +x aimee-module-postgres 2>/dev/null; true'

# The Postgres-backed test binaries, when they have been staged. Without these a
# rebuilt container silently loses run-pg-tests.sh -- the one thing that
# exercises the class-rank guard on the engine that actually runs it.
if [ -f /tmp/pgt.tgz ]; then
  p /tmp/pgt.tgz /tmp/pgt.tgz
  [ -f /tmp/pgsql.tgz ] && p /tmp/pgsql.tgz /tmp/pgsql.tgz
  x 'cd /root/pgtests && tar xzf /tmp/pgt.tgz && { [ -f /tmp/pgsql.tgz ] && tar xzf /tmp/pgsql.tgz; }; chmod +x unit-test-* db2-test-template 2>/dev/null'
fi

# The trust-chain rig, when staged. test-account-tcp-authority.sh needs it to
# provision the management JWKS chain and mint identity tokens; without it that
# probe fails with "not installed", which on a fresh box is a deployment gap
# rather than anything about the code.
[ -f /tmp/write-tier-enforce-live ] && {
  p /tmp/write-tier-enforce-live /usr/local/bin/write-tier-enforce-live
  x 'chmod +x /usr/local/bin/write-tier-enforce-live'
}

# The memory module binary and the capture proxy, when staged.
[ -f /tmp/logging-proxy.py ] && p /tmp/logging-proxy.py /root/logging-proxy.py

p /tmp/kb-aimee.yaml     /root/.config/aimee/aimee.yaml
p /tmp/server-aimee.yaml /root/aimee.yaml

# EVERY staged script and helper, not a hand-maintained list.
#
# This was an explicit allowlist of names, and it drifted: make-mtls-certs,
# enroll-first-user, provision-mgmt-trust, make-oidc-idp, set-mtls-mode,
# test-embed-stage, stub-embedder.py, run-suite and others were all added to the
# directory and never added here. On the container that had been running all
# session it did not matter, because they had been copied by hand. On a fresh one
# the suite could not be prepared at all -- half the steps reported "No such file
# or directory".
#
# A list that has to be updated by hand is a list that will be wrong, and its
# being wrong is invisible until someone starts from nothing. Copying whatever is
# staged cannot drift.
for f in /tmp/*.sh /tmp/*.py; do
  [ -e "$f" ] || continue
  b="$(basename "$f")"
  case "$b" in
    provision-host.sh|deploy-all.sh|bootstrap-fresh.sh|keepalive-loop.sh) continue ;;  # host-side
    test-retract-remote.sh) continue ;;                                               # host-side by design
  esac
  p "$f" "/root/${b}"
done

x 'chmod +x /root/*.sh'

# Short aliases the earlier runs used, kept so the scripts call each other.
x 'cd /root && ln -sf install-memory-module.sh imm.sh; ln -sf install-memory-module-server.sh imms.sh;
   ln -sf start-memory-module.sh smm.sh; ln -sf restore-stack.sh restore.sh;
   ln -sf test-server-retract.sh t.sh; ln -sf test-context-block.sh tcb.sh;
   ln -sf test-provenance.sh tp.sh; ln -sf test-memory-delete.sh td.sh;
   ln -sf test-drain-supersede.sh tds.sh; ln -sf install-postgres-module.sh ipm.sh; ln -sf install-chat-provider.sh icp.sh;
   ln -sf start-logging-proxy.sh slp.sh; ln -sf show-upstream-prompt.sh sup.sh;
   ln -sf test-rerank-live.sh trl.sh; ln -sf test-retrieve-live.sh trv.sh;
   ln -sf test-chat-memory-stages.sh tcms.sh; ln -sf probe-query-pollution.sh pqp.sh'

x 'bash /root/provision.sh' 2>&1 | tail -2

# Config grants before either daemon: both refuse to start without the config
# module, and a grant written after startup is not seen.
x 'bash /root/install-config-module.sh grants'

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
