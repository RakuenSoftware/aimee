#!/bin/bash
# Controlled paired efficacy study for Aimee self-learning against real
# PostgreSQL with both deployed services and their required modules.
#
# Treatment and control see identical failed jobs. Treatment runs the
# production synthesis pass that turns those jobs into negative approach
# knowledge; control deliberately omits that pass. A deterministic consumer
# receives the production `aimee learning approaches` output and changes its
# seeded first choice only when that choice is reported as previously failed.
# Twenty-four novel tasks check that unrelated history does not change outcomes.
set -euo pipefail

AIMEE_ROOT="${AIMEE_ROOT:-/root/aimee}"
AIMEE_SRC="${AIMEE_SRC:-$AIMEE_ROOT/src}"
WORKDIR="${WORKDIR:-/tmp/self-learning-efficacy}"
OUTPUT_DIR="${OUTPUT_DIR:?OUTPUT_DIR is required}"
KB_PORT="${KB_PORT:-18755}"
PGDB="${PGDB:-aimee_self_learning_efficacy}"
export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgres:///$PGDB?host=/var/run/postgresql}"
export AIMEE_STORE_URL="${AIMEE_STORE_URL:-$AIMEE_DB2_URL}"
OBJ="$AIMEE_SRC/build/obj"
KBHOME="$WORKDIR/kbhome"
SRVHOME="$WORKDIR/srvhome"

mkdir -p "$OUTPUT_DIR/advisories"
RESULTS="$OUTPUT_DIR/results.csv"
printf 'arm,task_class,task_id,first_choice,failed_choice,correct_choice,advisory_present,choice,success\n' > "$RESULTS"

PASS=0
FAIL=0
check() {
    if [ "$2" = "$3" ]; then
        printf '  PASS  %s\n' "$1"
        PASS=$((PASS + 1))
    else
        printf '  FAIL  %s\n        expected: %s\n        actual:   %s\n' "$1" "$2" "$3"
        FAIL=$((FAIL + 1))
    fi
}
section() { printf '\n=== %s\n' "$1"; }
q() { psql -d "$PGDB" -tA -c "$1"; }

