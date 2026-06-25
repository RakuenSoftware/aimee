#!/usr/bin/env bash
#
# aimee-kb-docker-smoke.sh — prove an aimee-kb Docker container fully spins up
# and is usable.
#
# Brings up the compose.yaml stack (postgres + embedder + aimee-kb), waits for
# the kb to report healthy, then exercises the live /v1 surface end to end:
#
#   1. /v1/health              — HTTP API is up
#   2. /v1/health?status=1     — DB2 connected, schema applied, vector store ready
#   3. /v1/version             — build identifies itself
#   4. /v1/capabilities        — capability manifest serves
#   5. POST /v1/search         — DB2-backed query path works (empty result is OK)
#   6. embedder /embed         — real embedding round-trip (in-network)
#
# The embedder port is not published by compose.yaml, so its checks run inside
# the kb container via `docker compose exec` (the kb image ships curl).
#
# Usage:
#   scripts/aimee-kb-docker-smoke.sh            # assume stack already up on :8741
#   scripts/aimee-kb-docker-smoke.sh --up       # build + bring the stack up first
#   scripts/aimee-kb-docker-smoke.sh --up --down # also tear the stack down after
#
# Env:
#   KB_URL          base URL of the kb /v1 API   (default http://localhost:8741)
#   COMPOSE_FILE    compose file(s), space-separated (default compose.yaml);
#                   list several to layer an override (e.g. remap host ports)
#   WAIT_SECONDS    health wait budget on --up   (default 300)
#
# Exit code: 0 = all checks passed, non-zero = first failure.

set -euo pipefail

KB_URL="${KB_URL:-http://localhost:8741}"
COMPOSE_FILE="${COMPOSE_FILE:-compose.yaml}"
WAIT_SECONDS="${WAIT_SECONDS:-300}"

# Smoke tests validate the retrieval pipeline, not a specific model. Default to
# the light tier (0.6b embedder/1024-dim + 400m reranker) so `up --build` is fast
# and fits CI runners; the shipped default is the 4b + 1b reranker. Override any.
: "${EMBEDDER_MODEL:=perplexity-ai/pplx-embed-v1-0.6b}"
: "${RERANKER_MODEL:=cross-encoder/ettin-reranker-400m-v1}"
: "${AIMEE_EMBEDDING_DIM:=1024}"
export EMBEDDER_MODEL RERANKER_MODEL AIMEE_EMBEDDING_DIM
DO_UP=0
DO_DOWN=0

for arg in "$@"; do
  case "$arg" in
    --up)   DO_UP=1 ;;
    --down) DO_DOWN=1 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Run from the repo root (this script lives in scripts/).
cd "$(dirname "$0")/.."

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

# COMPOSE_FILE may name several files (space-separated) so an override can
# layer on top of the base stack (e.g. remap published ports).
DC=(docker compose)
for f in $COMPOSE_FILE; do DC+=(-f "$f"); done
PASS=0
FAIL=0

check() {
  # check <name> <expected-substring> <curl-args...>
  local name="$1" expect="$2"; shift 2
  local body
  if body="$(curl -fsS --max-time 15 "$@" 2>/dev/null)" && [[ "$body" == *"$expect"* ]]; then
    green  "  PASS  $name"
    PASS=$((PASS + 1))
  else
    red    "  FAIL  $name"
    printf '        expected substring: %s\n' "$expect"
    printf '        got: %s\n' "${body:-<no response / curl error>}"
    FAIL=$((FAIL + 1))
  fi
}

cleanup() {
  if [[ "$DO_DOWN" == 1 ]]; then
    bold "==> Tearing down the stack (--down)"
    "${DC[@]}" down -v || true
  fi
}
trap cleanup EXIT

