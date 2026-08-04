set -u
CELL=aimee__am_e1af40a0f5__r1
# The previous run left .aimee/worktrees/<id>/main inside the cell checkout, and a
# git worktree registration in .git/worktrees. --force replays the cell but must
# not inherit that state, so remove the workspace and the artifact outright.
rm -rf "/var/lib/aimee-workspaces/bench/cells/$CELL"
rm -rf "/opt/bench/results/cells/$CELL"
echo "cleaned $CELL"

cd /opt/bench/ponytail-codex-benchmark/battery
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

nohup timeout 3600 python3 codex_matrix_runner.py run \
  --arms aimee --tasks "am_e1af40a0f5" --replicates 1 --timeout 1800 --force \
  > run-inv.log 2>&1 &
disown
echo "launched pid=$!"
