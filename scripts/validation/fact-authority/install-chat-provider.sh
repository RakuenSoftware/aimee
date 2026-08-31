#!/bin/bash
# Configure Qwen3.8-27B as aimee-server's chat provider.
#
# This is what makes a real chat turn possible, and a chat turn is the only
# thing that drives the memory module's two server-only stages: RERANK (the
# ingress confidence tier) and RETRIEVE (the PII recall gate).
#
# The shape mirrors what `aimee agent setup` writes for a direct HTTP provider
# (cmd_agent_setup.c setup_api_provider): an `openai` wire adapter, a base
# endpoint, a model id, and auth_type "none" because llama-server takes no key.
# The endpoint is the tunnel onto the appliance's loopback-bound llama-server.
#
# The model id must be what /v1/models reports -- llama-server matches on the
# full GGUF path -- so it is read from the server rather than hardcoded.
# Run AS ROOT in the container.
set -u
ENDPOINT="${LLM_ENDPOINT:-http://192.168.1.100:8762/v1}"

MODEL="$(curl -s -m 10 "${ENDPOINT}/models" \
         | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' | head -1)"
[ -n "$MODEL" ] || { echo "could not read a model id from ${ENDPOINT}/models" >&2; exit 1; }
echo "model: $MODEL"

cat > /root/agents.json <<EOF
{
  "default_agent": "qwen",
  "agents": [
    {
      "name": "qwen",
      "provider": "openai",
      "endpoint": "${ENDPOINT}",
      "model": "${MODEL}",
      "auth_type": "none",
      "enabled": true,
      "tools_enabled": true,
      "cost_tier": 0,
      "max_tokens": 2048,
      "timeout_ms": 300000,
      "max_parallel": 1,
      "roles": ["summarize", "format", "draft", "explain", "code", "execute"]
    }
  ]
}
EOF
echo "wrote /root/agents.json"
python3 -c "import json;d=json.load(open('/root/agents.json'));print('valid json, agents:',[a['name'] for a in d['agents']])"