if [[ "$DO_UP" == 1 ]]; then
  bold "==> Building + starting the stack ($COMPOSE_FILE)"
  "${DC[@]}" up -d --build

  bold "==> Waiting up to ${WAIT_SECONDS}s for aimee-kb to report healthy"
  deadline=$((SECONDS + WAIT_SECONDS))
  while true; do
    state="$("${DC[@]}" ps --format '{{.Service}} {{.Health}}' 2>/dev/null | awk '$1=="aimee-kb"{print $2}')"
    [[ "$state" == "healthy" ]] && { green "    aimee-kb is healthy"; break; }
    if (( SECONDS >= deadline )); then
      red "    aimee-kb did not become healthy within ${WAIT_SECONDS}s (state: ${state:-unknown})"
      "${DC[@]}" ps
      "${DC[@]}" logs --tail=40 aimee-kb || true
      exit 1
    fi
    sleep 3
  done
fi

bold "==> Exercising the kb /v1 surface at ${KB_URL}"
check "/v1/health"                  '"status"'  "${KB_URL}/v1/health"
# status=1 collects project status from DB2 and reports the pgvector store
# state under "vector" — its presence proves the schema is applied + queried.
check "/v1/health?status=1 (vector)" '"vector"' "${KB_URL}/v1/health?status=1"
check "/v1/version"               'version'    "${KB_URL}/v1/version"
check "/v1/capabilities"          'capab'      "${KB_URL}/v1/capabilities"
# A real query exercises the full ranked path (query -> embed -> pgvector); an
# empty "hits" array on a fresh DB is still a well-formed pass and proves the
# schema is applied and the query path executes.
check "POST /v1/search"           '"hits"'     -X POST -H 'content-type: application/json' \
                                               -d '{"query":"docker smoke test","max_results":3}' \
                                               "${KB_URL}/v1/search"

# memory.find_facts with graph-code fusion ON is the deepest-stack worker path
# (the server's `aimee memory search` drives it). It SIGSEGVs the kb (exit 139)
# unless the container has a 64 MB stack ulimit — a regression the /v1/search
# check above does NOT catch. curl -fsS fails here if the kb crashed, so this
# guards the ulimit fix. An empty "facts" array on a fresh DB is a pass.
check "POST memory.find_facts (fusion)" '"facts"' -X POST -H 'content-type: application/json' \
                                               -d '{"query":"docker smoke test","limit":3,"graph_code_fusion_state":"on"}' \
                                               "${KB_URL}/v1/actions/memory.find_facts"

bold "==> Embed backend round-trip (in-network, via the kb container)"
# Unified topology: the kb embeds against the aimee-llm container (AIMEE_LLM_URL,
# /embed). Legacy topology: the torch embedder service (AIMEE_EMBEDDER_URL). Probe
# whichever backend this compose project runs.
emb_backend=""
if "${DC[@]}" ps --format '{{.Service}}' 2>/dev/null | grep -qx aimee-llm; then
  emb_backend='${AIMEE_LLM_URL:-http://aimee-llm:8080}/embed'
elif "${DC[@]}" ps --format '{{.Service}}' 2>/dev/null | grep -qx embedder; then
  emb_backend='${AIMEE_EMBEDDER_URL:-http://embedder:8080}/embed'
fi
if [[ -n "$emb_backend" ]]; then
  if emb="$("${DC[@]}" exec -T aimee-kb sh -c \
        "printf 'aimee docker smoke test' | curl -fsS --max-time 30 -X POST \
           --data-binary @- \"$emb_backend\"" 2>/dev/null)" \
     && [[ "$emb" == \[* ]]; then
    dims="$(($(printf '%s' "$emb" | tr -cd ',' | wc -c) + 1))"
    green "  PASS  /embed returned a ${dims}-dim vector"
    PASS=$((PASS + 1))
  else
    red   "  FAIL  /embed round-trip"
    printf '        got: %s\n' "${emb:-<no response>}"
    FAIL=$((FAIL + 1))
  fi
else
  printf '  SKIP  no embed backend service in this compose project\n'
fi

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-kb container is up and usable."
