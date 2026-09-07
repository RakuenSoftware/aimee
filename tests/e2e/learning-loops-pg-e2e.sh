#!/bin/bash
# learning-loops-pg-e2e.sh — the recursive self-improvement loops against REAL
# PostgreSQL, with aimee-kb and aimee-server both running.
#
# Why this exists, twice over.
#
# First: these slices each shipped with the consuming half built and the
# producing half absent. Nothing wrote a fate, so regret was permanently zero
# and the detector bar never moved; no evidence probe was installed, so the
# backlog drain refused to run; no sampler was registered, so arm selection
# always fell back to its default. Every one of them passed its unit tests
# throughout, because a unit test can prove a consumer reads a row correctly
# without ever asking whether anything writes one.
#
# Second: the endogeneity gate is a DB2 reader, and DB2 lives in the KB. An
# earlier version of it ran in aimee-server, which builds with
# -DAIMEE_DB2_DISABLED, so it reported "open" by never having consulted a ledger
# at all. A gate that cannot see its own evidence is not a gate.
#
# Neither failure is visible without both services up and a real database
# underneath, which is precisely the environment `make unit-tests` does not have.
#
# Usage:  ./learning-loops-pg-e2e.sh
#   AIMEE_ROOT     dir holding aimee-server / aimee-kb / aimee   (default /root/aimee)
#   AIMEE_SRC      the source tree, for build/obj                (default $AIMEE_ROOT/src)
#   WORKDIR        scratch for HOMEs and logs                    (default /tmp/learning-loops)
#   PGDB           the throwaway database                        (default aimee_shared)
#   AIMEE_DB2_URL  libpq URL reaching that database
#   AIMEE_STORE_URL non-owner PostgreSQL URL for the daemon store (required)
#   AIMEE_STORE_MIGRATION_URL owner URL used only for schema migration
#                             (required and must name a different role)
#   KB_PORT        TCP port for aimee-kb                         (default 18745)
#
# Every assertion prints PASS or FAIL; the script exits non-zero if any failed.
# It DELETES the learning and curiosity tables it seeds. Use a throwaway box.
set -uo pipefail

AIMEE_ROOT="${AIMEE_ROOT:-/root/aimee}"
AIMEE_SRC="${AIMEE_SRC:-$AIMEE_ROOT/src}"
WORKDIR="${WORKDIR:-/tmp/learning-loops}"
KB_PORT="${KB_PORT:-18745}"
PGDB="${PGDB:-aimee_shared}"
export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgres:///$PGDB?host=/var/run/postgresql}"
export AIMEE_STORE_URL="${AIMEE_STORE_URL:-}"
export AIMEE_STORE_MIGRATION_URL="${AIMEE_STORE_MIGRATION_URL:-}"
[ -n "$AIMEE_STORE_URL" ] || {
  echo "AIMEE_STORE_URL is required and must name the non-owner store role" >&2; exit 1;
}
[ -n "$AIMEE_STORE_MIGRATION_URL" ] || {
  echo "AIMEE_STORE_MIGRATION_URL is required and must name the schema owner" >&2; exit 1;
}
OBJ="$AIMEE_SRC/build/obj"
[ -r "$OBJ/module-bundle/grants/kb/config.grant" ] || {
    python3 "$AIMEE_ROOT/scripts/export_c_repositories.py" --runtime-bundle "$OBJ/module-bundle" >/dev/null || exit 1
}

KBHOME="$WORKDIR/kbhome"
SRVHOME="$WORKDIR/srvhome"

PASS=0; FAIL=0
S1_RESULT="NOT RUN"; S2_RESULT="NOT RUN"; S3_RESULT="NOT RUN"
S4_RESULT="NOT RUN"; S5_RESULT="NOT RUN"; S6_RESULT="NOT RUN"
check() { # check <name> <expected> <actual>
  if [ "$2" = "$3" ]; then printf '  PASS  %s\n' "$1"; PASS=$((PASS+1))
  else printf '  FAIL  %s\n        expected: %s\n        actual:   %s\n' "$1" "$2" "$3"; FAIL=$((FAIL+1)); fi
}
ok()  { printf '  PASS  %s\n' "$1"; PASS=$((PASS+1)); }
bad() { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL+1)); }
section() { printf '\n=== %s\n' "$1"; }
q() { psql -d "$PGDB" -tA -c "$1" 2>/dev/null; }

