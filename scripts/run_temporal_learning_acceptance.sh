#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
"$root/scripts/check_semantic_retrieval_boundary.sh" "$@"
"$root/scripts/test_temporal_assertion_retrieval.sh" "$@"
"$root/scripts/test_exact_evidence_attribution.sh" "$@"
"$root/scripts/test_observation_contract.sh" "$@"
"$root/scripts/test_failure_learning_loop.sh" "$@"
"$root/scripts/test_derived_context_security.sh" "$@"
"$root/scripts/run_memory_sufficiency_gate.sh" "$@"
"$root/scripts/run_learning_observation_gate.sh" "$@"
"$root/scripts/test_observation_retrieval_failure_modes.sh" "$@"
