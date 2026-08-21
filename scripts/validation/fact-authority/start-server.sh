#!/bin/bash
# Start aimee-server against the local kb. Run AS ROOT in the container.
set -u
# Restart, not "start a second one". Without this a re-run silently left the
# ORIGINAL process alive holding the old module policy, and a freshly installed
# grant looked like it was being ignored (module attach denied) when in fact
# nothing had reloaded it.
pkill -f aimee-server 2>/dev/null
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
ulimit -S -s 65536 || true

# Ingress pre-injection is fail-closed without an ACTIVE REPOSITORY:
# ingress_preinject_resolve_active_scope() derives the project identity from the
# server's cwd via workspace_repo_identity(), and with no repo there is no scope,
# so neither memory nor code may be injected ("must not silently broaden to
# global recall"). Running from /root therefore meant the envelope was never
# built and the memory module's RERANK confidence tier was never requested.
bash /root/make-scope-repo.sh 2>/dev/null || true
cd /root/proj 2>/dev/null || cd /root
nohup /usr/local/bin/aimee-server --socket=/root/aimee-server.sock >/root/server.log 2>&1 &
echo $! > /root/server.pid
sleep 10
echo "server pid=$(cat /root/server.pid)"
ls -la /root/aimee-server.sock 2>&1
tail -6 /root/server.log