KB_PID=""; SRV_PID=""; MOD_PIDS=""
cleanup() {
    for p in $MOD_PIDS "$SRV_PID" "$KB_PID"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null && wait "$p" 2>/dev/null
    done
    return 0
}
trap cleanup EXIT INT TERM

rm -rf "$KBHOME" "$SRVHOME"
mkdir -p "$KBHOME/.config/aimee/modules.d/kb" "$SRVHOME/.config/aimee/modules.d/server"

# The policy loop is only live when the KB has an optimizer command. Production
# defaults leave it empty, which is the safe no-sidecar posture but makes every
# selection fall back to the shipped arm. This isolated HOME deliberately wires
# the repository's deterministic protocol implementation; section 8 then puts
# one non-default arm under overwhelming posterior pressure.
mkdir -p "$KBHOME/.config/aimee"
printf 'bandit_optimize_command: "/usr/bin/python3 %s/../scripts/bandit-sidecar.py"\nbandit_exploration_fraction: 0\n' \
    "$AIMEE_SRC" > "$KBHOME/.config/aimee/aimee.yaml"

deploy() { # deploy <placement> <grant-name> <home> [executable-name]
    local grant="$OBJ/module-bundle/grants/$1/$2.grant"
    local executable_name="${4:-$2}"
    [ -r "$grant" ] || return 1
    local bin="$OBJ/aimee-module-$executable_name"
    [ -x "$bin" ] || bin="$OBJ/aimee-module"
    [ -x "$bin" ] || return 1
    cp "$bin" "$3/.config/aimee/aimee-module-$executable_name"
    chmod 0755 "$3/.config/aimee/aimee-module-$executable_name"
    local companion declared
    for companion in "$OBJ/module-bundle/grants/$1/"*.grant; do
        declared=$(sed -n 's/^executable=//p' "$companion")
        [ "${declared##*/}" = "aimee-module-$executable_name" ] || continue
        sed "s|^executable=.*|executable=$3/.config/aimee/aimee-module-$executable_name|" "$companion" \
            > "$3/.config/aimee/modules.d/$1/$(basename "$companion")"
    done
}
attach() { # attach <name> <home> <bus> <tag>
    [ -x "$2/.config/aimee/aimee-module-$1" ] || return 1
    local placement="$4"
    [ "$placement" != srv ] || placement=server
    env AIMEE_MODULE_PLACEMENT="$placement" HOME="$2" AIMEE_HOME="$2/.config/aimee" AIMEE_DB1_PATH="$2/.config/aimee/aimee.db" \
        AIMEE_DB2_URL="$AIMEE_DB2_URL" AIMEE_STORE_URL="$AIMEE_STORE_URL" \
        AIMEE_STORE_MIGRATION_URL="$AIMEE_STORE_MIGRATION_URL" \
        "$2/.config/aimee/aimee-module-$1" "$3" > "$WORKDIR/mod-$4-$1.log" 2>&1 &
    MOD_PIDS="$MOD_PIDS $!"
}

section "0  both services, with the modules these loops need"
# learning carries the signal classifier the router needs; without it every
# signal is refused and sections 3-5 below measure nothing.
for m in config learning memory postgres; do deploy kb "$m" "$KBHOME" || { echo "cannot deploy KB module $m" >&2; exit 1; }; done
KBBUS="$KBHOME/.config/aimee/kb-module-bus.sock"
env HOME="$KBHOME" AIMEE_HOME="$KBHOME/.config/aimee" \
    "$AIMEE_ROOT/aimee-kb" --http-port="$KB_PORT" > "$WORKDIR/kb.log" 2>&1 &
KB_PID=$!
for _ in $(seq 1 300); do [ -S "$KBBUS" ] && break; sleep 0.1; done
for m in config learning memory postgres; do attach "$m" "$KBHOME" "$KBBUS" kb; done

KB_URL="http://127.0.0.1:$KB_PORT"
kb_up=0
for _ in $(seq 1 180); do
    curl -fsS --max-time 3 "$KB_URL/v1/health" >/dev/null 2>&1 && { kb_up=1; break; }
    sleep 1
