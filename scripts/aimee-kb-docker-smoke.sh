#!/usr/bin/env bash
#
# aimee-kb-docker-smoke.sh — prove an aimee-kb Docker container fully spins up
# and is usable.
#
# Brings up the compose.yaml stack (self-contained aimee-kb; it embeds in-container,
# so there is no separate inference service), waits for
# the kb to report healthy, then exercises the live /v1 surface end to end:
#
#   1. /v1/health              — HTTP API is up
#   2. /v1/health?status=1     — DB2 connected, schema applied, vector store ready
#   3. /v1/version             — build identifies itself
#   4. /v1/capabilities        — capability manifest serves
#   5. POST /v1/search         — DB2-backed query path works (empty result is OK)
#   6. embedder /embed         — real embedding round-trip (in-container)
#   7. no dim refusal          — the vectors fit the columns the kb built
#
# The embedder listens on loopback INSIDE the kb container, so its checks run via
# `docker compose exec` (the kb image ships curl).
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

# The kb embeds in-container from weights baked into the image, so there is no tier
# to pick and nothing to download. Select the bundled model (the image pre-selects
# nothing) and let AIMEE_EMBEDDING_DIM default from config so the schema width and
# the model's output width come from one source.
: "${EMBEDDER_MODEL:=bekko-a25m}"
export EMBEDDER_MODEL
# The width is NOT asserted against a number here. It is a setting, so it has one
# home — config, inside the deployment — and a copy in this script would be a second
# declaration that can disagree. What this smoke can check without duplicating it is
# the property that matters: the vectors the embedder returns must fit the columns the
# kb built, which the dim guard enforces at insert time. So the checks below drive a
# real embed + search and then assert the kb logged no dim refusal.

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
  bold "==> Building + Vault-bootstrapping + starting the stack ($COMPOSE_FILE)"
  "${DC[@]}" build
  # Port-remap overrides do not affect the persistent Vault volume. Bootstrap
  # against the base file so the disposable helper seals first-boot values
  # before any long-lived service is created.
  bootstrap_compose="${COMPOSE_FILE%% *}"
  scripts/aimee-compose-vault-bootstrap.sh -f "$bootstrap_compose" kb
  "${DC[@]}" up -d --no-build

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
                                               -d '{"query":"docker smoke test","scope":"all","max_results":3}' \
                                               "${KB_URL}/v1/search"

# memory.find_facts with graph-code fusion ON is the deepest-stack worker path
# (the server's `aimee memory search` drives it). It SIGSEGVs the kb (exit 139)
# unless the container has a 64 MB stack ulimit — a regression the /v1/search
# check above does NOT catch. curl -fsS fails here if the kb crashed, so this
# guards the ulimit fix. An empty "facts" array on a fresh DB is a pass.
check "POST memory.find_facts (fusion)" '"facts"' -X POST -H 'content-type: application/json' \
                                               -d '{"query":"docker smoke test","limit":3,"graph_code_fusion_state":"on"}' \
                                               "${KB_URL}/v1/actions/memory.find_facts"

bold "==> Embed backend round-trip (in-container, inside the kb container)"
# The embedder is a process INSIDE aimee-kb now, not a service beside it: the
# entrypoint starts it on loopback and exports AIMEE_EMBEDDER_URL to point at it.
# So the probe asks the kb container to embed against its own configured backend,
# which is the same variable an external endpoint would set — one code path.
#
# This must not skip. The old version looked for an `aimee-llm` or `embedder`
# service and skipped when it found neither; with both retired that skip would be
# unconditional, and a skipped check reads as a pass. If EMBEDDER_MODEL selected a
# model, a working round-trip is mandatory.
emb_url="$("${DC[@]}" exec -T aimee-kb sh -c 'printf "%s" "${AIMEE_EMBEDDER_URL:-}"' 2>/dev/null || true)"
if [[ -z "$emb_url" ]]; then
  red   "  FAIL  kb reports no AIMEE_EMBEDDER_URL despite EMBEDDER_MODEL=${EMBEDDER_MODEL}"
  printf '        the entrypoint should have started the bundled model and exported it\n'
  FAIL=$((FAIL + 1))
elif emb="$("${DC[@]}" exec -T aimee-kb sh -c \
      "printf 'aimee docker smoke test' | curl -fsS --max-time 60 -X POST \
         --data-binary @- \"\${AIMEE_EMBEDDER_URL}/embed\"" 2>/dev/null)" \
   && [[ "$emb" == \[* ]]; then
  dims="$(($(printf '%s' "$emb" | tr -cd ',' | wc -c) + 1))"
  green "  PASS  /embed returned a ${dims}-dim vector (${EMBEDDER_MODEL})"
  PASS=$((PASS + 1))

  # The width is only correct if the kb can STORE it. A mismatch between the
  # embedder's output and the schema's columns shows up as the dim guard refusing
  # the upsert — which is silent in the API response, so read the kb's log. This is
  # what a bad default did in practice: columns sized 1024, vectors 384, and every
  # insert refused while /v1/search still answered 200 with an empty result.
  if "${DC[@]}" logs aimee-kb 2>&1 | grep -qiE "dim mismatch|refusing upsert|vector-space"; then
    red   "  FAIL  kb refused vectors — embedder width disagrees with the schema"
    "${DC[@]}" logs aimee-kb 2>&1 | grep -iE "dim mismatch|refusing upsert|vector-space" | tail -3
    FAIL=$((FAIL + 1))
  else
    green "  PASS  kb stored embeddings with no dim/vector-space refusal"
    PASS=$((PASS + 1))
  fi
else
  red   "  FAIL  /embed round-trip against ${emb_url}"
  printf '        got: %s\n' "${emb:-<no response>}"
  FAIL=$((FAIL + 1))
fi

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-kb container is up and usable."
