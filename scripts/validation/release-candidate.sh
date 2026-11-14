#!/usr/bin/env bash
# Shipped-image release qualification on a disposable Docker host. Every e2e
# driver owns and removes its stack. LUKS qualification additionally requires
# the privileged CI runner; this portable gate covers plain storage.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${AIMEE_APPLICATION_IMAGE:?name the candidate application image}"
: "${AIMEE_POSTGRES_IMAGE:?name the candidate PostgreSQL image}"
: "${AIMEE_EMBEDDER_IMAGE:?name the candidate embedder image}"
: "${AIMEE_EVIDENCE_DIR:?choose a persistent evidence directory}"
mkdir -p "$AIMEE_EVIDENCE_DIR"
O=$(realpath "$AIMEE_EVIDENCE_DIR")
failures=0
ck() {
    local name=$1; shift
    if "$@" > "$O/$name.log" 2>&1; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s (see %s/%s.log)\n' "$name" "$O" "$name"
        failures=$((failures + 1))
    fi
}
export AIMEE_POSTGRES_STORAGE=plain
ck kb-typed-facts-e4b env AIMEE_LLM_IMAGE=ghcr.io/rakuensoftware/aimee-llm-e4b:latest python3 tests/e2e/kb-typed-facts-e2e.py --model gemma-4-E4B-it --output "$O/kb-typed-facts-e4b"
ck kb-typed-facts-e2b env AIMEE_LLM_IMAGE=ghcr.io/rakuensoftware/aimee-llm-e2b:latest python3 tests/e2e/kb-typed-facts-e2e.py --model gemma-4-E2B-it --output "$O/kb-typed-facts-e2b"
ck server-typed-facts-e4b env AIMEE_LLM_IMAGE=ghcr.io/rakuensoftware/aimee-llm-e4b:latest python3 tests/e2e/kb-typed-facts-e2e.py --role server --model gemma-4-E4B-it --output "$O/server-typed-facts-e4b"
ck server-typed-facts-e2b env AIMEE_LLM_IMAGE=ghcr.io/rakuensoftware/aimee-llm-e2b:latest python3 tests/e2e/kb-typed-facts-e2e.py --role server --model gemma-4-E2B-it --output "$O/server-typed-facts-e2b"
ck T1 python3 tests/e2e/deployment-matrix.py --topology T1 --output "$O/T1"
ck T2 python3 tests/e2e/deployment-matrix.py --topology T2 --skip-kb-gates --skip-kb-free-gate --output "$O/T2"
ck T3 python3 tests/e2e/deployment-matrix.py --topology T3 --output "$O/T3"
ck private-verifier env AIMEE_PRIVATE_TEST_REQUIRE_DOCKER=1 AIMEE_PRIVATE_TEST_DOCKER_SOCKET=/var/run/docker.sock AIMEE_PRIVATE_TEST_DOCKER_IMAGE=ubuntu:22.04 go -C server-go test -race -count=1 ./modules/delegates -run '^TestPrivateVerifierRealDocker$'
ck egress-vault python3 tests/e2e/egress-vault-e2e.py --output "$O/egress-vault"
ck postgres-plain python3 tests/e2e/postgres-plain-e2e.py --postgres-image "$AIMEE_POSTGRES_IMAGE" --evidence "$O/postgres-plain.json"
ck kb-upgrade-plain python3 tests/e2e/kb-upgrade-e2e.py --storage plain --output "$O/kb-upgrade-plain"
printf 'release-candidate: %d failed gate(s)\n' "$failures"
test "$failures" -eq 0
