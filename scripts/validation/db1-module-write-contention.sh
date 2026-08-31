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
# It is worth running anyway: "we believe it handles it" and "we watched it
# handle it at this rate" are different claims, and only the second one is
# evidence.
#
# This used to drive four sqlite3 processes holding an IMMEDIATE transaction on
# the store file, standing in for a competing writer, to force the module's own
# BEGIN IMMEDIATE retry. Neither half survives the move to PostgreSQL: there is
# no file for an outside process to open, and there is no whole-database write
# lock to contend for -- concurrent inserts of distinct rows do not block each
# other at all, which is a good part of why the store moved.
#
# So the writers are now module clients, which is the only concurrency a
# deployment can have, and the tolerance for refused writes is zero rather than
# the tenth SQLite needed.
#
# The store module is PostgreSQL-backed: it reads AIMEE_STORE_URL and refuses to
# start without it. Say so here rather than letting the module exit into a log
# nobody reads and the rig time out on a socket that never appears.
require_store_url() {
   if [ -z "${AIMEE_STORE_URL:-}" ]; then
      echo "$(basename "$0"): AIMEE_STORE_URL is not set." >&2
      echo "  The store is a Go module against PostgreSQL; it no longer opens a" >&2
      echo "  SQLite file. Point this at a database the rig may create and drop:" >&2
      echo "    export AIMEE_STORE_URL=postgres://user:pass@host:5432/aimee_store" >&2
      exit 2
   fi
}

PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-aimee}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/aimee.grant}
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
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
# The grant's executable= is what the daemon pins the peer against, so it must
# name the module this rig actually starts. In a container the two are the same
# path and a plain copy works; against a build tree they are not, and a grant
# naming an uninstalled path makes the daemon reject the whole policy and exit.
sed "s|^executable=.*|executable=$MODULE|" "$GRANT" \
    >"$AIMEE_HOME/modules.d/server/aimee.grant"

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
require_store_url
AIMEE_STORE_URL="$AIMEE_STORE_URL" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
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

# Concurrent writers, all through the module -- the only way into the store, so
# the only concurrency a deployment can actually have. They start first so they
# are already running when the serial writes below begin.
WRITER_PIDS=""
w=1
while [ $w -le "$WRITERS" ]; do
   (
      err=0
      r=1
      while [ $r -le "$ROUNDS" ]; do
         out=$(curl -s --unix-socket "$HTTP_SOCK" -X POST \
            -H 'Content-Type: application/json' \
            -d "{\"title\":\"writer $w round $r\"}" \
            http://localhost/v1/sessions/create 2>>"$HOME/writer.$w.err")
         case "$out" in
         *'"session_id"'*) ;;
         *) err=$((err + 1)); printf '%s\n' "$out" >>"$HOME/writer.$w.err" ;;
         esac
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

# More writes, serially, while the above is running.
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

WRITER_TOTAL=$((WRITERS * ROUNDS))
TOTAL=$((WRITER_TOTAL + CREATES))
ERRORS=$((WRITER_ERR + CREATE_ERR))

echo
echo "=== results ==="
echo "      serial writes attempted: $CREATES, failed: $CREATE_ERR"
echo "      concurrent writes attempted: $WRITER_TOTAL, failed: $WRITER_ERR"

# Zero, not a tolerance. The SQLite version of this rig bounded refusals at a
# tenth, because four processes holding an IMMEDIATE transaction on one file
# really did exhaust each other's busy-retry budget -- measured at 19 and 22
# refusals across four runs. PostgreSQL does not have a whole-database write
# lock: concurrent inserts of distinct rows do not contend, so a refusal here is
# a defect rather than the database behaving as designed.
[ "$ERRORS" -eq 0 ] &&
   say_pass "no write was refused under $WRITERS-way concurrency ($TOTAL writes)" ||
   say_fail "$ERRORS of $TOTAL writes were refused"

# And every write that was accepted is readable back. Asked through /v1, because
# nothing outside the module opens the store -- reaching around it would be
# testing something other than what a caller gets.
LISTED=$(curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
         -d '{}' http://localhost/v1/sessions/list | grep -o '"client_type"' | wc -l)
# The list answers one page -- 100 rows -- so with hundreds of writes accepted
# this cannot count them all. What it does prove is that reads still work at
# this volume and come back full rather than short, which is what a lost or
# half-committed write would break.
PAGE=100
EXPECT=$((TOTAL - ERRORS)); [ "$EXPECT" -gt "$PAGE" ] && EXPECT=$PAGE
echo "      sessions readable afterwards: ${LISTED:-0} (one page is $PAGE)"
[ "${LISTED:-0}" -ge "$EXPECT" ] &&
   say_pass "reads come back full after $((TOTAL - ERRORS)) accepted writes" ||
   say_fail "$LISTED sessions readable, expected at least $EXPECT"

[ "$(state)" = "ok" ] && say_pass "store still healthy afterwards" ||
   say_fail "store unhealthy afterwards: $(state)"

echo
echo "=============================================="
echo " $PASS passed, $FAIL failed"
echo " home: $HOME"
echo "=============================================="
kill "$MPID" "$SPID" 2>/dev/null
[ "$FAIL" -eq 0 ]
