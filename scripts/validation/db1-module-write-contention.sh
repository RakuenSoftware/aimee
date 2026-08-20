#!/bin/sh
# Two writers on one file, under load.
#
# db1-module-wfe-coexistence.sh establishes that the shipped topology has two
# processes holding aimee.db: the DB1 module and the Go WFE. That is a doctrine
# problem on its own. Whether it is also an operational problem depends on
# something the coexistence script does not test -- what happens when both write
# at once.
#
# Both sides are configured for it deliberately, which is worth stating because
# it changes what this test is looking for:
#
#   module (db.c):        journal_mode=WAL, busy_timeout 5000/15000ms
#   Go (internal/db1):    journal_mode=WAL, busy_timeout 5000ms,
#                         MaxOpenConns(1), _txlock=immediate
#
# WAL supports multiple processes, so the expected result is that this passes.
# It is worth running anyway: "we believe WAL handles it" and "we watched it
# handle it at this rate" are different claims, and only the second one is
# evidence. A busy_timeout does not make contention free -- it makes it slow,
# and a write that takes longer than the timeout still fails.
#
# The second writer here is a sqlite3 process holding an IMMEDIATE transaction
# over lifecycle_work_item in a loop. It stands in for a competing writer rather
# than reproducing one: after the engine moved behind the module there is no
# second writer in production, which is the point of the port -- so this is a
# deliberately adversarial condition, and what it exercises is the module's own
# BEGIN IMMEDIATE retry. Without that retry a caller would see an operation
# "refused" the first time a competing writer held the lock through the
# connection's busy timeout.
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-db1}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/db1.grant}
SERVER=${AIMEE_SERVER_BIN:-/usr/local/bin/aimee-server}
WRITERS=${WRITERS:-4}
ROUNDS=${ROUNDS:-150}
CREATES=${CREATES:-150}

PASS=0
FAIL=0
say_pass() {
   echo "PASS  $1"
   PASS=$((PASS + 1))
}
say_fail() {
   echo "FAIL  $1"
   FAIL=$((FAIL + 1))
}

HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-contend-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="contend$$"
mkdir -p "$AIMEE_HOME/modules.d/server"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
cp "$GRANT" "$AIMEE_HOME/modules.d/server/db1.grant"

state() {
   curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health |
      sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'
}

"$SERVER" --foreground >"$HOME/server.log" 2>&1 &
SPID=$!
i=0
while [ $i -lt 300 ]; do
   [ -S "$HTTP_SOCK" ] && break
   sleep 0.1
   i=$((i + 1))
done
AIMEE_DB1_PATH="$DB" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 150 ]; do
   [ "$(state)" = "ok" ] && break
   sleep 0.2
   i=$((i + 1))
done
[ "$(state)" = "ok" ] && say_pass "daemon and module up" || {
   say_fail "topology did not come up"
   exit 1
}

echo "      $WRITERS external writers x $ROUNDS rows, against $CREATES module writes"

# The external writers, started first so they are already contending when the
# module's writes begin.
WRITER_PIDS=""
w=1
while [ $w -le "$WRITERS" ]; do
   (
      # lifecycle_work_item carries UNIQUE(repo, proposal_path) as well as a
      # unique work_item_id, so every row needs all three distinct or the
      # insert fails on the constraint and measures nothing. The first version
      # of this reused one repo/path pair and "failed" 599 of 600 writes with
      # no lock involved at all.
      err=0
      r=1
      while [ $r -le "$ROUNDS" ]; do
         # An explicit IMMEDIATE transaction that holds the write lock for a
         # moment, rather than a bare INSERT. A bare INSERT takes and releases
         # the lock so fast that the module rarely collides with it, and a
         # contention test that never contends proves nothing. Holding it is
         # what forces the module's own BEGIN IMMEDIATE to wait and retry.
         sqlite3 "$DB" \
            "PRAGMA busy_timeout=5000;
             BEGIN IMMEDIATE;
             INSERT INTO lifecycle_work_item
               (work_item_id, repo, proposal_path, workflow_name, workflow_version,
                current_stage, state, mode)
             VALUES ('w$w-$r','repo-$w','path-$w-$r','wf','1','s','queued','m');
             SELECT COUNT(*) FROM lifecycle_work_item;
             COMMIT;" \
            >/dev/null 2>>"$HOME/writer.$w.err" || err=$((err + 1))
         r=$((r + 1))
      done
      echo "$err" >"$HOME/writer.$w.failures"
   ) &
   # Collected so the wait below names them. A bare `wait` also waits on the
   # daemon and the module, which were started with & in this same shell and
   # never exit -- the first version of this hung there forever with all its
   # measurements already taken and none of them printed.
   WRITER_PIDS="$WRITER_PIDS $!"
   w=$((w + 1))
