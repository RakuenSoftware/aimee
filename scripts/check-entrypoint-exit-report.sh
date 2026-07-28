#!/bin/sh
# The container's final log line is the only post-mortem an operator gets. Two
# defects made a routine `docker stop` read as a crash: the plane name was
# hardcoded to aimee-server (so a Go WFE exit sent you into the wrong process),
# and runuser turns a caught SIGTERM into a bare exit 1 with the signal
# discarded (so "exited (status 1)" looked like a failure and implied a core
# dump that exit(1) never writes).
#
# plane_exit_message is pure — args in, string out — so both are testable here
# without building or running a container.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ENTRYPOINT="$ROOT/deploy/container/server-entrypoint.sh"

[ -r "$ENTRYPOINT" ] || {
    echo "check-entrypoint-exit-report: cannot read $ENTRYPOINT" >&2
    exit 1
}

# Source only the function under test. The script proper bootstraps PAM users
# and launches daemons, so it cannot simply be sourced.
eval "$(sed -n '/^plane_exit_message() {$/,/^}$/p' "$ENTRYPOINT")"

command -v plane_exit_message >/dev/null 2>&1 || {
    echo "check-entrypoint-exit-report: plane_exit_message not found in entrypoint" >&2
    echo "  (was it renamed? this guard exists so the check cannot silently pass)" >&2
    exit 1
}

fail=0
expect() {
    _desc=$1
    _got=$2
    _want=$3
    if [ "$_got" = "$_want" ]; then
        echo "  ok: $_desc"
    else
        echo "  FAIL: $_desc" >&2
        echo "    want: $_want" >&2
        echo "    got:  $_got" >&2
        fail=1
    fi
}

echo "check-entrypoint-exit-report:"

# A signalled stop must not read as a failure. The status carries the signal
# (wait reports 128+N), so the wording is derived from it rather than from a
# separate flag that could contradict it.
expect "SIGTERM'd server names the signal, not a bare failure" \
    "$(plane_exit_message server 143)" \
    "aimee-server stopped on signal 15 (status 143); shutting down webchat"

expect "SIGKILL/OOM is distinguishable from SIGTERM" \
    "$(plane_exit_message server 137)" \
    "aimee-server stopped on signal 9 (status 137); shutting down webchat"

# ...and a genuine failure still reads as one.
expect "plain exit 1 is reported as a failure" \
    "$(plane_exit_message server 1)" \
    "aimee-server exited (status 1); shutting down webchat"

expect "plain exit 3 is reported with its own code" \
    "$(plane_exit_message server 3)" \
    "aimee-server exited (status 3); shutting down webchat"

# The three below all used to collapse to "exited (status 1)". Prove they differ.
if [ "$(plane_exit_message server 143)" = "$(plane_exit_message server 1)" ] ||
   [ "$(plane_exit_message server 137)" = "$(plane_exit_message server 143)" ]; then
    echo "  FAIL: signalled and failed exits are indistinguishable" >&2
    fail=1
else
    echo "  ok: SIGTERM, SIGKILL and a plain failure all read differently"
fi

# The WFE plane must be named as itself — no status can convey this, which is
# why the naming fix is separate from the status fix.
expect "wfe exit names aimee-wfe" \
    "$(plane_exit_message wfe 3)" \
    "aimee-wfe exited (status 3); shutting down webchat"

expect "wfe signal names aimee-wfe" \
    "$(plane_exit_message wfe 143)" \
    "aimee-wfe stopped on signal 15 (status 143); shutting down webchat"

# Single-plane path leaves `first` empty; it must still name the server.
expect "empty first defaults to aimee-server" \
    "$(plane_exit_message '' 137)" \
    "aimee-server stopped on signal 9 (status 137); shutting down webchat"

if [ "$fail" -ne 0 ]; then
    echo "check-entrypoint-exit-report: FAILED" >&2
    exit 1
fi
echo "check-entrypoint-exit-report: ok"
