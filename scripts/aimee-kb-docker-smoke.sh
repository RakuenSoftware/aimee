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
# nothing) and let EMBEDDER_DIMS default from config so the schema width and
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

# A fresh smoke stack owns its own project. Without an explicit name the Vault
# bootstrap helper discovers every running project that used the same Compose
# filename and refuses an otherwise isolated run on a shared Docker host.
if [[ "$DO_UP" == 1 && -z "${COMPOSE_PROJECT_NAME:-}" ]]; then
  export COMPOSE_PROJECT_NAME="aimee-e2e-kb-$$"
fi

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
  if [[ "${AIMEE_E2E_SKIP_BUILD:-0}" != "1" ]]; then
    "${DC[@]}" build
  else
    bold "==> Using prebuilt topology images (AIMEE_E2E_SKIP_BUILD=1)"
  fi
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
# The embedder is a process INSIDE the kb container, not a service beside it: the
# entrypoint starts it on loopback, exports EMBEDDER_URL, and then runs the kb.
#
# Getting at that value took three tries, so the reasoning is recorded here:
#
#  1. `compose exec` does NOT see it. It spawns a fresh process from the container's
#     CONFIGURED environment (image ENV + the compose `environment:` block), never a
#     variable exported at runtime. And EMBEDDER_URL must be declared in compose
#     with an empty default so an operator can point the kb at an external endpoint —
#     so `compose exec` reads "" no matter what the entrypoint did.
#  2. /proc/1/environ does NOT see it either, for two reasons. On the embedded-DB path
#     (compose.yaml, i.e. T1) the entrypoint stays PID 1 and runs the kb as a CHILD, so
#     PID 1 is the shell. And /proc/PID/environ is a snapshot taken at exec time — a
#     variable the shell exported afterwards never appears in its own environ.
#  3. What works: the environ of the AIMEE-KB process. It was exec'd after the export,
#     so its snapshot contains the effective value. Found by scanning /proc rather than
#     with pgrep, which the image does not ship. This is correct on both paths — kb as
#     a child here, kb as PID 1 on the external-DB path.
#
# This must not skip. The version before the cutover looked for an `aimee-llm` or
# `embedder` SERVICE and skipped when it found neither; with both retired that skip
# would be unconditional, and a skipped check reads as a pass.
read_kb_env() {
  # $1 = variable name. Prints its value from the aimee-kb process's environment.
  "${DC[@]}" exec -T aimee-kb sh -c '
    for p in /proc/[0-9]*; do
      [ -r "$p/comm" ] || continue
      if [ "$(cat "$p/comm" 2>/dev/null)" = "aimee-kb" ]; then
        tr "\0" "\n" < "$p/environ" 2>/dev/null | sed -n "s/^'"$1"'=//p" | head -1
        return 0
      fi
    done
  ' 2>/dev/null || true
}

emb_url="$(read_kb_env EMBEDDER_URL)"
if [[ -z "$emb_url" ]]; then
  red   "  FAIL  the kb process was given no EMBEDDER_URL (EMBEDDER_MODEL=${EMBEDDER_MODEL})"
  printf '        the entrypoint should have started the bundled model and exported it\n'
  printf '        kb process environment (embedder-related):\n'
  "${DC[@]}" exec -T aimee-kb sh -c '
    for p in /proc/[0-9]*; do
      [ -r "$p/comm" ] || continue
      if [ "$(cat "$p/comm" 2>/dev/null)" = "aimee-kb" ]; then
        tr "\0" "\n" < "$p/environ" 2>/dev/null | grep -E "EMBEDDER|EMBEDDING" || echo "          (none)"
        return 0
      fi
    done
    echo "          (no aimee-kb process found)"
  ' 2>/dev/null || true
  printf '        entrypoint embedder log lines:\n'
  "${DC[@]}" logs aimee-kb 2>&1 | grep -iE "embedder" | tail -5 || printf '          (none)\n'
  FAIL=$((FAIL + 1))
elif emb="$("${DC[@]}" exec -T aimee-kb sh -c \
      "printf 'aimee docker smoke test' | curl -fsS --max-time 60 -X POST \
         --data-binary @- \"${emb_url}/embed\"" 2>/dev/null)" \
   && [[ "$emb" == \[* ]]; then
  dims="$(($(printf '%s' "$emb" | tr -cd ',' | wc -c) + 1))"
  green "  PASS  /embed returned a ${dims}-dim vector (${EMBEDDER_MODEL}, via ${emb_url})"
  PASS=$((PASS + 1))
else
  red   "  FAIL  /embed round-trip against ${emb_url}"
  printf '        got: %s\n' "${emb:-<no response>}"
  "${DC[@]}" logs aimee-kb 2>&1 | grep -iE "embedder" | tail -5 || true
  FAIL=$((FAIL + 1))
fi

if [[ -n "$emb_url" ]]; then
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
fi

# ---- bundled synthesis, on the image variants that carry one ----------------
# Only the *-llm-e2b / *-llm-e4b tags bake a model. On those, the entrypoint is
# supposed to start llama-server on loopback and point synthesis at it; on the
# others there is nothing to check and this is skipped rather than failed.
#
# Worth checking here rather than trusting the build: everything upstream proves
# the image CONTAINS llama.cpp and a model. Only a booted container proves the
# entrypoint decided to start it — and start_synthesis shipped once having never
# been executed at all.
has_llm="$("${DC[@]}" exec -T aimee-kb sh -c 'printf %s "${AIMEE_WITH_LLAMACPP:-0}"' 2>/dev/null || echo 0)"
if [[ "$has_llm" == "1" ]]; then
  syn_model="$("${DC[@]}" exec -T aimee-kb sh -c 'printf %s "${AIMEE_SYNTHESIS_MODEL:-}"' 2>/dev/null || true)"
  bold "==> Bundled synthesis (${syn_model:-unknown})"
  syn_ok=0
  for _ in $(seq 1 100); do
    if "${DC[@]}" exec -T aimee-kb curl -fsS -m 5 http://127.0.0.1:8761/health >/dev/null 2>&1; then
      syn_ok=1; break
    fi
    sleep 6
  done
  if [[ "$syn_ok" == 1 ]]; then
    green "  PASS  entrypoint started llama-server on :8761"
    PASS=$((PASS + 1))
    if "${DC[@]}" exec -T aimee-kb curl -fsS -m 120 http://127.0.0.1:8761/v1/chat/completions \
         -H 'content-type: application/json' \
         -d '{"messages":[{"role":"user","content":"Reply with one word: ok"}],"max_tokens":8}' \
         2>/dev/null | grep -q '"choices"'; then
      green "  PASS  bundled synthesis completed a request"
      PASS=$((PASS + 1))
    else
      red   "  FAIL  llama-server is up but did not complete a request"
      "${DC[@]}" logs aimee-kb 2>&1 | grep -iE "synthesis|llama" | tail -5 || true
      FAIL=$((FAIL + 1))
    fi
  else
    red   "  FAIL  no llama-server on :8761 — start_synthesis did not run"
    "${DC[@]}" logs aimee-kb 2>&1 | grep -iE "synthesis|llama" | tail -8 || true
    FAIL=$((FAIL + 1))
  fi
fi

echo
bold "==> Summary: ${PASS} passed, ${FAIL} failed"
[[ "$FAIL" == 0 ]] || exit 1
green "aimee-kb container is up and usable."
