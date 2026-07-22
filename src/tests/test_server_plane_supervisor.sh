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
    [ "$AIMEE_FIRST_EXIT" = "$first" ]
    ! kill -0 "$peer" 2>/dev/null
    return 0
)

if ! exercise_exit server; then
    echo "FAIL: server-plane exit was not detected or its peer survived" >&2
    exit 1
fi
if ! exercise_exit wfe; then
    echo "FAIL: WFE-plane exit was not detected or its peer survived" >&2
    exit 1
fi
# The helper verifies first-exit attribution and peer teardown. The entrypoint
# separately converts that successful supervision result into a nonzero unit
# exit so restartPolicy recreates both planes.
grep -q 'status=1' ../deploy/container/server-entrypoint.sh
echo "server-plane-supervisor: ok (both exit directions terminate peer and unit)"
