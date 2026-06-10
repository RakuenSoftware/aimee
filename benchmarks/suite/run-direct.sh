#!/bin/bash
# benchmarks/suite/run-direct.sh
# Direct-track benchmark dispatch for the unified suite.
#
# Usage: run-direct.sh [--target <name>] [--bench <id,...>]
#
# --target  Target name (default: aimee). Supports: aimee, model_only, small_agent, rag_chromadb.
# --bench   Comma-separated benchmark ids (default: locomo,longmemeval_s).
#
# The aimee target dispatches to existing bench_aimee_direct.py scripts.
# Adapter-based targets (model_only, small_agent, rag_chromadb) dispatch through
# benchmarks/targets/<name>/adapter.py.

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DATA_DIR="${AIMEE_BENCH_DATA_DIR:-${ROOT_DIR}/data}"
RESULTS_DIR="${AIMEE_BENCH_RESULTS_DIR:-${ROOT_DIR}/benchmarks/results}"
MAX_SAMPLES="${AIMEE_BENCH_MAX_SAMPLES:-0}"
MAX_CASES="${AIMEE_BENCH_MAX_CASES:-0}"
GIT_SHA=$(git -C "${ROOT_DIR}" rev-parse --short HEAD)
TARGET="aimee"
BENCH="locomo,longmemeval_s"
CONFIG_VARIANT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET="${2:-}"; shift 2 ;;
    --bench)  BENCH="${2:-}";  shift 2 ;;
    # Tier-B rollout A/B knob: set one config flag for the duration of this run,
    # then restore it. Run twice (KEY=off, KEY=on) to isolate a single flag.
    --config-variant) CONFIG_VARIANT="${2:-}"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "${RESULTS_DIR}"

# --config-variant KEY=VALUE: flip one flag for this run via the aimee CLI,
# capturing the prior value and restoring it on exit (the flag-rollout-readiness
# A/B mechanism — see docs/validation/flag-rollout-readiness.md). The bench run
# itself still needs a live aimee-server + corpora; this only sets the flag.
if [[ -n "${CONFIG_VARIANT}" ]]; then
  if [[ "${CONFIG_VARIANT}" != *=* ]]; then
    echo "--config-variant expects KEY=VALUE (got '${CONFIG_VARIANT}')" >&2
    exit 1
  fi
  AIMEE_BIN="${AIMEE_BIN:-aimee}"
  CV_KEY="${CONFIG_VARIANT%%=*}"
  CV_VAL="${CONFIG_VARIANT#*=}"
  if [[ -z "${CV_KEY}" ]]; then
    echo "--config-variant KEY must be non-empty" >&2
    exit 1
  fi
  CV_PRIOR="$("${AIMEE_BIN}" config get "${CV_KEY}" 2>/dev/null | tail -1 || true)"
  echo "[config-variant] ${CV_KEY}: ${CV_PRIOR:-<unset>} -> ${CV_VAL}" >&2
  "${AIMEE_BIN}" config set "${CV_KEY}" "${CV_VAL}"
  restore_config_variant() {
    if [[ -n "${CV_PRIOR}" ]]; then
      "${AIMEE_BIN}" config set "${CV_KEY}" "${CV_PRIOR}" >/dev/null 2>&1 || true
    fi
    echo "[config-variant] restored ${CV_KEY} -> ${CV_PRIOR:-<unset>}" >&2
  }
  trap restore_config_variant EXIT
fi

IFS=',' read -r -a BENCH_LIST <<< "${BENCH}"

