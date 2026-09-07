#!/usr/bin/env bash
# Validate a fresh isolated Server connected to an optional shared KB.
# Requires locally built AIMEE_APPLICATION_IMAGE, AIMEE_POSTGRES_IMAGE and
# AIMEE_EMBEDDER_IMAGE. Images are never pulled or built implicitly.
# All resources belong to a random disposable project; cleanup is automatic.
# --up / --down remain accepted for existing callers. Use --keep to retain fixtures.
set -euo pipefail
cd "$(dirname "$0")/.."
args=()
for arg in "$@"; do
  case "$arg" in
    --up|--down) ;;
    --keep) args+=(--keep) ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
output=${AIMEE_E2E_OUTPUT:-$(mktemp -d /tmp/aimee-t2-smoke.XXXXXX)}
exec python3 tests/e2e/deployment-matrix.py --topology T2 --output "$output" "${args[@]}"