done
check "aimee-kb is serving" "1" "$kb_up"
[ "$kb_up" = 1 ] || { tail -15 "$WORKDIR/kb.log"; printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"; exit 1; }

deploy server config "$SRVHOME"
deploy server postgres "$SRVHOME"
# The `aimee` Go module owns DB1. Its second grant, `aimee-db1`, authorizes the
# same executable's outbound session-family reads; both grants must name the
# one launched binary.
deploy server aimee "$SRVHOME"
deploy server learning "$SRVHOME"
export AIMEE_KB_API_URL="$KB_URL"
SRVBUS="$SRVHOME/.config/aimee/server-module-bus.sock"
SRVSOCK="$SRVHOME/.config/aimee/aimee-http.sock"
env HOME="$SRVHOME" AIMEE_HOME="$SRVHOME/.config/aimee" AIMEE_KB_API_URL="$KB_URL" \
    "$AIMEE_ROOT/aimee-server" --foreground > "$WORKDIR/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 300); do [ -S "$SRVBUS" ] && break; sleep 0.1; done
for m in config postgres aimee learning; do attach "$m" "$SRVHOME" "$SRVBUS" srv; done
for _ in $(seq 1 300); do [ -S "$SRVSOCK" ] && break; sleep 0.2; done
[ -S "$SRVSOCK" ] && ok "aimee-server is serving" || bad "aimee-server never came up"

export HOME="$SRVHOME" AIMEE_HOME="$SRVHOME/.config/aimee"
export AIMEE_API_ENDPOINT="unix:$SRVSOCK"
A="$AIMEE_ROOT/aimee"

# The server socket can appear before the newly attached store module has
# applied its schema. Do not let section 1 race module readiness and report a
# closed gate when the real answer is merely "store not ready yet".
store_up=0
for _ in $(seq 1 120); do
    STORE_PROBE=$("$A" eval candidates --limit 1 2>&1 | head -1)
    printf '%s' "$STORE_PROBE" | grep -q 'could not read candidates' || { store_up=1; break; }
    sleep 0.5
done
check "the daemon store is serving" "1" "$store_up"

section "1  S0: the gate answers to a real ledger, not to a guess"
q "DELETE FROM learning_proposal_fate; DELETE FROM learning_proposals; DELETE FROM learning_signals;" >/dev/null
# Below the sample floor the gate is OPEN and says the ratio is not yet
# meaningful. Failing closed on "no observations" is indistinguishable from
# failing closed on "all endogenous", and would leave a fresh install unable to
# admit anything ever.
GATE=$("$A" eval candidates --limit 1 2>&1 | head -1)
printf '        %s\n' "$GATE"
printf '%s' "$GATE" | grep -q 'open' && ok "an empty ledger leaves the gate open" \
    || bad "an empty window closed the gate"

# 25 committed proposals, every one self-derived: the loop eating its own tail.
# repeat_question is a self-derived detector type, and signal_type overrides
# source precisely so this cannot be laundered by omitting a field.
for i in $(seq 1 25); do
  q "INSERT INTO learning_signals (id, signal_type, source, polarity, title, description, target_key)
       VALUES ($i,'repeat_question','implicit','positive','t$i','d$i','endo-$i');
     INSERT INTO learning_proposals (id, signal_id, sink, state, target_key, action_json, committed_at)
       VALUES ($i,$i,'rule','committed','endo-$i','{}',pg_now_text());" >/dev/null
done
KBGATE=$(curl -sS -m 10 -X POST -H 'Content-Type: application/json' -d '{}' \
         "$KB_URL/v1/actions/learning.endogeneity")
printf '        KB direct:  %s\n' "$(printf '%s' "$KBGATE" | head -c 120)"
printf '%s' "$KBGATE" | grep -q '"gate":"closed"' \
    && ok "the KB closes the gate on a wholly self-referential ledger" \
    || bad "the KB did not close the gate"

# And the DAEMON must report the same thing. This is the defect that hid: the
# gate ran server-side, where DB2 is compiled out, and answered "open" by never
# having consulted the ledger at all.
DGATE=$("$A" eval candidates --limit 1 2>&1 | head -1)
printf '        via daemon: %s\n' "$DGATE"
printf '%s' "$DGATE" | grep -q 'closed' \
    && ok "the daemon reports the gate the KB actually enforces" \
    || bad "the daemon disagrees with the KB about the gate"

section "2  S0: a closed gate refuses admission, and reopening restores it"
# admit writes materialised tasks into a suite directory, so it needs one.
SUITE="$WORKDIR/gated-suite"
mkdir -p "$SUITE"
ADMIT=$("$A" eval candidates-update admit --suite-dir "$SUITE" 2>&1 | head -1)
printf '        %s\n' "$ADMIT"
printf '%s' "$ADMIT" | grep -q '0 admitted' \
    && ok "nothing is admitted while the gate is closed" \
    || bad "admission happened through a closed gate"
# and nothing was written to disk either: a refusal that still materialises the
# task would leave the suite carrying work the gate meant to hold back.
check "no task file was written" "0" "$(find "$SUITE" -type f 2>/dev/null | wc -l)"

# Make a quarter of the ledger exogenous and the floor is met again.
q "UPDATE learning_signals SET signal_type='mark_rule', source='explicit' WHERE id <= 10;" >/dev/null
REOPEN=$("$A" eval candidates --limit 1 2>&1 | head -1)
printf '        %s\n' "$REOPEN"
printf '%s' "$REOPEN" | grep -q 'open' && ok "real outside evidence reopens the gate" \
    || bad "the gate stayed closed after the ledger recovered"

section "3  S1: failed jobs become an admitted regression task"
BEFORE=$FAIL
q "DELETE FROM eval_candidates; DELETE FROM approach_failures; DELETE FROM agent_jobs;
   INSERT INTO agent_jobs (role, prompt, agent_name, status, result) VALUES
     ('execute','Rebuild the evidence index and report the row count.','agent-a','failed',
      'index rebuild aborted'),
     ('execute','Rebuild the evidence index and report the row count.','agent-a','failed',
      'index rebuild aborted'),
     ('execute','Say hello.','agent-a','done','hello');" >/dev/null
SCAN=$("$A" eval candidates-update scan --suite evidence-synth 2>&1)
printf '%s\n' "$SCAN" | sed 's/^/        /'
printf '%s' "$SCAN" | grep -q 'scan: 2 observed' \
    && ok "two failed jobs are observed through the daemon" \
    || bad "the scan did not observe both failed jobs"
check "the repeated failure becomes one candidate" "1" \
      "$(q "SELECT count(*) FROM eval_candidates WHERE origin='agent_job'")"
check "the candidate retains both observations" "2" \
      "$(q "SELECT occurrences FROM eval_candidates WHERE origin='agent_job'")"

GENERATED="$WORKDIR/generated-suite"
mkdir -p "$GENERATED"
ADMIT=$("$A" eval candidates-update admit --suite-dir "$GENERATED" --min-occurrences 2 2>&1)
printf '%s\n' "$ADMIT" | sed 's/^/        /'
printf '%s' "$ADMIT" | grep -q 'admit: 1 admitted' \
    && ok "the reproduced failure is admitted" \
    || bad "the reproduced failure was not admitted"
check "admission materialises one task file" "1" \
      "$(find "$GENERATED" -type f -name '*.json' 2>/dev/null | wc -l)"
check "the candidate ledger records admission" "admitted" \
      "$(q "SELECT state FROM eval_candidates WHERE origin='agent_job'")"
[ "$FAIL" -eq "$BEFORE" ] && S1_RESULT=PASS || S1_RESULT=FAIL

section "4  S3: the same failures become reusable negative knowledge"
BEFORE=$FAIL
check "one dead end is retained for the repeated approach" "1" \
      "$(q "SELECT count(*) FROM approach_failures WHERE source='agent_job'")"
check "the dead end retains both occurrences" "2" \
      "$(q "SELECT occurrences FROM approach_failures WHERE source='agent_job'")"
APPROACHES=$("$A" learning approaches \
    "Rebuild the evidence index and report the row count." 2>&1)
printf '%s\n' "$APPROACHES" | sed 's/^/        /'
printf '%s' "$APPROACHES" | grep -q 'execute via agent-a' \
    && ok "the production recall route returns the failed approach" \
    || bad "the production recall route forgot the failed approach"
printf '%s' "$APPROACHES" | grep -q 'index rebuild aborted' \
    && ok "recall explains how the approach failed" \
    || bad "recall omitted the failure mode"
[ "$FAIL" -eq "$BEFORE" ] && S3_RESULT=PASS || S3_RESULT=FAIL

section "5  S2: attribution reads a paired grid from the real store"
BEFORE=$FAIL
# These rows verify the store -> module -> CLI attribution plumbing. They are
# deliberately an established runner ablation, not invented no_evalgrow,
# no_deadend, or no_supersede labels. Only real runs with those loops disabled
# could support the stronger efficacy claim.
q "DELETE FROM eval_results WHERE suite='evidence-paired';
   INSERT INTO eval_results (suite, task_name, ablation, success) VALUES
     ('evidence-paired','task-a','full',true),
     ('evidence-paired','task-a','no_rescue',false),
     ('evidence-paired','task-b','full',true),
     ('evidence-paired','task-b','no_rescue',false),
     ('evidence-paired','task-c','full',true),
     ('evidence-paired','task-c','no_rescue',false);" >/dev/null
ATTRIBUTION=$("$A" learning attribution evidence-paired 2>&1)
printf '%s\n' "$ATTRIBUTION" | sed 's/^/        /'
printf '%s' "$ATTRIBUTION" | grep -q 'Baseline: full' \
    && ok "attribution names the production baseline" \
    || bad "attribution did not name its baseline"
printf '%s' "$ATTRIBUTION" | grep -qE 'no_rescue[[:space:]]+3[[:space:]]+\+1\.000[[:space:]]+removing it cost us' \
    && ok "three paired tasks carry the expected attribution" \
    || bad "the paired grid did not produce its expected attribution"
[ "$FAIL" -eq "$BEFORE" ] && S2_RESULT=PASS || S2_RESULT=FAIL

section "6  S5: the fate ledger, written by the router and by an operator"
BEFORE=$FAIL
q "DELETE FROM learning_proposal_fate; DELETE FROM learning_proposals; DELETE FROM learning_signals;" >/dev/null
q "INSERT INTO learning_signals (id, signal_type, source, polarity, title, description, target_key)
     VALUES (9001,'mark_rule','explicit','positive','first','the earlier rule','fate-target');
   INSERT INTO learning_proposals (id, signal_id, sink, state, target_key, action_json, committed_at)
     VALUES (9001,9001,'rule','committed','fate-target','{}',pg_now_text());" >/dev/null

# A later commit to the SAME target supersedes the earlier one, unasked. This is
# the producer that could not fire at all while the KB had no classifier.
R=$(curl -sS -m 15 -X POST -H 'Content-Type: application/json' \
    -d '{"signal_type":"mark_rule","source":"explicit","polarity":"positive","title":"second","description":"the later rule","target_key":"fate-target","high_confidence":true}' \
    "$KB_URL/v1/actions/learning.propose_signal")
printf '        %s\n' "$(printf '%s' "$R" | head -c 120)"
check "the later signal was committed" "1" \
      "$(printf '%s' "$R" | grep -c '"committed_ids":\[')"
check "the earlier commit is marked superseded" "superseded" \
      "$(q "SELECT fate FROM learning_proposal_fate WHERE proposal_id=9001")"

# The verdict the router cannot infer: a human says the rule was wrong. It must
# reach the ledger through the daemon, and it must count as regret.
OUT=$("$A" learning fate 9001 contradicted --reason "later evidence" 2>&1 | head -1)
printf '        %s\n' "$OUT"
check "an operator verdict is recorded" "contradicted" \
      "$(q "SELECT fate FROM learning_proposal_fate WHERE proposal_id=9001")"
printf '%s' "$OUT" | grep -q 'counts against the detector' \
    && ok "and it counts against the detector that raised it" \
    || bad "a contradiction was not counted as regret"
[ "$FAIL" -eq "$BEFORE" ] && S5_RESULT=PASS || S5_RESULT=FAIL

section "7  S4: the backlog drain runs a real probe"
BEFORE=$FAIL
q "DELETE FROM curiosity_items;" >/dev/null
q "INSERT INTO curiosity_items (gap_type, target_topic, evidence, importance, novelty, state, source_session)
     VALUES ('weak_coverage','a topic nothing in this store covers','seeded',0.5,0.5,'open','s-e2e');" >/dev/null
OUT=$("$A" learning resolve --budget 5 2>&1)
printf '%s\n' "$OUT" | sed 's/^/        /'
if printf '%s' "$OUT" | grep -q 'No evidence probe is installed'; then
    bad "S4 has no probe installed -- the drain refuses to run"
else
    ok "the drain ran with a real probe"
fi
# A gap nothing covers must stay OPEN. Closing it would empty the backlog by
# assertion, which is the failure the refusal above exists to prevent.
printf '%s' "$OUT" | grep -qE 'resolved [0-9]+ of [1-9]' \
    && ok "it considered the seeded gap and reports a real pass" \
    || bad "the drain considered nothing"
check "an uncovered gap is left open, not closed" "open" \
      "$(q "SELECT state FROM curiosity_items WHERE source_session='s-e2e'")"

# The other half, and the one that actually proves the probe DECIDES rather than
# merely declines: a gap whose subject the memory graph does cover must be
# CLOSED. Without this the section passes just as well against a probe hardwired
# to answer "no evidence", which is indistinguishable from the inert version
# this slice shipped with.
q "INSERT INTO memories (id, tier, kind, key, content)
     VALUES (7700,'L1','fact','e2e-covered-subject','the covered subject is documented here')
   ON CONFLICT (id) DO NOTHING;
   INSERT INTO memory_relations (memory_id, src_entity, relation, dst_entity, fact_text)
     VALUES (7700,'covered subject','documented_in','the handbook',
             'the covered subject is documented in the handbook');" >/dev/null
q "INSERT INTO curiosity_items (gap_type, target_topic, evidence, importance, novelty, state, source_session)
     VALUES ('weak_coverage','covered subject','seeded',0.5,0.5,'open','s-e2e-covered');" >/dev/null
OUT2=$("$A" learning resolve --budget 5 2>&1)
printf '%s\n' "$OUT2" | sed 's/^/        /'
check "a gap the graph covers is closed" "resolved" \
      "$(q "SELECT state FROM curiosity_items WHERE source_session='s-e2e-covered'")"
check "and the uncovered one is still open" "open" \
      "$(q "SELECT state FROM curiosity_items WHERE source_session='s-e2e'")"
printf '%s' "$OUT2" | grep -qE 'resolved [1-9]' \
    && ok "the pass reports the close it made" \
    || bad "the drain closed a gap without reporting it"
[ "$FAIL" -eq "$BEFORE" ] && S4_RESULT=PASS || S4_RESULT=FAIL

section "8  S6: reward pressure selects a non-default declared arm"
BEFORE=$FAIL
# A sampler merely returning the default does not prove the posterior reaches
# selection. Make `brief` essentially certain while keeping `full` as the
# shipped default. With exploration disabled above, the sidecar chooses the MAP
# arm deterministically.
q "DELETE FROM bandit_decisions WHERE decision_point='plan_advisory';
   DELETE FROM bandit_arm_stats WHERE decision_point='plan_advisory';
   INSERT INTO bandit_arm_stats
     (decision_point, arm_id, n_decisions, n_rewards, sum_reward, sum_reward_sq,
      posterior_alpha, posterior_beta)
   VALUES
     ('plan_advisory','off',100,100,0,0,1,101),
     ('plan_advisory','brief',100,100,100,100,101,1),
     ('plan_advisory','full',100,100,0,0,1,101);" >/dev/null
SEL=$(curl -sS -m 10 -X POST -H 'Content-Type: application/json' -d '{}' \
      "$KB_URL/v1/actions/learning.policy_select")
printf '        %s\n' "$SEL"
printf '%s' "$SEL" | grep -q '"arm":"brief"' \
    && ok "reward pressure selects the non-default brief arm" \
    || bad "policy_select did not follow the rewarded non-default arm"
printf '%s' "$SEL" | grep -q '"default_arm":"full"' \
    && ok "sampling did not silently promote the selected arm" \
    || bad "selection changed the declared default"
check "the sampled decision is recorded against brief" "brief" \
      "$(q "SELECT arm_id FROM bandit_decisions
             WHERE decision_point='plan_advisory' ORDER BY decided_at DESC LIMIT 1")"
[ "$FAIL" -eq "$BEFORE" ] && S6_RESULT=PASS || S6_RESULT=FAIL

section "9  the fate ledger's SQL, executed by real Postgres"
# These statements run on the sqlite shim in `make unit-tests`, and sqlite
# accepts SQL that Postgres rejects. They are exercised here for the same reason
# the typed-fact suite exists next door.
q "DELETE FROM learning_proposal_fate; DELETE FROM learning_proposals; DELETE FROM learning_signals;" >/dev/null
q "INSERT INTO learning_signals (id, signal_type, source, title, target_key) VALUES
     (7001,'mark_rule','explicit','a','t1'),
     (7002,'repeat_question','implicit','b','t2'),
     (7003,'mark_rule','explicit','c','t3');
   INSERT INTO learning_proposals (id, signal_id, sink, state, target_key, committed_at) VALUES
     (7001,7001,'rule','committed','t1',pg_now_text()),
     (7002,7002,'rule','committed','t2',pg_now_text()),
     (7003,7003,'rule','committed','t3',pg_now_text());" >/dev/null

# One verdict per proposal: recording twice replaces, it does not accumulate.
q "INSERT INTO learning_proposal_fate (proposal_id, fate) VALUES (7001,'standing')
     ON CONFLICT (proposal_id) DO UPDATE SET fate = EXCLUDED.fate;" >/dev/null
q "INSERT INTO learning_proposal_fate (proposal_id, fate) VALUES (7001,'reverted')
     ON CONFLICT (proposal_id) DO UPDATE SET fate = EXCLUDED.fate;" >/dev/null
check "one fate row per proposal" "1" "$(q "SELECT count(*) FROM learning_proposal_fate WHERE proposal_id=7001")"
check "the later verdict wins" "reverted" "$(q "SELECT fate FROM learning_proposal_fate WHERE proposal_id=7001")"

# The delimited LIKE: a fate must not be counted as regret because a LONGER
# regret name starts with it. Give 7002 a fate that a naive prefix match would
# wrongly catch, and 7003 one that genuinely is in the vocabulary.
q "INSERT INTO learning_proposal_fate (proposal_id, fate) VALUES (7002,'reverted_by_operator')
     ON CONFLICT (proposal_id) DO UPDATE SET fate = EXCLUDED.fate;
   INSERT INTO learning_proposal_fate (proposal_id, fate) VALUES (7003,'contradicted')
     ON CONFLICT (proposal_id) DO UPDATE SET fate = EXCLUDED.fate;" >/dev/null
REGRET=$(q "SELECT SUM(CASE WHEN f.fate IS NOT NULL
              AND (',' || 'reverted,contradicted' || ',') LIKE ('%,' || f.fate || ',%')
              THEN 1 ELSE 0 END)
            FROM learning_proposals p
            JOIN learning_signals s ON s.id = p.signal_id
            LEFT JOIN learning_proposal_fate f ON f.proposal_id = p.id
            WHERE p.state='committed'")
check "a longer fate name is not counted by prefix" "2" "$REGRET"
check "and the excluded row is the prefix one" "reverted_by_operator" \
      "$(q "SELECT fate FROM learning_proposal_fate WHERE proposal_id=7002")"

# Provenance grouping, which is what the S0 gate counts.
check "committed proposals group by signal type" "mark_rule:2 repeat_question:1" \
      "$(q "SELECT string_agg(t, ' ' ORDER BY t) FROM (
              SELECT s.signal_type || ':' || count(*) AS t
              FROM learning_proposals p JOIN learning_signals s ON s.id = p.signal_id
              WHERE p.state='committed' GROUP BY s.signal_type) x")"

section "10  the surfaces refuse bad input instead of breaking on it"
# The exploratory pass. None of these should crash a service or answer as though
# the request made sense; the assertion at the end of this section is that both
# daemons are still standing and nothing below section 11 went silent.
probe() { # probe <label> <argv...>
    local label="$1"; shift
    local out
    out=$("$@" 2>&1 | head -1)
    printf '        %-30s %s\n' "$label" "$(printf '%s' "$out" | cut -c1-64)"
}
probe "negative limit"        "$A" eval candidates --limit -5
probe "limit past any cap"    "$A" eval candidates --limit 99999999
probe "unknown state filter"  "$A" eval candidates --state not-a-state
probe "fate, no id"           "$A" learning fate
probe "fate, unknown id"      "$A" learning fate 424242 contradicted
probe "fate, bad verdict"     "$A" learning fate 7001 not-a-verdict
probe "resolve, huge budget"  "$A" learning resolve --budget 100000
# --budget 0 is NOT "do nothing": curiosity_resolve_pass treats any budget <= 0
# as unset and substitutes its default, so an operator asking for none still
# gets a full pass. That is deliberate and worth pinning down, because the
# output otherwise reads as the command ignoring its argument.
ZERO=$("$A" learning resolve --budget 0 2>&1 | head -1)
printf '        %-30s %s\n' "resolve, zero budget" "$(printf '%s' "$ZERO" | cut -c1-64)"
printf '%s' "$ZERO" | grep -qE 'budget (25|[1-9][0-9]*)\)' \
    && ok "a zero budget means unset, and the default pass runs" \
    || bad "a zero budget did something other than fall back to the default"
probe "approaches, empty goal" "$A" learning approaches ""
probe "attribution, no suite" "$A" learning attribution
probe "admit, missing dir"    "$A" eval candidates-update admit --suite-dir /nonexistent/nowhere
probe "unknown op"            "$A" eval candidates-update not-an-op
# A malformed body must not be answered as if it parsed.
BAD=$(curl -sS -m 10 -X POST -H 'Content-Type: application/json' -d '{"signal_type":' \
      "$KB_URL/v1/actions/learning.propose_signal" 2>&1 | head -c 100)
printf '        %-30s %s\n' "malformed JSON body" "$BAD"
printf '%s' "$BAD" | grep -q '"status":"ok"' \
    && bad "a malformed body was accepted" \
    || ok "a malformed body is not accepted"
# An empty signal_type is the field the router refuses on; it must say so.
EMPTY=$(curl -sS -m 10 -X POST -H 'Content-Type: application/json' -d '{"signal_type":""}' \
        "$KB_URL/v1/actions/learning.propose_signal" 2>&1 | head -c 100)
printf '        %-30s %s\n' "empty signal_type" "$EMPTY"
printf '%s' "$EMPTY" | grep -q '"status":"ok"' \
    && bad "an empty signal_type was accepted" \
    || ok "an empty signal_type is refused"

section "11  neither service reported a missing provider, and neither crashed"
PATTERNS='classification unavailable|provider (is )?(not registered|unavailable|missing)|no provider|verdict=TRANSPORT|module call failed'
for log in "$WORKDIR/kb.log" "$WORKDIR/server.log"; do
    n=$(grep -iE "$PATTERNS" "$log" 2>/dev/null | grep -vc 'NOTICE:')
    if [ "${n:-0}" -gt 0 ]; then
        bad "$(basename "$log"): $n line(s) report a missing provider"
        grep -iE "$PATTERNS" "$log" | grep -v 'NOTICE:' | sort -u | head -5 | sed 's/^/          /'
    else
        ok "$(basename "$log"): nothing reports a missing provider"
    fi
done
kill -0 "$KB_PID" 2>/dev/null && ok "aimee-kb survived" || bad "aimee-kb died"
kill -0 "$SRV_PID" 2>/dev/null && ok "aimee-server survived" || bad "aimee-server died"

printf '\n=== six-loop live evidence\n'
printf '  %-4s %-9s %s\n' "LOOP" "RESULT" "OBSERVATION"
printf '  %-4s %-9s %s\n' "S1" "$S1_RESULT" "failed jobs -> admitted regression task"
printf '  %-4s %-9s %s\n' "S2" "$S2_RESULT" "paired-grid attribution (plumbing, not new-loop efficacy)"
printf '  %-4s %-9s %s\n' "S3" "$S3_RESULT" "failed approach -> recalled dead end"
printf '  %-4s %-9s %s\n' "S4" "$S4_RESULT" "evidence-backed curiosity close"
printf '  %-4s %-9s %s\n' "S5" "$S5_RESULT" "supersession and operator regret"
printf '  %-4s %-9s %s\n' "S6" "$S6_RESULT" "rewarded non-default policy arm"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
