set -u
# Usage: TASK=am_xxx ARMS=all bash run-task.sh
TASK="${TASK:?}"
ARMS="${ARMS:-all}"
LOG="${LOG:-run-$TASK.log}"

cd /opt/bench/ponytail-codex-benchmark/battery
# Clean ONLY the arms being run. Cleaning all four while re-running one wipes the
# control results for that task -- which is exactly what happened to
# am_edb3594485's baseline/ponytail cells and cost a re-run to recover.
CLEAN_ARMS="$ARMS"
[ "$CLEAN_ARMS" = "all" ] && CLEAN_ARMS="baseline ponytail-instructions ponytail-addon aimee"
for arm in $(echo "$CLEAN_ARMS" | tr ',' ' '); do
  rm -rf "/var/lib/aimee-workspaces/bench/cells/${arm}__${TASK}__r1"
  rm -rf "/opt/bench/results/cells/${arm}__${TASK}__r1"
done
echo "cleaned $TASK cells"

export AIMEE_HOME=/var/lib/docker/volumes/aimee_aimee-server-home/_data
export PT_AIMEE=/usr/local/bin/aimee PT_AIMEE_MODE=index-only
export PT_BUILD_TIMEOUT=2400 PT_CHECK_TIMEOUT=600
export PT_CODEX=/usr/local/bin/codex PT_CODEX_AUTH=/root/.codex/auth.json
export PT_COMPILE_CMD=skip PT_FIXTURE=/opt/bench/amcorpus/corpus
export PT_GRADE_TIMEOUT=2400 PT_HIDDEN=/opt/bench/amcorpus/hidden
export PT_HOME=/root PT_MARKETPLACE_MANIFEST=/root/.agents/plugins/marketplace.json
export PT_MARKETPLACE_ROOT=/root PT_PONYTAIL=/opt/bench/ponytail-upstream
export PT_PROBE_CALLERS=dstr_append_str,dstr_append_char
export PT_PROBE_FILE=src/dstr.c PT_PROBE_SYMBOL=dstr_append
export PT_RED_TIMEOUT=2400 PT_RESULTS=/opt/bench/results
export PT_RUNTIME=/var/lib/aimee-workspaces/bench PT_SCAN_TIMEOUT=1800
export PT_SKIP_KB_BUILD=1 PT_SMOKE_CMD=skip
export PT_TASKS=/opt/bench/amcorpus/arms/tasks.tsv PT_WS_OWNER=1000:1000

nohup timeout 10800 python3 codex_matrix_runner.py run \
  --arms "$ARMS" --tasks "$TASK" --replicates 1 --timeout 1800 --force \
  > "$LOG" 2>&1 &
disown
echo "launched $TASK arms=$ARMS pid=$! log=$LOG"