done

# The module's writes, through the daemon, while the above is running.
CREATE_ERR=0
n=1
while [ $n -le "$CREATES" ]; do
   r=$(curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
      -d "{\"title\":\"contend $n\"}" http://localhost/v1/sessions/create)
   case "$r" in
   *'"session_id"'*) ;;
   *) CREATE_ERR=$((CREATE_ERR + 1)) ;;
   esac
   n=$((n + 1))
done
# shellcheck disable=SC2086
wait $WRITER_PIDS

WRITER_ERR=0
w=1
while [ $w -le "$WRITERS" ]; do
   f=$(cat "$HOME/writer.$w.failures" 2>/dev/null || echo 0)
   WRITER_ERR=$((WRITER_ERR + f))
   w=$((w + 1))
done

echo
echo "=== results ==="
echo "      module writes attempted: $CREATES, failed: $CREATE_ERR"
echo "      external writes attempted: $((WRITERS * ROUNDS)), failed: $WRITER_ERR"

# A refused write is not a lost write. Four external writers hammering the same
# SQLite file will make some module writes exhaust their busy-retry budget and be
# REFUSED -- reported to the caller, never silently dropped -- and that is the
# database behaving as designed, not the module failing. Measured interleaved
# against the pre-migration build on an idle host: base 19 refusals across four
# runs, this build 22. Indistinguishable, and the base refuses too, so a
# zero-tolerance assertion here fails for both builds on a busy machine and says
# nothing about either.
#
# What must hold is the pair below: every write that was ACCEPTED landed, and
# integrity is clean afterwards. Those are asserted exactly. This one bounds the
# refusal rate instead, loosely enough not to flap and tightly enough that a real
# regression -- a module that starts refusing most of its writes -- still trips
# it.
CREATE_ERR_MAX=$((CREATES / 10))
[ "$CREATE_ERR" -le "$CREATE_ERR_MAX" ] &&
   say_pass "module writes survived the contention ($CREATE_ERR refused, bound $CREATE_ERR_MAX)" ||
   say_fail "$CREATE_ERR of $CREATES module writes refused, over the $CREATE_ERR_MAX bound"
# The same bound, for the same reason, on the control side. These writers hold an
# IMMEDIATE transaction with busy_timeout=5000 and still occasionally exhaust it
# under the module's concurrent writes -- observed on the pre-migration build too.
# A refused external write is likewise reported and not lost, which the exact
# row-count check below proves.
WRITER_TOTAL=$((WRITERS * ROUNDS))
WRITER_ERR_MAX=$((WRITER_TOTAL / 10))
[ "$WRITER_ERR" -le "$WRITER_ERR_MAX" ] &&
   say_pass "external writes survived the contention ($WRITER_ERR refused, bound $WRITER_ERR_MAX)" ||
   say_fail "$WRITER_ERR of $WRITER_TOTAL external writes refused, over the $WRITER_ERR_MAX bound"

SESS=$(sqlite3 "$DB" "select count(*) from server_sessions" 2>/dev/null)
ITEMS=$(sqlite3 "$DB" "select count(*) from lifecycle_work_item" 2>/dev/null)
echo "      rows landed: server_sessions=$SESS lifecycle_work_item=$ITEMS"
[ "${SESS:-0}" -eq $((CREATES - CREATE_ERR)) ] &&
   say_pass "no module write was lost" ||
   say_fail "module rows do not match successful writes ($SESS vs $((CREATES - CREATE_ERR)))"
[ "${ITEMS:-0}" -eq $((WRITERS * ROUNDS - WRITER_ERR)) ] &&
   say_pass "no external write was lost" ||
   say_fail "external rows do not match ($ITEMS vs $((WRITERS * ROUNDS - WRITER_ERR)))"

# Integrity is the thing that would make this a corruption story rather than a
# contention story, so ask SQLite directly.
INTEG=$(sqlite3 "$DB" "PRAGMA integrity_check;" 2>&1 | head -1)
[ "$INTEG" = "ok" ] && say_pass "integrity_check clean after concurrent writers" ||
   say_fail "integrity_check: $INTEG"

LOCKED=$(cat "$HOME"/writer.*.err 2>/dev/null | grep -ci "locked\|busy" || echo 0)
echo "      lock/busy complaints from external writers: $LOCKED"
[ "$(state)" = "ok" ] && say_pass "store still healthy afterwards" ||
   say_fail "store unhealthy afterwards: $(state)"

echo
echo "=============================================="
echo " $PASS passed, $FAIL failed"
echo " home: $HOME"
echo "=============================================="
kill "$MPID" "$SPID" 2>/dev/null
[ "$FAIL" -eq 0 ]
