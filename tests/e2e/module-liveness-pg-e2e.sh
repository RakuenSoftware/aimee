#!/bin/bash
# module-liveness-pg-e2e.sh — every module a daemon is GRANTED actually attaches,
# and no provider its code paths need is left null.
#
# Why this exists: signal capture is served by aimee-kb, and the router it calls
# needs a signal classifier to decide which sinks a signal reaches. Only
# aimee-server ever registered one. In the KB the pointer was null, every signal
# was refused with a single WARN, and the route answered 200 carrying an error
# document while writing nothing. Signal ingest through the KB had never worked.
#
# No unit test could have caught it: every test registers its own provider, so
# none can observe that production does not. `make lint` now gates the shape
# (check_provider_registration.py), but a gate reads source -- it cannot tell you
# that a deployed daemon came up with its modules attached and its paths live.
# That is what this suite is for.
#
# It also exists because the harness that FOUND that bug started two modules.
# A module which is granted but never attached fails exactly like a module that
# was never placed, so a thin environment hides precisely this class of defect.
# This one attaches every module each daemon is granted and has a binary for,
# and says out loud which it could not start rather than skipping it quietly.
#
# Usage:  ./module-liveness-pg-e2e.sh
#   AIMEE_ROOT     dir holding aimee-server / aimee-kb / aimee   (default /root/aimee)
#   AIMEE_SRC      the source tree, for build/obj                (default $AIMEE_ROOT/src)
#   WORKDIR        scratch for HOMEs and logs                    (default /tmp/module-liveness)
#   AIMEE_DB2_URL  libpq URL for a throwaway database
#   KB_PORT        TCP port for aimee-kb                         (default 18744)
#
# Every assertion prints PASS or FAIL; the script exits non-zero if any failed.
# Use a throwaway host: it writes rows and starts real services.
set -uo pipefail

AIMEE_ROOT="${AIMEE_ROOT:-/root/aimee}"
AIMEE_SRC="${AIMEE_SRC:-$AIMEE_ROOT/src}"
WORKDIR="${WORKDIR:-/tmp/module-liveness}"
KB_PORT="${KB_PORT:-18744}"
export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgres:///aimee_shared?host=/var/run/postgresql}"
OBJ="$AIMEE_SRC/build/obj"
KBHOME="$WORKDIR/kbhome"
SRVHOME="$WORKDIR/srvhome"

