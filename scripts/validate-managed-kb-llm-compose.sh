#!/usr/bin/env bash
# Validate the wizard-managed KB <-> LLM contract on an isolated Docker project.
# Uses the deterministic LLM stub, a fresh KB/DB2 volume, a unique network, and
# the locally built aimee-kb binary. The trap removes only resources whose names
# begin with the unique project name printed at startup.
set -euo pipefail

cd "$(dirname "$0")/.."
root="$(pwd)"
test -x "$root/aimee-kb" || {
  echo "managed-kb-llm: build ./aimee-kb first (make -C src kb)" >&2
  exit 2
}
command -v docker >/dev/null 2>&1 || {
  echo "managed-kb-llm: docker is required" >&2
  exit 2
}

run_id="$(date +%s)-$$"
project="aimee-kb-llm-validation-${run_id}"
llm_image="${project}-llm:stub"
kb_base_image="${AIMEE_VALIDATION_KB_BASE_IMAGE:-ghcr.io/rakuensoftware/aimee-kb:testing-7704a64}"
override="$root/deploy/container/aimee-managed.validation.override.yaml"
token="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

cleanup() {
  COMPOSE_PROJECT_NAME="$project" COMPOSE_PROFILES=kb,llm \
  AIMEE_KB_IMAGE="$kb_base_image" AIMEE_LLM_IMAGE="$llm_image" \
    AIMEE_LLM_AUTH_TOKEN="$token" AIMEE_VALIDATION_ROOT="$root" \
    docker compose -f "$root/deploy/container/aimee-managed.compose.yaml" \
      -f "$override" down -v --remove-orphans >/dev/null 2>&1 || true
  docker image rm "$llm_image" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

echo "managed-kb-llm: isolated project $project"
docker build -q -f Dockerfile.aimee-llm-stub -t "$llm_image" . >/dev/null

export COMPOSE_PROJECT_NAME="$project"
export COMPOSE_PROFILES="kb,llm"
export AIMEE_KB_IMAGE="$kb_base_image"
export AIMEE_LLM_IMAGE="$llm_image"
export AIMEE_LLM_AUTH_TOKEN="$token"
export AIMEE_LLM_AUTH_REQUIRED="1"
export AIMEE_LLM_MODEL="validation-synth"
export AIMEE_LLM_EMBED_MODE="local"
export AIMEE_LLM_EMBED_TIER="mid"
export AIMEE_LLM_RERANK_MODE="local"
export AIMEE_LLM_RERANK_TIER="small"
export AIMEE_LLM_SYNTH_MODE="local"
export AIMEE_LLM_SYNTH_TIER="cpu"
export AIMEE_EMBEDDING_DIM="1024"
export AIMEE_VALIDATION_ROOT="$root"

compose=(docker compose -f "$root/deploy/container/aimee-managed.compose.yaml" -f "$override")
"${compose[@]}" config --quiet
env -0 | "${compose[@]}" run --rm --no-deps -T \
  --entrypoint /usr/local/bin/aimee-kb aimee-kb --bootstrap-vault-stdin
"${compose[@]}" up -d --wait --wait-timeout 240

# The KB must load the identity from Vault with no credential-shaped Config.Env;
# the LLM receives the same deployment identity as its own auth boundary.
echo "managed-kb-llm: checking propagated identity and role configuration"
"${compose[@]}" exec -T aimee-kb sh -ec '
  ! env | grep -q "^AIMEE_LLM_AUTH_TOKEN="
  ! env | grep -q "^LLM_API_KEY="
  test "$AIMEE_LLM_AUTH_REQUIRED" = "1"
  test "$AIMEE_LLM_URL" = "http://aimee-llm:8742"
  test "$AIMEE_LLM_MODEL" = "validation-synth"
'
"${compose[@]}" exec -T aimee-kb \
  /usr/local/bin/aimee-kb --vault-llm-auth-configured
"${compose[@]}" exec -T aimee-llm sh -ec '
  test "${#AIMEE_LLM_AUTH_TOKEN}" -eq 64
  test "$AIMEE_LLM_STRICT_BIND" = "1"
  test "$AIMEE_LLM_EMBED_MODE/$AIMEE_LLM_EMBED_TIER" = "local/mid"
  test "$AIMEE_LLM_RERANK_MODE/$AIMEE_LLM_RERANK_TIER" = "local/small"
  test "$AIMEE_LLM_SYNTH_MODE/$AIMEE_LLM_SYNTH_TIER" = "local/cpu"
  test "$AIMEE_LLM_SYNTH_MODEL" = "validation-synth"
  test "$AIMEE_EMBEDDING_DIM" = "1024"
'

# Missing and incorrect credentials fail. A short-lived stdin-only curl proves
# the deployment identity matches without recreating a credential environment.
echo "managed-kb-llm: checking authenticated KB-to-LLM probe"
"${compose[@]}" exec -T aimee-kb sh -ec '
  code=$(curl -sS -o /dev/null -w "%{http_code}" -X POST "$AIMEE_LLM_URL/auth/verify")
  test "$code" = 401
  code=$(curl -sS -o /dev/null -w "%{http_code}" -X POST \
    -H "Authorization: Bearer wrong" "$AIMEE_LLM_URL/auth/verify")
  test "$code" = 401
'
vault_curl() {
  printf 'header = "Authorization: Bearer %s"\n' "$token" |
    "${compose[@]}" exec -T aimee-kb curl -K - "$@"
}
vault_curl -fsS -X POST http://aimee-llm:8742/auth/verify |
  python3 -c 'import json,sys; x=json.load(sys.stdin); assert x.get("status")=="ok" and x.get("scope")=="curator"'

# Exercise every authenticated gateway role with the same stdin-only identity.
echo "managed-kb-llm: checking embed, batch, rerank, and synth endpoints"
vault_curl -fsS -H 'Content-Type: application/json' -d '["managed identity embedding"]' \
  http://aimee-llm:8742/embed_batch |
  python3 -c 'import json,sys; x=json.load(sys.stdin); assert len(x)==1 and len(x[0])==1024'
vault_curl -fsS -H 'Content-Type: application/json' -d '[["q","a"],["q","b"]]' \
  http://aimee-llm:8742/rerank |
  python3 -c 'import json,sys; assert len(json.load(sys.stdin))==2'
vault_curl -fsS -H 'Content-Type: application/json' \
  -d '{"model":"validation-synth","messages":[{"role":"user","content":"ping"}]}' \
  http://aimee-llm:8742/v1/chat/completions |
  python3 -c 'import json,sys; assert json.load(sys.stdin).get("choices")'

# The same image must refuse an unauthenticated wildcard bind.
echo "managed-kb-llm: checking fail-closed empty-token bind"
if docker run --rm --network none \
  -e AIMEE_LLM_STUB=1 -e AIMEE_LLM_STRICT_BIND=1 -e AIMEE_LLM_BIND=0.0.0.0 \
  "$llm_image" >/dev/null 2>&1; then
  echo "managed-kb-llm: strict bind unexpectedly accepted an empty token" >&2
  exit 1
fi

echo "managed-kb-llm: PASS (credential, role config, embed, batch, rerank, synth, fail-closed bind)"
