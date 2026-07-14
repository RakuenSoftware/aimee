#!/usr/bin/env bash
#
# e2e-matrix.sh — run the aimee deploy-matrix E2E
# (docs/proposals/pending/aimee-e2e-deploy-matrix.md) and print one pass/fail
# table. Runs ON the host it is invoked on: on a Docker host it can run the
# container topologies (T1-T3); on a Linux box with the build deps it can run the
# local topologies (T5-T6). For the pve runs, invoke this INSIDE CT 101
# (ssh root@192.168.1.253 -> pct exec 101).
#
# Topologies:
#   T1  Docker kb-only                 (compose.yaml)
#   T2  Docker server + kb split       (compose.server.yaml)
#   T3  Docker server standalone       (compose.server-standalone.yaml)
#   T5  Local full stack               (scratch server + local kb)    [Linux only]
#   T6  Local server + Docker kb hybrid (scratch server -> :8741)     [Linux only]
#   PC  Thin-client smoke              (against the T2 server URL)
#   AD  Thin-client adoption           (TOFU bootstrap-bearer enrollment) [Linux only]
#
# Usage:
#   scripts/e2e-matrix.sh                      # everything runnable on this host
#   scripts/e2e-matrix.sh --only T1,T3         # a subset
#   scripts/e2e-matrix.sh --only T6 --kb-url http://localhost:8741
#
# Flags:
#   --only LIST       comma-separated topology IDs (default: all)
#   --kb-url URL       external kb URL for T6 (default the offset kb URL)
#   --port-offset N    shift published host ports by N to dodge ports already in
#                      use (e.g. --port-offset 10000 -> server :18740, kb :18741).
#                      Container ports never move; only the host mapping shifts.
#   --keep             do not tear stacks down (omit --down)
#
# Exit code: 0 only if every SELECTED + RUN topology passed. SKIPPED topologies
# (e.g. Docker ones with no Docker, local ones off-Linux) are reported loudly and
# do NOT count as passes.

set -uo pipefail

cd "$(dirname "$0")/.."
SCRIPTS="$(pwd)/scripts"
ROOT="$(pwd)"

ONLY="T1,T2,T3,T5,T6,PC,AD"
KB_URL=""
DOWN="--down"
PORT_OFFSET=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --only) ONLY="$2"; shift 2 ;;
    --kb-url) KB_URL="$2"; shift 2 ;;
    --port-offset) PORT_OFFSET="$2"; shift 2 ;;
    --keep) DOWN=""; shift ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# Models are fetched at runtime now (not baked), so CI uses the STUB embedder
# (EMBEDDER_STUB=1): deterministic fixed-dim vectors, no multi-GB cold download.
# It exercises the real kb -> embedder -> pgvector wiring at the deployment's
# real dim (2560 here, matching the 4b default schema_embedding_dim — never the
# retired 384). compose reads these via ${...}; exporting covers every topology.
export EMBEDDER_STUB="${EMBEDDER_STUB:-1}"
export EMBEDDER_STUB_DIM="${EMBEDDER_STUB_DIM:-2560}"
export AIMEE_EMBEDDING_DIM="${AIMEE_EMBEDDING_DIM:-2560}"
# Unified-llm topologies (T1 kb-only, T2 server+kb) now embed/rerank/synth against
# the `aimee-llm` container instead of the torch `embedder`. CI builds the tiny
# STUB image (no llama.cpp / GGUFs) and runs it in stub mode at the same dim, so
# the kb -> gateway contract is exercised cheaply. (T3 standalone has no embedder.)
# compose reads these via ${...}.
export AIMEE_LLM_STUB="${AIMEE_LLM_STUB:-1}"
export AIMEE_LLM_STUB_DIM="${AIMEE_LLM_STUB_DIM:-2560}"
export AIMEE_LLM_DOCKERFILE="${AIMEE_LLM_DOCKERFILE:-Dockerfile.aimee-llm-stub}"

# COMPOSE_PROFILES: the `aimee-llm` service is now profile-gated (`llm`) — it only
# starts when a page-2 role is local — so CI opts it in to exercise the kb->gateway
# contract (with the stub image above). The separate `curator-llm` profile stays
# OFF: the smoke tests don't exercise the curator and its multi-GB Gemma GGUF would
# blow the runner's disk/time. Selecting only `llm` overrides the committed .env's
# curator-llm default. T3 (standalone) has no aimee-llm service, so this is a no-op there.
export COMPOSE_PROFILES="${COMPOSE_PROFILES:-llm}"

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
selected() { case ",$ONLY," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }
have_docker() { command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; }
is_linux() { [[ "$(uname -s)" == "Linux" ]]; }

# Published host ports (shift by --port-offset to dodge ports already in use on
# the host). Container ports never move. The server's published /v1 is native TLS
# on 8743 (plaintext 8740 is loopback-only inside the container); the kb stays
# plaintext http on 8741.
SERVER_PORT=$((8743 + PORT_OFFSET))
KB_PORT=$((8741 + PORT_OFFSET))
SERVER_URL="https://localhost:${SERVER_PORT}"
[[ -z "$KB_URL" ]] && KB_URL="http://localhost:${KB_PORT}"