PASS=0; FAIL=0
check() { # check <name> <expected> <actual>
  if [ "$2" = "$3" ]; then printf '  PASS  %s\n' "$1"; PASS=$((PASS+1))
  else printf '  FAIL  %s\n        expected: %s\n        actual:   %s\n' "$1" "$2" "$3"; FAIL=$((FAIL+1)); fi
}
ok()   { printf '  PASS  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL+1)); }
section() { printf '\n=== %s\n' "$1"; }

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

# Deploy one module: its grant, and a binary to serve it. The Go modules share a
# single host binary and take their identity from argv[0], so a module is
# deployed by copying that host under its own name.
deploy() { # deploy <placement> <name> <home>
    local placement="$1" name="$2" home="$3"
    local grant="$OBJ/module-bundle/grants/$placement/$name.grant"
    [ -r "$grant" ] || return 1
    local bin="$OBJ/aimee-module-$name"
    [ -x "$bin" ] || bin="$OBJ/aimee-module"
    [ -x "$bin" ] || { bad "granted '$name' has no binary to serve it"; return 1; }
    cp "$bin" "$home/.config/aimee/aimee-module-$name"
    chmod 0755 "$home/.config/aimee/aimee-module-$name"
    sed "s|^executable=.*|executable=$home/.config/aimee/aimee-module-$name|" "$grant" \
        > "$home/.config/aimee/modules.d/$placement/$name.grant"
    return 0
}

attach() { # attach <name> <home> <bus> <tag>
    local name="$1" home="$2" bus="$3" tag="$4"
    [ -x "$home/.config/aimee/aimee-module-$name" ] || return 1
    env HOME="$home" AIMEE_HOME="$home/.config/aimee" \
        AIMEE_DB1_PATH="$home/.config/aimee/aimee.db" AIMEE_DB2_URL="$AIMEE_DB2_URL" \
        "$home/.config/aimee/aimee-module-$name" "$bus" > "$WORKDIR/mod-$tag-$name.log" 2>&1 &
    MOD_PIDS="$MOD_PIDS $!"
    return 0
}

section "0  aimee-kb, with every module it is granted"
# db2 is granted but deliberately not started in the KB image, so leaving it out
# mirrors production rather than forgetting it.
KB_MODULES="config learning memory postgres kb-synthesis control-web benchmarks"
KB_DEPLOYED=""
for m in $KB_MODULES; do
    if deploy kb "$m" "$KBHOME"; then KB_DEPLOYED="$KB_DEPLOYED $m"
    else bad "no kb grant for module '$m'"; fi
done
printf '        deployed:%s\n' "$KB_DEPLOYED"

KBBUS="$KBHOME/.config/aimee/kb-module-bus.sock"
env HOME="$KBHOME" AIMEE_HOME="$KBHOME/.config/aimee" \
    "$AIMEE_ROOT/aimee-kb" --http-port="$KB_PORT" > "$WORKDIR/kb.log" 2>&1 &
KB_PID=$!
for _ in $(seq 1 300); do [ -S "$KBBUS" ] && break; sleep 0.1; done
[ -S "$KBBUS" ] && ok "the KB opened its module bus" || bad "the KB never opened its module bus"
for m in $KB_DEPLOYED; do attach "$m" "$KBHOME" "$KBBUS" kb; done

KB_URL="http://127.0.0.1:$KB_PORT"
kb_up=0
for _ in $(seq 1 180); do
    curl -fsS --max-time 3 "$KB_URL/v1/health" >/dev/null 2>&1 && { kb_up=1; break; }
    sleep 1
done
if [ "$kb_up" = 1 ]; then ok "aimee-kb is serving on $KB_URL"
else bad "aimee-kb never answered"; tail -15 "$WORKDIR/kb.log"; fi

# A module that exits on attach is a rejected grant, and the daemon carries on
# without it in silence. That silence is the whole subject of this suite.
alive=0; dead=0
for p in $MOD_PIDS; do
    if kill -0 "$p" 2>/dev/null; then alive=$((alive+1)); else dead=$((dead+1)); fi
done
check "every KB module is still attached" "0" "$dead"
printf '        %d attached\n' "$alive"

section "1  aimee-server, with every module it is granted"
SRV_MODULES="config db1 learning memory routing delegates tools workspace git skills
             response-composition governance roundtable runtime-web sandbox economizer benchmarks"
SRV_DEPLOYED=""
for m in $SRV_MODULES; do
    deploy server "$m" "$SRVHOME" && SRV_DEPLOYED="$SRV_DEPLOYED $m"
done
printf '        deployed:%s\n' "$SRV_DEPLOYED"

export AIMEE_KB_API_URL="$KB_URL"
SRVBUS="$SRVHOME/.config/aimee/server-module-bus.sock"
SRVSOCK="$SRVHOME/.config/aimee/aimee-http.sock"
env HOME="$SRVHOME" AIMEE_HOME="$SRVHOME/.config/aimee" AIMEE_KB_API_URL="$KB_URL" \
    "$AIMEE_ROOT/aimee-server" --foreground > "$WORKDIR/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 300); do [ -S "$SRVBUS" ] && break; sleep 0.1; done
for m in $SRV_DEPLOYED; do attach "$m" "$SRVHOME" "$SRVBUS" srv; done
for _ in $(seq 1 300); do [ -S "$SRVSOCK" ] && break; sleep 0.2; done
if [ -S "$SRVSOCK" ]; then ok "aimee-server is serving on its socket"
else bad "aimee-server never came up"; tail -15 "$WORKDIR/server.log"; fi

export HOME="$SRVHOME" AIMEE_HOME="$SRVHOME/.config/aimee"
export AIMEE_API_ENDPOINT="unix:$SRVSOCK"
A="$AIMEE_ROOT/aimee"

section "2  the surfaces answer"
# Breadth is the point: the classifier gap was invisible because nothing drove
# the route that needed it. Drive many, and let section 4 judge the logs.
run() { # run <label> <argv...>
    local label="$1"; shift
    local out
    out=$("$@" 2>&1 | head -1)
    printf '        %-26s %s\n' "$label" "$(printf '%s' "$out" | cut -c1-72)"
    case "$out" in
        *"unknown command"*|*"is not a subcommand"*|*"no command catalogue"*)
            bad "the suite drove a command that does not exist: $label";;
    esac
}
run "eval candidates"      "$A" eval candidates --limit 1
run "learning approaches"  "$A" learning approaches "rebuild the search index"
run "learning attribution" "$A" learning attribution --suite regression
run "learning resolve"     "$A" learning resolve --budget 3
run "status"               "$A" status
run "memory recall"        curl -sS --max-time 10 -X POST -H 'Content-Type: application/json' \
                                -d '{"task_hint":"index rebuild","limit_tokens":200}' \
                                "$KB_URL/v1/actions/memory.recall"
run "endogeneity"          curl -sS --max-time 10 -X POST -H 'Content-Type: application/json' \
                                -d '{}' "$KB_URL/v1/actions/learning.endogeneity"
