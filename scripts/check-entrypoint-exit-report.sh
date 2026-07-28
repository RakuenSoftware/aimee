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

# A signalled stop must not be described as a failure.
expect "SIGTERM'd server is reported as a termination, not a failure" \
    "$(plane_exit_message server 1 1)" \
    "aimee-server stopped on termination signal (status 1); shutting down webchat"

# ...and a real failure must still read as one, same status code.
expect "unsignalled exit 1 is still reported as a failure" \
    "$(plane_exit_message server 1 0)" \
    "aimee-server exited (status 1); shutting down webchat"

# The two above share status 1 — that is the whole point. Prove they differ.
if [ "$(plane_exit_message server 1 1)" = "$(plane_exit_message server 1 0)" ]; then
    echo "  FAIL: signalled and failed exits are indistinguishable at status 1" >&2
    fail=1
else
    echo "  ok: signalled and failed exits differ despite identical status"
fi

# The WFE plane must be named as itself.
expect "wfe exit names aimee-wfe" \
    "$(plane_exit_message wfe 2 0)" \
    "aimee-wfe exited (status 2); shutting down webchat"

expect "wfe termination names aimee-wfe" \
    "$(plane_exit_message wfe 1 1)" \
    "aimee-wfe stopped on termination signal (status 1); shutting down webchat"

# Single-plane path leaves `first` empty; it must still name the server.
expect "empty first defaults to aimee-server" \
    "$(plane_exit_message '' 137 0)" \
    "aimee-server exited (status 137); shutting down webchat"

if [ "$fail" -ne 0 ]; then
    echo "check-entrypoint-exit-report: FAILED" >&2
    exit 1
fi
echo "check-entrypoint-exit-report: ok"