run_aimee_direct() {
  local bench="$1"
  case "${bench}" in
    locomo)
      dataset="${DATA_DIR}/locomo/locomo10.json"
      if [[ ! -f "${dataset}" ]]; then
        echo "missing dataset: ${dataset}" >&2
        echo "run ./scripts/download-memory-benchmarks.sh or set AIMEE_BENCH_DATA_DIR" >&2
        exit 1
      fi
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/locomo/bench_aimee_direct.py" \
        --dataset "${dataset}" \
        --max-samples "${MAX_SAMPLES}" \
        --output "${RESULTS_DIR}/locomo_aimee_direct_v${GIT_SHA}.json"
      ;;
    longmemeval_s)
      dataset="${DATA_DIR}/longmemeval/longmemeval_s_cleaned.json"
      if [[ ! -f "${dataset}" ]]; then
        echo "missing dataset: ${dataset}" >&2
        echo "run ./scripts/download-memory-benchmarks.sh or set AIMEE_BENCH_DATA_DIR" >&2
        exit 1
      fi
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/longmemeval/bench_aimee_direct.py" \
        --dataset "${dataset}" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/longmemeval_aimee_direct_v${GIT_SHA}.json"
      ;;
    longmemeval_m|longmemeval_l)
      variant="${bench#longmemeval_}"
      dataset="${DATA_DIR}/longmemeval/longmemeval_${variant}_cleaned.json"
      if [[ ! -f "${dataset}" ]]; then
        echo "missing dataset: ${dataset}" >&2
        echo "run ./scripts/download-memory-benchmarks.sh or set AIMEE_BENCH_DATA_DIR" >&2
        exit 1
      fi
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/longmemeval/bench_aimee_direct.py" \
        --dataset "${dataset}" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/${bench}_aimee_direct_v${GIT_SHA}.json"
      ;;
    humaneval)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/coding/bench_humaneval.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/humaneval_aimee_direct_v${GIT_SHA}.json"
      ;;
    mbpp_plus)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/coding/bench_mbpp_plus.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/mbpp_plus_aimee_direct_v${GIT_SHA}.json"
      ;;
    gsm8k)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/reasoning/bench_gsm8k.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/gsm8k_aimee_direct_v${GIT_SHA}.json"
      ;;
    math_500)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/reasoning/bench_math500.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/math_500_aimee_direct_v${GIT_SHA}.json"
      ;;
    aime|gpqa|mmlu_pro|bbh|hle|frontiermath|arc_agi_2|drop|logiqa)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/reasoning/bench_${bench}.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/${bench}_aimee_direct_v${GIT_SHA}.json"
      ;;
    bigcodebench|repobench|livecodebench|aider_polyglot|terminalbench)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/coding/bench_${bench}.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/${bench}_aimee_direct_v${GIT_SHA}.json"
      ;;
    swebench_lite|swebench_verified)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/coding/bench_swebench.py" \
        --variant "${bench#swebench_}" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/${bench}_aimee_direct_v${GIT_SHA}.json"
      ;;
    mrcr|ruler|l_eval)
      PYTHONPATH="${ROOT_DIR}" python3 "${ROOT_DIR}/benchmarks/memory/bench_${bench}.py" \
        --max-cases "${MAX_CASES}" \
        --output "${RESULTS_DIR}/${bench}_aimee_direct_v${GIT_SHA}.json"
      ;;
    *)
      echo "direct track: benchmark '${bench}' not yet wired for target '${TARGET}'" >&2
      exit 1
      ;;
  esac
}

run_adapter_direct() {
  local target="$1"
  local bench="$2"
  local adapter_script="${ROOT_DIR}/benchmarks/targets/${target}/adapter.py"

  if [[ ! -f "${adapter_script}" ]]; then
    echo "adapter not found: ${adapter_script}" >&2
    exit 1
  fi

  local output="${RESULTS_DIR}/${bench}_${target}_direct_v${GIT_SHA}.json"

  case "${bench}" in
    humaneval|mbpp_plus|bigcodebench|repobench|livecodebench|aider_polyglot|terminalbench|gsm8k|math_500|aime|gpqa|mmlu_pro|bbh|hle|frontiermath|arc_agi_2|drop|logiqa|mrcr|ruler|l_eval)
      local pillar_script
      case "${bench}" in
        humaneval)   pillar_script="${ROOT_DIR}/benchmarks/coding/bench_humaneval.py" ;;
        mbpp_plus)   pillar_script="${ROOT_DIR}/benchmarks/coding/bench_mbpp_plus.py" ;;
        gsm8k)       pillar_script="${ROOT_DIR}/benchmarks/reasoning/bench_gsm8k.py" ;;
        math_500)    pillar_script="${ROOT_DIR}/benchmarks/reasoning/bench_math500.py" ;;
        bigcodebench|repobench|livecodebench|aider_polyglot|terminalbench)
                     pillar_script="${ROOT_DIR}/benchmarks/coding/bench_${bench}.py" ;;
        mrcr|ruler|l_eval)
                     pillar_script="${ROOT_DIR}/benchmarks/memory/bench_${bench}.py" ;;
        *)           pillar_script="${ROOT_DIR}/benchmarks/reasoning/bench_${bench}.py" ;;
      esac
      PYTHONPATH="${ROOT_DIR}" python3 "${pillar_script}" \
        --target "${target}" \
        --max-cases "${MAX_CASES}" \
        --output "${output}"
      return
      ;;
    locomo)
      dataset="${DATA_DIR}/locomo/locomo10.json"
      ;;
    longmemeval_s)
      dataset="${DATA_DIR}/longmemeval/longmemeval_s_cleaned.json"
      ;;
    longmemeval_m|longmemeval_l)
      dataset="${DATA_DIR}/longmemeval/longmemeval_${bench#longmemeval_}_cleaned.json"
      ;;
    *)
      echo "direct track: benchmark '${bench}' not yet wired for target '${target}'" >&2
      exit 1
      ;;
  esac

  if [[ ! -f "${dataset}" ]]; then
    echo "missing dataset: ${dataset}" >&2
    echo "run ./scripts/download-memory-benchmarks.sh or set AIMEE_BENCH_DATA_DIR" >&2
    exit 1
  fi

  PYTHONPATH="${ROOT_DIR}" python3 - <<PYEOF
