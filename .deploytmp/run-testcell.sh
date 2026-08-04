set -u
cd /opt/bench/ponytail-codex-benchmark/battery
# Environment lifted verbatim from the live CT402 runner so this cell is
# configured identically to the ones it will be compared against.
# AIMEE_HOME is what points the thin client at the server's socket inside the
# docker volume. Without it the client cannot reach the server, tools/list returns
# zero tools, and the model silently greps instead of using the index.
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

# --force is REQUIRED: run_cell() skips any cell whose complete.json already
# exists at the current artifact schema, so without it a re-measurement silently
# returns "skipped" and reports the OLD summary as if it were new.
# am_e1af40a0f5 is the worst cell in the study: baseline solved it in 9 calls,
# aimee took 47 and 2.61x the cost. Best single probe for whether batching moved
# turn count.
nohup timeout 3600 python3 codex_matrix_runner.py run \
  --arms aimee --tasks "am_e1af40a0f5" --replicates 1 --timeout 1800 --force \
  > run-batch-test.log 2>&1 &
disown
echo "launched pid=$!"
