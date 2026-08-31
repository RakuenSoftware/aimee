#!/bin/bash
# Locate an OpenAI-compatible LLM endpoint on a host.
#
# Two shapes are probed. A direct port (llama.cpp/vLLM 8000/8080, Ollama 11434,
# the aimee-llm plugin's published 8742), and -- for an appliance that fronts
# everything through nginx, as SmoothNAS does -- a reverse-proxied PATH on 443.
# Usage: find-provider.sh <host> [extra-ports...]
set -u
HOST="${1:-192.168.1.254}"
shift || true
PORTS="8742 8000 8080 11434 1234 5000 5001 8001 8081 3000 9000 8880 8888 7860 ${*}"

open=""
for p in $PORTS; do
  timeout 2 bash -c "exec 3<>/dev/tcp/${HOST}/${p}" 2>/dev/null && open="$open $p"
done
echo "open LLM-ish ports:${open:- none}"
for p in $open; do
  echo "== ${HOST}:${p}/v1/models =="
  timeout 5 curl -s -m 5 "http://${HOST}:${p}/v1/models" | head -c 500; echo
done

echo
echo "== reverse-proxied paths on https://${HOST} =="
for path in /v1/models /llm/v1/models /aimee-llm/v1/models /api/llm/v1/models \
            /plugin/aimee-llm/v1/models /aimee/v1/models /ollama/api/tags; do
  code="$(timeout 5 curl -sk -m 5 -o /dev/null -w '%{http_code}' "https://${HOST}${path}" 2>/dev/null)"
  [ "$code" = "000" ] && continue
  printf '  %-34s %s\n' "$path" "$code"
  if [ "$code" = "200" ]; then
    timeout 5 curl -sk -m 5 "https://${HOST}${path}" | head -c 400; echo
  fi
done