import json, subprocess, sys

adapter_script = '${adapter_script}'
proc = subprocess.Popen(
    ['python3', adapter_script],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
)

proc.stdin.write(json.dumps({'op': 'describe'}) + '\n')
proc.stdin.flush()
desc_resp = json.loads(proc.stdout.readline())
print(f'Target: {desc_resp["system"]} v{desc_resp["system_version"]}', file=sys.stderr)

with open('${dataset}') as f:
    data = json.load(f)
max_s = ${MAX_SAMPLES}
samples = data[:max_s] if max_s > 0 else data[:10]

results = []
for item in samples:
    events = item.get('events', item.get('context', []))
    proc.stdin.write(json.dumps({'op': 'ingest', 'session_id': item.get('id', 'bench'), 'events': events}) + '\n')
    proc.stdin.flush()
    ingest_resp = json.loads(proc.stdout.readline())
    state_ref = ingest_resp.get('state_ref', '')

    question = item.get('question', item.get('query', ''))
    proc.stdin.write(json.dumps({'op': 'answer', 'state_ref': state_ref, 'question': question, 'budget': {'max_tokens': 512}}) + '\n')
    proc.stdin.flush()
    answer_resp = json.loads(proc.stdout.readline())

    results.append({
        'id': item.get('id', ''),
        'answer': answer_resp.get('answer', ''),
        'retrieved_ids': answer_resp.get('retrieved_ids', []),
        'retrieved_tokens': answer_resp.get('retrieved_tokens', 0),
        'assembled_context_tokens': answer_resp.get('assembled_context_tokens', 0),
        'latency_ms': answer_resp.get('latency_ms', 0),
        'cost_usd': answer_resp.get('cost_usd', 0.0),
    })

proc.stdin.write(json.dumps({'op': 'shutdown'}) + '\n')
proc.stdin.flush()
# Close stdin so the adapter's 'for line in sys.stdin' loop ends; otherwise
# proc.wait() blocks forever on an adapter that keeps reading. Kill on timeout.
proc.stdin.close()
try:
    proc.wait(timeout=10)
except subprocess.TimeoutExpired:
    proc.kill()
    proc.wait(timeout=5)

output = {
    'dataset': '${bench}',
    'track': 'direct',
    'target_system': '${target}',
    'judge_profile': 'none',
    'dataset_hash': '',
    'target_hash': '${GIT_SHA}',
    'seed': 42,
    'results': results,
}
with open('${output}', 'w') as f:
    json.dump(output, f, indent=2)
print(f'Results written to ${output}', file=sys.stderr)
PYEOF
}

case "${TARGET}" in
  aimee)
    for bench in "${BENCH_LIST[@]}"; do
      run_aimee_direct "${bench}"
    done
    ;;
  model_only|small_agent|rag_chromadb|bm25|mem0)
    for bench in "${BENCH_LIST[@]}"; do
      run_adapter_direct "${TARGET}" "${bench}"
    done
    ;;
  *)
    echo "unsupported target '${TARGET}'" >&2
    exit 1
    ;;
esac
