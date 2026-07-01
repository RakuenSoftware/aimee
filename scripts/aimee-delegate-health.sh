#!/bin/bash
# Healthcheck for the aimee-delegate container: the model server must be up AND the
# entrypoint's post-launch verification must not have tripped the degraded sentinel
# (flash-attention no-op, or MoE experts CPU-fell-back past target). Reporting
# unhealthy on a silent perf regression lets the orchestrator/operator notice a
# misconfigured KV/offload profile instead of silently shipping degraded throughput.
set -u
PORT="${AIMEE_DELEGATE_PORT:-8083}"
[ -f /tmp/aimee-delegate-degraded ] && { echo "degraded: startup verification failed (see logs)"; exit 1; }
curl -fsS --max-time 4 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1 || { echo "llama-server /health failed"; exit 1; }
exit 0
