#!/usr/bin/env bash
#
# e2e-matrix.sh — run the aimee deploy-matrix E2E below and print one pass/fail
# table. Runs ON the host it is invoked on: on a Docker host it can run the
# container topologies (T1-T3); on a Linux box with the build deps it can run the
# local topologies (T5-T6). For the pve runs, invoke this INSIDE CT 101
# (ssh root@192.168.1.253 -> pct exec 101).
#
# Topologies:
#   T1  Docker kb-only                 (compose.kb.yaml)
#   T2  Docker server + optional KB    (two isolated compositions)
#   T3  Docker server standalone       (compose.yaml)
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

# Docker topologies use the real local model in its own container. Each fixture
# owns isolated networks and requests unused host ports automatically.
export COMPOSE_PROFILES="${COMPOSE_PROFILES:-}"
# These topology smokes exercise the machine APIs, not browser authentication.
# Disable webchat instead of inventing a credential fixture outside Vault.
export AIMEE_RUNTIME_WEB_ENABLED="${AIMEE_RUNTIME_WEB_ENABLED:-0}"

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

docker_images_built=0
run_docker_topology() {
  local id="$1" desc="$2"
  selected "$id" || return 0
  if ! have_docker; then
    record "$id" FAIL "Docker is required for the selected topology"
    return 0
  fi
  export AIMEE_APPLICATION_IMAGE="${AIMEE_APPLICATION_IMAGE:-aimee:e2e}"
  export AIMEE_POSTGRES_IMAGE="${AIMEE_POSTGRES_IMAGE:-aimee-postgres:e2e}"
  export AIMEE_EMBEDDER_IMAGE="${AIMEE_EMBEDDER_IMAGE:-$(cat tests/e2e/embedder-image.txt)}"
  if [[ "${AIMEE_E2E_SKIP_BUILD:-0}" != 1 && "$docker_images_built" == 0 ]]; then
    if ! docker build --build-arg WITH_VSCODE=0 -f Dockerfile.server -t "$AIMEE_APPLICATION_IMAGE" . ||
       ! docker build -f Dockerfile.postgres -t "$AIMEE_POSTGRES_IMAGE" . ||
       ! (docker image inspect "$AIMEE_EMBEDDER_IMAGE" >/dev/null 2>&1 ||
          docker pull "$AIMEE_EMBEDDER_IMAGE"); then
      record "$id" FAIL "candidate image build failed"
      return 0
    fi
    docker_images_built=1
  fi
  local evidence
  evidence=$(mktemp -d "/tmp/aimee-${id}-evidence-XXXXXX")
  local flags=()
  [[ -n "$DOWN" ]] || flags+=(--keep)
  bold "== $id ($desc); evidence: $evidence =="
  if python3 tests/e2e/deployment-matrix.py --topology "$id" --output "$evidence" "${flags[@]}"; then
    record "$id" PASS "$desc"
  else
    record "$id" FAIL "$desc; see $evidence"
  fi
}

run_docker_topology T1 "Docker shared KB with encrypted PostgreSQL"
run_docker_topology T2 "Independent Server enrolled into optional KB"
run_docker_topology T3 "KB-free Server with local semantic memory"

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
