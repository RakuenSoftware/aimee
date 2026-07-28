#!/bin/sh
set -eu

. ../deploy/container/plane-supervisor.sh

exercise_exit() (
    first=$1
    sleep 30 & server_pid=$!
    sleep 30 & wfe_pid=$!
    trap 'kill "$server_pid" "$wfe_pid" 2>/dev/null || true' EXIT INT TERM
    if [ "$first" = server ]; then victim=$server_pid; peer=$wfe_pid; else victim=$wfe_pid; peer=$server_pid; fi
    ( sleep 0.2; kill "$victim" ) &
    aimee_supervise_plane_pair "$server_pid" "$wfe_pid"
    [ "$AIMEE_FIRST_EXIT" = "$first" ] || return 11
    if kill -0 "$peer" 2>/dev/null; then return 12; fi
    return 0
)

server_rc=0
exercise_exit server || server_rc=$?
if [ "$server_rc" -ne 0 ]; then
    echo "FAIL: server-plane supervision assertion $server_rc" >&2
    exit 1
fi
wfe_rc=0
exercise_exit wfe || wfe_rc=$?
if [ "$wfe_rc" -ne 0 ]; then
    echo "FAIL: WFE-plane supervision assertion $wfe_rc" >&2
    exit 1
fi
# The wrapper must report the FIRST plane's REAL status after terminating its
# peer. It used to `return 1` unconditionally, so a SIGKILL, an OOM, a clean stop
# and a genuine failure all surfaced as "exited (status 1)" — the status was a
# constant, not a measurement, and it sent an operator hunting a core dump for a
# container that had merely been stopped.
#
# restart: unless-stopped recreates on any exit code, so the true status costs
# nothing in the restart contract.
exercise_unit_signal() (
    _signal=$1
    _want=$2
    sleep 30 & _srv=$!
    sleep 30 & _wfe=$!
    trap 'kill -KILL "$_srv" "$_wfe" 2>/dev/null || true' EXIT INT TERM
    ( sleep 0.2; kill "$_signal" "$_srv" 2>/dev/null ) &
    _rc=0
    aimee_supervise_plane_unit "$_srv" "$_wfe" || _rc=$?
    if [ "$_rc" -ne "$_want" ]; then
        echo "FAIL: kill $_signal -> status $_rc, want $_want" >&2
        return 1
    fi
    if kill -0 "$_wfe" 2>/dev/null; then
        echo "FAIL: peer survived after kill $_signal" >&2
        return 1
    fi
    echo "  ok: kill $_signal reported as $_rc"
)

# A stopped container and an OOM kill must not look alike, and neither may look
# like a plain failure. All three collapsed to 1 before.
exercise_unit_signal -TERM 143
exercise_unit_signal -KILL 137

# A plane that fails on its own reports its own code, not a laundered 1.
exercise_unit_code() (
    _want=$1
    sh -c 'sleep 0.2; exit '"$_want" & _srv=$!
    sleep 30 & _wfe=$!
    trap 'kill -KILL "$_srv" "$_wfe" 2>/dev/null || true' EXIT INT TERM
    _rc=0
    aimee_supervise_plane_unit "$_srv" "$_wfe" || _rc=$?
    if [ "$_rc" -ne "$_want" ]; then
        echo "FAIL: exit $_want -> status $_rc" >&2
        return 1
    fi
    echo "  ok: exit $_want reported as $_rc"
)
exercise_unit_code 3
exercise_unit_code 1

# A plane exiting 0 is still a failure OF THE UNIT: the pair must come down and
# be recreated, so 0 maps to 1 rather than signalling success to the entrypoint.
unit0_rc=0
(
    sh -c 'sleep 0.2; exit 0' & _srv=$!
    sleep 30 & _wfe=$!
    trap 'kill -KILL "$_srv" "$_wfe" 2>/dev/null || true' EXIT INT TERM
    _rc=0
    aimee_supervise_plane_unit "$_srv" "$_wfe" || _rc=$?
    [ "$_rc" -eq 1 ] || { echo "FAIL: clean plane exit -> $_rc, want 1" >&2; exit 1; }
    echo "  ok: plane exit 0 still fails the unit as 1"
) || unit0_rc=$?
[ "$unit0_rc" -eq 0 ]

grep -q 'aimee_supervise_plane_unit' ../deploy/container/server-entrypoint.sh
echo "server-plane-supervisor: ok (both exit directions terminate peer; real status reported)"