KB_PID=""
SRV_PID=""
MOD_PIDS=""
cleanup() {
    for p in $MOD_PIDS "$SRV_PID" "$KB_PID"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null && wait "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

rm -rf "$KBHOME" "$SRVHOME"
mkdir -p "$KBHOME/.config/aimee/modules.d/kb" "$SRVHOME/.config/aimee/modules.d/server"
mkdir -p "$KBHOME/.config/aimee"
printf 'bandit_optimize_command: "/usr/bin/python3 %s/../scripts/bandit-sidecar.py"\nbandit_exploration_fraction: 0\n' \
    "$AIMEE_SRC" > "$KBHOME/.config/aimee/aimee.yaml"

deploy() {
    local placement="$1"
    local grant_name="$2"
    local home="$3"
    local executable_name="${4:-$grant_name}"
    local grant="$OBJ/module-bundle/grants/$placement/$grant_name.grant"
    local bin="$OBJ/aimee-module-$executable_name"
    [ -r "$grant" ] || return 1
    [ -x "$bin" ] || bin="$OBJ/aimee-module"
    [ -x "$bin" ] || return 1
    cp "$bin" "$home/.config/aimee/aimee-module-$executable_name"
    chmod 0755 "$home/.config/aimee/aimee-module-$executable_name"
    sed "s|^executable=.*|executable=$home/.config/aimee/aimee-module-$executable_name|" "$grant" \
        > "$home/.config/aimee/modules.d/$placement/$grant_name.grant"
}

attach() {
    local name="$1"
    local home="$2"
    local bus="$3"
    local tag="$4"
    [ -x "$home/.config/aimee/aimee-module-$name" ] || return 1
    env HOME="$home" AIMEE_HOME="$home/.config/aimee" \
        AIMEE_DB1_PATH="$home/.config/aimee/aimee.db" \
        AIMEE_DB2_URL="$AIMEE_DB2_URL" AIMEE_STORE_URL="$AIMEE_STORE_URL" \
        "$home/.config/aimee/aimee-module-$name" "$bus" \
        > "$WORKDIR/mod-$tag-$name.log" 2>&1 &
    MOD_PIDS="$MOD_PIDS $!"
}

section "deployed services"
for m in config learning memory postgres; do deploy kb "$m" "$KBHOME"; done
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
[ "$kb_up" = 1 ] || { tail -30 "$WORKDIR/kb.log"; exit 1; }

deploy server config "$SRVHOME"
deploy server postgres "$SRVHOME"
deploy server aimee "$SRVHOME"
deploy server aimee-db1 "$SRVHOME" aimee
deploy server aimee-postgres "$SRVHOME" aimee
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
check "aimee-server is serving" "1" "$([ -S "$SRVSOCK" ] && echo 1 || echo 0)"

export HOME="$SRVHOME" AIMEE_HOME="$SRVHOME/.config/aimee"
export AIMEE_API_ENDPOINT="unix:$SRVSOCK"
A="$AIMEE_ROOT/aimee"
store_up=0
for _ in $(seq 1 120); do
    store_probe=$("$A" eval candidates --limit 1 2>&1) || true
    printf '%s' "$store_probe" | grep -q 'could not read candidates' || { store_up=1; break; }
    sleep 0.5
done
check "the daemon store is serving" "1" "$store_up"
[ "$store_up" = 1 ] || exit 1

reset_learning_state() {
    q "DELETE FROM eval_candidates; DELETE FROM approach_failures; DELETE FROM agent_jobs;" >/dev/null
    check "state reset leaves no failed approaches" "0" "$(q 'SELECT count(*) FROM approach_failures')"
}

seed_setup_failures() {
    local i goal alpha beta failed
    for i in $(seq -w 1 24); do
        goal="Recover efficacycase${i} after checksum mismatch"
        alpha="route_${i}_alpha"
        beta="route_${i}_beta"
        if [ $((10#$i % 2)) -eq 0 ]; then failed="$alpha"; else failed="$beta"; fi
        q "INSERT INTO agent_jobs (role, prompt, agent_name, status, result) VALUES
             ('execute','$goal','$failed','failed','$failed returned exit 17'),
             ('execute','$goal','$failed','failed','$failed returned exit 17');" >/dev/null
    done
    check "setup contains two failures for each repeated task" "48" \
        "$(q "SELECT count(*) FROM agent_jobs WHERE status='failed'")"
}

run_consumers() {
    local arm="$1"
    local i goal alpha beta failed correct first second output advisory choice success
    for i in $(seq -w 1 24); do
        goal="Recover efficacycase${i} after checksum mismatch"
        alpha="route_${i}_alpha"
        beta="route_${i}_beta"
        if [ $((10#$i % 2)) -eq 0 ]; then failed="$alpha"; correct="$beta"; else failed="$beta"; correct="$alpha"; fi
        if [ $((10#$i % 4)) -lt 2 ]; then first="$failed"; else first="$correct"; fi
        if [ "$first" = "$alpha" ]; then second="$beta"; else second="$alpha"; fi
        output=$("$A" learning approaches "$goal" 2>&1)
        printf '%s\n' "$output" > "$OUTPUT_DIR/advisories/${arm}-repeat-${i}.txt"
        advisory=0
        printf '%s' "$output" | grep -Fq "execute via $failed" && advisory=1
        if printf '%s' "$output" | grep -Fq "execute via $first"; then choice="$second"; else choice="$first"; fi
        if [ "$choice" = "$correct" ]; then success=1; else success=0; fi
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$arm" repeat "$i" "$first" "$failed" "$correct" "$advisory" "$choice" "$success" >> "$RESULTS"
    done

    for i in $(seq -w 1 24); do
        goal="Recover novelcase${i} after checksum mismatch"
        alpha="novel_${i}_alpha"
        beta="novel_${i}_beta"
        if [ $((10#$i % 2)) -eq 0 ]; then correct="$alpha"; failed="$beta"; else correct="$beta"; failed="$alpha"; fi
        if [ $((10#$i % 4)) -lt 2 ]; then first="$alpha"; else first="$beta"; fi
        if [ "$first" = "$alpha" ]; then second="$beta"; else second="$alpha"; fi
        output=$("$A" learning approaches "$goal" 2>&1)
        printf '%s\n' "$output" > "$OUTPUT_DIR/advisories/${arm}-novel-${i}.txt"
        advisory=0
        printf '%s' "$output" | grep -Fq 'execute via' && advisory=1
        if printf '%s' "$output" | grep -Fq "execute via $first"; then choice="$second"; else choice="$first"; fi
        if [ "$choice" = "$correct" ]; then success=1; else success=0; fi
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$arm" novel "$i" "$first" "$failed" "$correct" "$advisory" "$choice" "$success" >> "$RESULTS"
    done
}

section "control: failures occur, self-learning is withheld"
reset_learning_state
seed_setup_failures
run_consumers control
check "control exposes no recalled failures" "0" \
    "$(awk -F, 'NR>1 && $1==\"control\" {s+=$7} END {print s+0}' "$RESULTS")"

section "treatment: the same failures become negative knowledge"
reset_learning_state
seed_setup_failures
scan_output=$("$A" eval candidates-update scan --suite efficacy-study 2>&1)
printf '%s\n' "$scan_output" > "$OUTPUT_DIR/treatment-scan.txt"
check "treatment records one failed approach per repeated task" "24" \
    "$(q 'SELECT count(*) FROM approach_failures')"
check "treatment retains both observations" "48" \
    "$(q 'SELECT coalesce(sum(occurrences),0) FROM approach_failures')"
run_consumers treatment
check "treatment exposes one recalled failure per repeated task" "24" \
    "$(awk -F, 'NR>1 && $1==\"treatment\" && $2==\"repeat\" {s+=$7} END {print s+0}' "$RESULTS")"
check "treatment exposes no failure on novel tasks" "0" \
    "$(awk -F, 'NR>1 && $1==\"treatment\" && $2==\"novel\" {s+=$7} END {print s+0}' "$RESULTS")"

python3 - "$RESULTS" "$OUTPUT_DIR/summary.json" <<'PY'
import csv
import json
import math
import os
import platform
import sys
from datetime import datetime, timezone

rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
by_key = {(r["arm"], r["task_class"], r["task_id"]): r for r in rows}

def score(arm, task_class):
    cells = [r for r in rows if r["arm"] == arm and r["task_class"] == task_class]
    return {"passed": sum(int(r["success"]) for r in cells), "total": len(cells)}

repeat_ids = sorted({r["task_id"] for r in rows if r["task_class"] == "repeat"})
b = c = 0
for task_id in repeat_ids:
    control = int(by_key[("control", "repeat", task_id)]["success"])
    treatment = int(by_key[("treatment", "repeat", task_id)]["success"])
    b += treatment == 1 and control == 0
    c += treatment == 0 and control == 1
n = b + c
p_exact = 1.0 if n == 0 else min(1.0, 2.0 * sum(math.comb(n, k) for k in range(min(b, c) + 1)) / (2**n))

summary = {
    "schema": "aimee-self-learning-efficacy-v1",
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "commit": os.environ.get("STUDY_COMMIT", "unknown"),
    "host": platform.node(),
    "conditions": {
        "control": "same failed jobs; synthesis withheld before consumer phase",
        "treatment": "same failed jobs; production synthesis and approach recall enabled",
    },
    "scores": {
        "repeat_control": score("control", "repeat"),
        "repeat_treatment": score("treatment", "repeat"),
        "novel_control": score("control", "novel"),
        "novel_treatment": score("treatment", "novel"),
    },
    "paired_repeat": {
        "treatment_only_success": int(b),
        "control_only_success": int(c),
        "mcnemar_exact_two_sided_p": p_exact,
    },
    "planner": "seeded first choice; switch only when production recall reports that choice failed",
}
with open(sys.argv[2], "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2, sort_keys=True)
    f.write("\n")
print(json.dumps(summary, indent=2, sort_keys=True))
PY

cp "$WORKDIR/kb.log" "$WORKDIR/server.log" "$OUTPUT_DIR/"
for log in "$WORKDIR"/mod-*.log; do [ -f "$log" ] && cp "$log" "$OUTPUT_DIR/"; done
printf '%s\n' "$PASS" > "$OUTPUT_DIR/harness-passed.txt"
printf '%s\n' "$FAIL" > "$OUTPUT_DIR/harness-failed.txt"
printf '\n%d harness checks passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
