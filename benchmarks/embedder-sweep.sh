#!/bin/bash
# embedder-sweep.sh: benchmark multiple embedding configurations on LoCoMo and
# LongMemEval and produce a comparative summary.
#
# The Aimee direct entry points invoked below were removed, so this is not
# currently runnable coverage. Their Go replacement is specified in
# docs/proposals/pending/dataset-benchmark-direct-track.md.
#
# Each candidate is identified by a short name and an embedding command that
# accepts text on stdin and writes a JSON float array to stdout.
#
# Usage:
#   ./benchmarks/embedder-sweep.sh [--models file] [--max-samples N] [--max-cases N]
#
# --models FILE   Path to a newline-separated model config file.
#                 Format: <name>  <command>
#                 e.g.:  minilm  python3 scripts/embed.py --model all-MiniLM-L6-v2
#                 Lines starting with '#' are ignored.
#                 Defaults to benchmarks/embedder-candidates.txt.
#
# Environment variables honoured:
#   AIMEE_BENCH_DATA_DIR    Root of dataset files (default: <repo>/data)
#   AIMEE_BENCH_RESULTS_DIR Output directory (default: <repo>/benchmarks/results)
#   AIMEE_BENCH_MAX_SAMPLES Max LoCoMo samples per model (default: 0 = all)
#   AIMEE_BENCH_MAX_CASES   Max LongMemEval cases per model (default: 0 = all)

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DATA_DIR="${AIMEE_BENCH_DATA_DIR:-${ROOT_DIR}/data}"
RESULTS_DIR="${AIMEE_BENCH_RESULTS_DIR:-${ROOT_DIR}/benchmarks/results}"
SWEEP_DIR="${RESULTS_DIR}/embedder-sweep"
MAX_SAMPLES="${AIMEE_BENCH_MAX_SAMPLES:-0}"
MAX_CASES="${AIMEE_BENCH_MAX_CASES:-0}"
MODELS_FILE="${ROOT_DIR}/benchmarks/embedder-candidates.txt"
GIT_SHA=$(git -C "${ROOT_DIR}" rev-parse --short HEAD)
RUN_DATE=$(date -u +%Y%m%dT%H%M%SZ)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --models)       MODELS_FILE="${2:-}"; shift 2 ;;
    --max-samples)  MAX_SAMPLES="${2:-0}"; shift 2 ;;
    --max-cases)    MAX_CASES="${2:-0}"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -f "${MODELS_FILE}" ]]; then
  echo "models file not found: ${MODELS_FILE}" >&2
  echo "Create it (see benchmarks/embedder-candidates.txt.example) or use --models <path>" >&2
  exit 1
fi

mkdir -p "${SWEEP_DIR}"

LOCOMO_DATASET="${DATA_DIR}/locomo/locomo10.json"
LONGMEMEVAL_DATASET="${DATA_DIR}/longmemeval/longmemeval_s_cleaned.json"

for dataset_path in "${LOCOMO_DATASET}" "${LONGMEMEVAL_DATASET}"; do
  if [[ ! -f "${dataset_path}" ]]; then
    echo "missing dataset: ${dataset_path}" >&2
    echo "run ./scripts/download-memory-benchmarks.sh or set AIMEE_BENCH_DATA_DIR" >&2
    exit 1
  fi
done

# ---- per-model sweep --------------------------------------------------------

SUMMARY_FILE="${SWEEP_DIR}/summary_${RUN_DATE}.txt"
: > "${SUMMARY_FILE}"

echo "Embedder sweep — ${RUN_DATE}" | tee -a "${SUMMARY_FILE}"
echo "git: ${GIT_SHA}" | tee -a "${SUMMARY_FILE}"
echo "" | tee -a "${SUMMARY_FILE}"

while IFS=$'\t ' read -r model_name embed_cmd_rest || [[ -n "${model_name}" ]]; do
  # Skip blank lines and comments
  [[ -z "${model_name}" || "${model_name}" == \#* ]] && continue

  echo "=== model: ${model_name} ===" | tee -a "${SUMMARY_FILE}"

  # NOTE: nothing in the tree reads AIMEE_EMBEDDING_COMMAND. The embedder is the
  # `embedding_command` config key, so whoever restores this harness must set
  # that key per candidate and restore the operator's value afterwards.
  # Left inert rather than wired up, because the entry points below are gone.
  export AIMEE_EMBEDDING_COMMAND="${embed_cmd_rest}"

  locomo_out="${SWEEP_DIR}/locomo_${model_name}_direct_v${GIT_SHA}.json"
  lme_out="${SWEEP_DIR}/longmemeval_${model_name}_direct_v${GIT_SHA}.json"

  echo "  running LoCoMo (${locomo_out})..."
  if PYTHONPATH="${ROOT_DIR}" python3 \
      "${ROOT_DIR}/benchmarks/locomo/bench_aimee_direct.py" \
      --dataset "${LOCOMO_DATASET}" \
      --max-samples "${MAX_SAMPLES}" \
      --output "${locomo_out}" 2>&1; then
    PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/verify_scores.py" \
        "${locomo_out}" 2>&1 | tee -a "${SUMMARY_FILE}"
  else
    echo "  [FAILED] LoCoMo run for ${model_name}" | tee -a "${SUMMARY_FILE}"
  fi

  echo "  running LongMemEval (${lme_out})..."
  if PYTHONPATH="${ROOT_DIR}" python3 \
      "${ROOT_DIR}/benchmarks/longmemeval/bench_aimee_direct.py" \
      --dataset "${LONGMEMEVAL_DATASET}" \
      --max-cases "${MAX_CASES}" \
      --output "${lme_out}" 2>&1; then
    PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/verify_scores.py" \
        "${lme_out}" 2>&1 | tee -a "${SUMMARY_FILE}"
  else
    echo "  [FAILED] LongMemEval run for ${model_name}" | tee -a "${SUMMARY_FILE}"
  fi

  echo "" | tee -a "${SUMMARY_FILE}"

done < "${MODELS_FILE}"

echo "Sweep complete. Summary: ${SUMMARY_FILE}"
