#!/bin/bash
# Start aimee-kb inside the validation container. Run AS ROOT in the container.
set -u
# Restart, not "start a second one". Without this a re-run left the ORIGINAL kb
# alive holding the previous environment, so a changed SYNTHESIS_ENDPOINT looked
# like it was being ignored -- the same trap start-server.sh had.
# Scoped to OUR binary path. CT 9078 is shared with other sessions running
# their own stacks from /opt/... -- a bare `pkill -f aimee-kb` matches theirs
# too and takes down work that has nothing to do with this branch.
pkill -f '/usr/local/bin/aimee-kb' 2>/dev/null
sleep 2
export AIMEE_HOME=/root/.config/aimee
export AIMEE_DB2_URL="postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_shared"
export AIMEE_KB_HTTP_BIND=1
export AIMEE_KB_API_BEARER_TOKEN="$(cat /root/kb-bearer.txt)"
# OIDC, when an issuer has been minted. kb verifies an RS256 bearer against this
# JWKS file and pins iss/aud, so the whole issuer is three env vars and a
# document -- no network IdP. Sourced rather than hardcoded so a container
# without make-oidc-idp.sh still starts in owner-bearer mode.
[ -f /root/.config/aimee-oidc/env.sh ] && . /root/.config/aimee-oidc/env.sh
# Start the curator's LLM lane, which is where the memory_facts drain runs (the
# lane will not start without a CONFIGURED endpoint -- see kb_curator_drain.c).
#
# The real model: Qwen3.8-27B on the appliance's llama-server. It binds
# 127.0.0.1:8762 there, so scripts/validation/fact-authority/llm-tunnel.sh
# forwards it onto the workstation's LAN address for the container to reach.
# Set AIMEE_E2E_SYNTH to override (the stub on :8799 is the offline fallback).
# A BASE url, not a full path: config_synth_chat_endpoint_normalize() appends
# /v1 itself, so passing ".../v1/chat/completions" yields
# ".../v1/chat/completions/v1" and every call fails as "provider HTTP -1".
export SYNTHESIS_ENDPOINT="${AIMEE_E2E_SYNTH:-http://192.168.1.100:8762}"
ulimit -S -s 65536 || true
cd /root
nohup /usr/local/bin/aimee-kb --http-port=8741 >/root/kb.log 2>&1 &
# The daemon now refuses to run without the config module, and the module can
# only attach once the daemon has created the bus socket -- so it is launched
# here, in the background, while the daemon is still coming up. Started before
# the daemon it would find no socket; started after the wait below, the daemon
# would already have given up.
bash /root/install-config-module.sh start-kb >/root/config-start-kb.log 2>&1 &
echo $! > /root/kb.pid
sleep 8
echo "kb pid=$(cat /root/kb.pid)"
tail -8 /root/kb.log