OVERRIDE_DIR=""
if [[ "$PORT_OFFSET" != 0 ]]; then
  OVERRIDE_DIR="$(mktemp -d)"
  trap 'rm -rf "$OVERRIDE_DIR"' EXIT
fi

# write_override <"svc=host:ctr[,host:ctr...]" ...> -> path to an override file
# that REPLACES each named service's published ports (compose !override tag).
# Comma-separated port maps land in a single !override list for that service.
write_override() {
  local f="$OVERRIDE_DIR/ovr.$$.${RANDOM}.yaml"
  { printf 'services:\n'
    for spec in "$@"; do
      local svc="${spec%%=*}" portmap="${spec#*=}"
      printf '  %s:\n    ports: !override [' "$svc"
      local sep="" p
      IFS=',' read -ra _ports <<<"$portmap"
      for p in "${_ports[@]}"; do printf '%s"%s"' "$sep" "$p"; sep=", "; done
      printf ']\n'
    done
  } > "$f"
  printf '%s' "$f"
}

declare -a ROWS=()
record() { ROWS+=("$1|$2|$3"); }   # id | result | detail

run_docker_topology() {
  # run_docker_topology <id> <desc> <script> <compose-file> [svc=hostport:ctport...]
  local id="$1" desc="$2" script="$3" compose="$4"; shift 4
  selected "$id" || return 0
  if ! have_docker; then
    bold "== $id ($desc): SKIP — no usable Docker on this host"
    record "$id" "SKIP" "no docker — run inside CT 101"
    return 0
  fi
  local compose_files="$compose"
  if [[ "$PORT_OFFSET" != 0 && $# -gt 0 ]]; then
    compose_files="$compose $(write_override "$@")"
  fi
  bold "== $id ($desc) =="
  if COMPOSE_FILE="$compose_files" SERVER_URL="$SERVER_URL" KB_URL="$KB_URL" \
       "$SCRIPTS/$script" --up $DOWN; then
    record "$id" "PASS" "$desc"
  else
    record "$id" "FAIL" "$desc"
  fi
}

# --- Docker topologies ----------------------------------------------------
# Trailing svc=host:ctr specs name which published ports to remap by
# PORT_OFFSET; ignored when --port-offset is 0.
run_docker_topology T1 "Docker kb-only"            aimee-kb-docker-smoke.sh                compose.yaml                     "aimee-kb=${KB_PORT}:8741"
run_docker_topology T2 "Docker server+kb split"    aimee-server-docker-smoke.sh            compose.server.yaml              "aimee-server=${SERVER_PORT}:8743" "aimee-kb=${KB_PORT}:8741"
run_docker_topology T3 "Docker server standalone"  aimee-server-standalone-docker-smoke.sh compose.server-standalone.yaml   "aimee-server=${SERVER_PORT}:8743"

# --- Local topologies (Linux only) ----------------------------------------
if selected T5; then
  if is_linux; then
    bold "== T5 (Local full stack) =="
    if MODE=full "$SCRIPTS/aimee-local-stack-e2e.sh"; then record T5 PASS "local full stack"; else record T5 FAIL "local full stack"; fi
  else
    bold "== T5: SKIP — local install is Linux-only"; record T5 SKIP "non-Linux host"
  fi
fi

if selected T6; then
  if is_linux; then
    bold "== T6 (Local server + Docker kb hybrid) =="
    if MODE=hybrid KB_URL="$KB_URL" "$SCRIPTS/aimee-local-stack-e2e.sh"; then record T6 PASS "hybrid"; else record T6 FAIL "hybrid"; fi
  else
    bold "== T6: SKIP — local install is Linux-only"; record T6 SKIP "non-Linux host"
  fi
fi

if selected AD; then
  if is_linux; then
    bold "== AD (Thin-client adoption / TOFU enrollment) =="
    if "$SCRIPTS/aimee-thinclient-adoption-e2e.sh"; then record AD PASS "thin-client adoption"; else record AD FAIL "thin-client adoption"; fi
  else
    bold "== AD: SKIP — local build is Linux-only"; record AD SKIP "non-Linux host"
  fi
fi

# --- Thin client (against the split-stack server) -------------------------
if selected PC; then
  bold "== PC (Thin-client smoke) =="
  if "$SCRIPTS/aimee-thin-client-smoke.sh"; then record PC PASS "thin client"; else record PC FAIL "thin client"; fi
fi

# --- Summary --------------------------------------------------------------
echo
bold "================= E2E matrix summary ================="
printf '  %-4s  %-6s  %s\n' "ID" "RESULT" "DETAIL"
fails=0
for row in "${ROWS[@]}"; do
  IFS='|' read -r id result detail <<<"$row"
  case "$result" in
    PASS) printf '  \033[32m%-4s  %-6s\033[0m  %s\n' "$id" "$result" "$detail" ;;
    FAIL) printf '  \033[31m%-4s  %-6s\033[0m  %s\n' "$id" "$result" "$detail"; fails=$((fails + 1)) ;;
    *)    printf '  \033[33m%-4s  %-6s\033[0m  %s\n' "$id" "$result" "$detail" ;;
  esac
done
echo "======================================================"
[[ "$fails" == 0 ]] || { bold "RESULT: FAIL ($fails topolog(ies) failed)"; exit 1; }
bold "RESULT: all RUN topologies passed (SKIPs are not passes — see table)"
