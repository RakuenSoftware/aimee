#!/usr/bin/env bash
# Run one or more cells of the 14-task am_ corpus.
#
# This exists because the environment below was reconstructed twice from cell
# artifacts, at a cost of hours each time, and got it wrong on both occasions.
# Every value here is load-bearing; the failure mode for most of them is not an
# error but a silently different experiment.
#
# Usage (from the test container, as root):
#   am-corpus-run.sh <arm[,arm...]> <task[,task...]> [replicates]
#   am-corpus-run.sh aimee am_1f0f1ab528 1
#   am-corpus-run.sh baseline,aimee all 1
#
# THE RUNNER. Two copies of codex_matrix_runner.py existed. The one under
# /root/bench/battery was an older revision that derived fixture/hidden/tasks/
# results from its OWN directory and ignored PT_FIXTURE / PT_HIDDEN / PT_TASKS /
# PT_RESULTS entirely. Pointing PT_TASKS at the am_ corpus while invoking it
# produced the unrelated 50-task synthetic battery with no error and no warning
# -- which is the likeliest origin of the "where did 50 tasks come from" episode.
# That copy is now a symlink to the canonical runner on 401/402/403, so either
# path behaves identically. Verify before trusting a run:
#   grep -c PT_FIXTURE "$RUNNER"      # must be >= 1
set -euo pipefail

ARMS="${1:?usage: am-corpus-run.sh <arms> <tasks> [replicates]}"
TASKS="${2:?usage: am-corpus-run.sh <arms> <tasks> [replicates]}"
REPLICATES="${3:-1}"

RUNNER_DIR=/opt/bench/ponytail-codex-benchmark/battery
RUNNER="$RUNNER_DIR/codex_matrix_runner.py"

if [ "$(grep -c PT_FIXTURE "$RUNNER" || true)" -lt 1 ]; then
  echo "FATAL: $RUNNER ignores PT_FIXTURE -- it is the stale pre-corpus revision." >&2
  echo "It would silently run the 50-task synthetic battery instead." >&2
  exit 2
fi

# --- corpus (ignored entirely by the stale runner) --------------------------
export PT_FIXTURE=/opt/bench/amcorpus/corpus
export PT_HIDDEN=/opt/bench/amcorpus/hidden
export PT_TASKS=/opt/bench/amcorpus/arms/tasks.tsv
export PT_RESULTS=/opt/bench/results
export PT_RUNTIME=/var/lib/aimee-workspaces/bench    # cells/ and raw/ hang off this

# --- host tooling. PT_HOME defaults to /home/virant, which does not exist in
# the containers; everything derived from it (codex, auth, marketplace) then
# resolves to a missing path and the run refuses to start. -------------------
export PT_HOME=/root                                 # has .codex/auth.json + .agents/plugins
export PT_CODEX=/usr/local/bin/codex                 # NOT $PT_HOME/.local/bin/codex
export PT_CODEX_AUTH=/root/.codex/auth.json
export PT_AIMEE=/usr/local/bin/aimee
export PT_PONYTAIL=/opt/bench/ponytail-upstream
export AIMEE_HOME=/var/lib/aimee

# --- aimee arm ---------------------------------------------------------------
export PT_SKIP_KB_BUILD=0
export PT_AIMEE_MODE=full
export PT_EMBED_WAIT_SECS=900
export PT_WS_OWNER=999:999                           # a tree owned by anyone else scans
                                                     # clean with ZERO files, silently

# --- readiness probe. These identifiers belong to the CORPUS, not the harness.
# The stock defaults (end_of_month / app/dates.py) fail every cell by
# construction on this corpus. ------------------------------------------------
export PT_PROBE_SYMBOL=dstr_append
export PT_PROBE_FILE=src/dstr.c
export PT_PROBE_CALLERS=anchor_format_read,ensure_codex_trusted_project_in_config,diff_format_unified

# --- timeouts. The hidden test for this corpus is a full C build; the stock
# 20s/30s defaults time out every cell, which reads as a task failure rather
# than a harness one. am_a7f183fd10 produced no cells at all until this was
# raised. ---------------------------------------------------------------------
export PT_GRADE_TIMEOUT=2700
export PT_RED_TIMEOUT=2700
export PT_BUILD_TIMEOUT=2400

# The aimee arm's MCP surface is served by aimee-server (the CLI only proxies via
# `aimee mcp-serve`), and the skill text ships in the Codex plugin bundle under
# $PT_HOME/.agents/plugins. After installing a new CLI or server image, confirm
# BOTH or the run silently measures the old build:
#   docker inspect aimee-aimee-server-1 --format '{{.Config.Image}}'
#   grep -c "<expected new string>" "$PT_HOME/.agents/plugins/plugins/aimee/skills/aimee/SKILL.md"
# The bundle regenerates on any aimee CLI invocation with HOME=$PT_HOME.
#
# DEPLOYING A NEW IMAGE HERE. Two traps, both hit in one sitting:
#
#  1. The server and the KB are owned by DIFFERENT compose projects and
#     directories -- aimee-server from /opt/aimee-src/compose.server-managed.yaml
#     (which has a .env), aimee-kb from
#     /opt/aimee-now/deploy/container/aimee-managed.compose.yaml (which does NOT).
#     Editing the wrong .env, or letting compose infer the project name from the
#     directory, silently creates a SECOND container on the default :latest image
#     and leaves the real service untouched. Always read the labels off the
#     running container and replay them:
#       docker inspect <container> --format '{{index .Config.Labels "com.docker.compose.project"}}'
#       docker compose -p <project> --project-directory <working_dir> -f <config_file> up -d <service>
#
#  2. The KB's runtime environment is NOT in any .env -- it came from whatever
#     shell first started it. Recreating the container drops every variable you
#     do not re-supply, and the KB then refuses to start ("no embedder selected"),
#     because serving a corpus against a different embedding model would silently
#     return wrong neighbours. Capture the env BEFORE recreating:
#       docker inspect <container> --format '{{range .Config.Env}}{{println .}}{{end}}'
#     At minimum EMBEDDER_MODEL=bekko-a25m must be present; the compose file also
#     reads EMBEDDER_DIMS, EMBEDDER_URL, AIMEE_LLM_HOST and the SYNTHESIS_* set.
#
# Keep aimee-server and aimee-kb on the SAME build. The index, symbol lookup and
# blast-radius answers the aimee arm depends on come from the KB, so a newer
# server against an older KB measures a combination that does not exist.

cd "$RUNNER_DIR"
echo "runner : $RUNNER"
echo "corpus : $PT_TASKS ($(wc -l < "$PT_TASKS") tasks)"
echo "arms   : $ARMS"
echo "tasks  : $TASKS  x$REPLICATES"
exec python3 codex_matrix_runner.py run \
  --arms "$ARMS" --tasks "$TASKS" --replicates "$REPLICATES" --force --timeout 2700