run "policy select"        curl -sS --max-time 10 -X POST -H 'Content-Type: application/json' \
                                -d '{}' "$KB_URL/v1/actions/learning.policy_select"

section "3  signal capture, the path that was dead"
# Count first. A bare "is there a superseded row?" passes on a row left by an
# earlier run, which is how this assertion once reported PASS in a run where
# every capture was refused and nothing was written at all.
BEFORE=$(psql -d "${PGDB:-aimee_shared}" -tA \
         -c "SELECT count(*) FROM learning_proposal_fate WHERE fate='superseded'" 2>/dev/null)
BEFORE="${BEFORE:-0}"
for i in 1 2; do
    R=$(curl -sS -m 15 -X POST -H 'Content-Type: application/json' \
        -d "{\"signal_type\":\"mark_rule\",\"source\":\"explicit\",\"polarity\":\"positive\",\"title\":\"t$i\",\"description\":\"rule number $i\",\"target_key\":\"liveness-target\",\"high_confidence\":true}" \
        "$KB_URL/v1/actions/learning.propose_signal")
    printf '        capture %d: %s\n' "$i" "$(printf '%s' "$R" | head -c 120)"
    if printf '%s' "$R" | grep -q '"status":"ok"'; then ok "signal $i was recorded"
    else bad "signal $i was refused -- the classifier is null again"; fi
done
# Read the effect back through psql, not from the process that wrote it, and
# judge the DELTA this run produced rather than the state it inherited.
AFTER=$(psql -d "${PGDB:-aimee_shared}" -tA \
        -c "SELECT count(*) FROM learning_proposal_fate WHERE fate='superseded'" 2>/dev/null)
AFTER="${AFTER:-0}"
if [ "$AFTER" -gt "$BEFORE" ]; then ok "the router recorded a supersession unasked"
else bad "no supersession was recorded by this run (was $BEFORE, now $AFTER)"; fi

section "4  nothing reports a missing provider or stage"
# The generalisation of the defect that started this. A provider left null, a
# stage nobody serves, and a grant the daemon rejected all surface as one of
# these. Any hit is a live gap, whether or not it was predicted.
PATTERNS='classification unavailable|provider (is )?(not registered|unavailable|missing)|no provider|not registered|stage [^ ]* unserved|call .*=[ ]*TRANSPORT|verdict=TRANSPORT|module call failed|grant (rejected|refused|invalid)'
# Postgres NOTICE lines carry column names (transport_cn, transport_identity)
# that are not verdicts. Schema chatter is not a defect.
EXCLUDE='^NOTICE:|NOTICE:  column'
for log in "$WORKDIR/kb.log" "$WORKDIR/server.log"; do
    n=$(grep -iE "$PATTERNS" "$log" 2>/dev/null | grep -vcE "$EXCLUDE")
    if [ "${n:-0}" -gt 0 ]; then
        bad "$(basename "$log"): $n line(s) report a missing provider or stage"
        grep -iE "$PATTERNS" "$log" | grep -vE "$EXCLUDE" | sort -u | head -8 | sed 's/^/          /'
    else
        ok "$(basename "$log"): nothing reports a missing provider or stage"
    fi
done

section "5  the service's own verdict, and no crashes"
H=$(curl -sS --max-time 10 "$KB_URL/v1/health")
if printf '%s' "$H" | grep -q 'store unavailable'; then
    bad "the KB calls its store unavailable while storing and retrieving through it"
    printf '        (is AIMEE_DB2_URL reaching the postgres module?)\n'
else
    ok "the KB does not contradict itself about its own store"
fi
# An environment with no embedder is HONESTLY degraded, so assert on the finding
# rather than on the summary. Any blocker beyond that one is a real result.
OTHER=$(printf '%s' "$H" | python3 -c 'import json,sys
d=json.load(sys.stdin)
print(sum(1 for b in d.get("blockers",[]) if "embedder" not in b))' 2>/dev/null)
check "no blocker beyond the absent embedder" "0" "${OTHER:-unknown}"

for log in "$WORKDIR"/kb.log "$WORKDIR"/server.log "$WORKDIR"/mod-*.log; do
    [ -r "$log" ] || continue
    if grep -qiE 'segmentation fault|AddressSanitizer|Assertion .* failed|panic:' "$log"; then
        bad "$(basename "$log") records a crash"
        grep -iE 'segmentation fault|AddressSanitizer|Assertion .* failed|panic:' "$log" \
            | head -3 | sed 's/^/          /'
    fi
done
kill -0 "$KB_PID" 2>/dev/null && ok "aimee-kb survived" || bad "aimee-kb died"
kill -0 "$SRV_PID" 2>/dev/null && ok "aimee-server survived" || bad "aimee-server died"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
