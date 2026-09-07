#!/bin/bash
# Start aimee-server against the local kb. Run AS ROOT in the container.
set -u
# Restart, not "start a second one". Without this a re-run silently left the
# ORIGINAL process alive holding the old module policy, and a freshly installed
# grant looked like it was being ignored (module attach denied) when in fact
# nothing had reloaded it.
# Scoped to OUR binary path. CT 9078 is shared with other sessions running
# their own stacks from /opt/... -- a bare `pkill -f aimee-server` matches theirs
# too and takes down work that has nothing to do with this branch.
pkill -f '/usr/local/bin/aimee-server' 2>/dev/null
sleep 2
export AIMEE_HOME=/root
export AIMEE_KB_API_URL="http://127.0.0.1:8741"
export AIMEE_KB_API_BEARER_TOKEN="$(cat /root/kb-bearer.txt)"
export AIMEE_DB1_URL="sqlite:///root/aimee.db"
export AIMEE_SERVER_HTTP_BIND=1
export AIMEE_DEPLOY_ENABLED=1
export AIMEE_API_BEARER_TOKEN="$(cat /root/server-bearer.txt)"
# Ingress pre-injection on the Anthropic-native route is opt-in (P5 §2.3), and
# that route (/v1/messages) is where the envelope -- and so the memory module's
# RERANK confidence tier -- is actually built. It has no env override, so it is
# set in the config file below. Off by default, which is why a chat turn on the
# native route builds no envelope and never reaches RERANK.
grep -q '^ingress_preinject_anthropic_enabled:' /root/aimee.yaml 2>/dev/null || \
  printf '\ningress_preinject_anthropic_enabled: true\n' >> /root/aimee.yaml

# code_context_mode defaults to "on", which is STRICT: an `on` packet may carry
# only validated current-project code evidence, so ingress_preinject_build sets
# facts_on = 0 AND legacy_preview_on = 0. Nothing is gathered, the envelope comes
# back NULL, and the memory module's RERANK tier is never requested -- with no
# error anywhere, because refusing to inject is the intended behaviour of strict
# mode. `observe` admits memory previews and typed facts.
grep -q '^code_context_mode:' /root/aimee.yaml 2>/dev/null || \
  printf '\ncode_context_mode: observe\n' >> /root/aimee.yaml

# Set the memory module toggle EXPLICITLY rather than relying on the -1
# "unspecified" tristate surviving a config round-trip: ir_memory_enabled()
# reads config_module_memory(), and a 0 there is an explicit DISABLE that
# silently skips the whole ir_stage_memory transform.
grep -q '^modules:' /root/aimee.yaml 2>/dev/null || \
  printf '\nmodules:\n  memory: true\n' >> /root/aimee.yaml
ulimit -S -s 65536 || true

# Ingress pre-injection is fail-closed without an ACTIVE REPOSITORY:
# ingress_preinject_resolve_active_scope() derives the project identity from the
# server's cwd via workspace_repo_identity(), and with no repo there is no scope,
# so neither memory nor code may be injected ("must not silently broaden to
# global recall"). Running from /root therefore meant the envelope was never
# built and the memory module's RERANK confidence tier was never requested.
bash /root/make-scope-repo.sh 2>/dev/null || true
cd /root/proj 2>/dev/null || cd /root
# The management trust chain, when it has been provisioned. Without these the
# server denies every KB-issued identity token as no_team_configured/INVALID,
# which looks like a bad token rather than an unconfigured server.
[ -f /root/mgmt-trust-env.sh ] && . /root/mgmt-trust-env.sh
nohup /usr/local/bin/aimee-server --socket=/root/aimee-server.sock >/root/server.log 2>&1 &
# The daemon now refuses to run without the config module, and the module can
# only attach once the daemon has created the bus socket -- so it is launched
# here, in the background, while the daemon is still coming up. Started before
# the daemon it would find no socket; started after the wait below, the daemon
# would already have given up.
bash /root/install-config-module.sh start-server >/root/config-start-server.log 2>&1 &
# db1 the same way, and for a sharper reason: server_tls_init_default() runs the
# mTLS ramp self-test during startup, and that test is db1 stage 19 (db1-pki).
# If db1 is not answering by then the ramp refuses and TLS is disabled for the
# life of the process -- reported as "tls_port set but TLS cert/key not
# loadable", which blames the certificate.
# PostgreSQL must be serving before the store applies its embedded schema. Run
# the pair serially in one background job while the daemon waits for DB1 PKI.
( bash /root/install-postgres-module-server.sh start >/root/postgres-start-server.log 2>&1 && \
  bash /root/install-db1-module.sh start >/root/db1-start.log 2>&1 ) &
echo $! > /root/server.pid
sleep 10
echo "server pid=$(cat /root/server.pid)"
ls -la /root/aimee-server.sock 2>&1
tail -6 /root/server.log
