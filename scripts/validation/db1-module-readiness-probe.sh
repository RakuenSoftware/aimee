#!/bin/sh
# Does the daemon keep reporting the store healthy after the module dies?
#
# The e2e run said it did for at least ten seconds, which would mean readiness
# is latched rather than live -- the exact failure the readiness fix existed to
# prevent. This isolates it: confirm the module process is really gone, then
# poll health with timestamps and watch what the daemon says.
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
# Overridable so this can run against a build tree as well as an install.
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-aimee}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/aimee.grant}
HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-probe-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
mkdir -p "$AIMEE_HOME/modules.d/server"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
# The grant's executable= is what the daemon pins the peer against, so it must
# name the module this rig actually starts. In a container the two are the same
# path and a plain copy works; against a build tree they are not, and a grant
# naming an uninstalled path makes the daemon reject the whole policy and exit.
sed "s|^executable=.*|executable=$MODULE|" "$GRANT" \
    >"$AIMEE_HOME/modules.d/server/aimee.grant"

health() { curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health; }
state() { health | sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'; }

aimee-server --foreground >"$HOME/server.log" 2>&1 &
SPID=$!
i=0; while [ $i -lt 300 ]; do [ -S "$HTTP_SOCK" ] && break; sleep 0.1; i=$((i+1)); done
echo "server pid $SPID, state with no module: $(state)"
echo "bus socket present before the module starts: $([ -S "$BUS_SOCK" ] && echo yes || echo no)"

require_store_url
AIMEE_STORE_URL="$AIMEE_STORE_URL" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
MPID=$!
i=0; while [ $i -lt 100 ]; do [ "$(state)" = "ok" ] && break; sleep 0.2; i=$((i+1)); done
echo "module pid $MPID, state once attached: $(state)"

# Prove a store call really is going through the module right now.
CREATED=$(curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
   -d '{"title":"probe"}' http://localhost/v1/sessions/create)
echo "create while attached: $CREATED"

echo
echo "--- killing the module (SIGTERM) ---"
kill "$MPID" 2>/dev/null
i=0
while [ $i -lt 50 ]; do
   kill -0 "$MPID" 2>/dev/null || break
   sleep 0.1
   i=$((i+1))
done
if kill -0 "$MPID" 2>/dev/null; then
   echo "module IGNORED SIGTERM and is still running -- that alone explains a healthy report"
else
   echo "module process is gone after $(echo "$i" | awk '{print $1/10}')s"
fi
# And nothing else is serving in its place.
echo "processes still matching the module binary: $(pgrep -c -f aimee-module-aimee 2>/dev/null || echo 0)"

echo
echo "--- health after the module is gone ---"
i=0
while [ $i -lt 140 ]; do
   ST=$(state)
   case $((i % 8)) in 0) printf '  t+%ss  state=%s\n' "$(echo "$i" | awk '{print $1*0.5}')" "$ST" ;; esac
   if [ "$ST" != "ok" ]; then
      printf '  FLIPPED to "%s" at t+%ss\n' "$ST" "$(echo "$i" | awk '{print $1*0.5}')"
      break
   fi
   sleep 0.5
   i=$((i+1))
done

echo
echo "--- does a store call still work with the module gone? ---"
curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
   -d '{"title":"after death"}' http://localhost/v1/sessions/create
echo
echo "final state: $(state)"
kill "$SPID" 2>/dev/null
echo "home: $HOME"
