#!/bin/sh
# Shared two-plane lifecycle primitive for aimee-server + aimee-wfe.
# Sourced by server-entrypoint.sh and exercised with real child processes by CI.

aimee_pid_start_time() {
    awk '{ print $22 }' "/proc/$1/stat" 2>/dev/null || true
}

aimee_pid_is_live() {
    _aimee_pid=$1 _aimee_born=$2
    _aimee_state_born=$(awk '{ print $3 " " $22 }' "/proc/$_aimee_pid/stat" 2>/dev/null || true)
    [ -n "$_aimee_state_born" ] || return 1
    set -- $_aimee_state_born
    [ "$1" != Z ] && [ "$2" = "$_aimee_born" ]
}

# Wait until either child exits, terminate and reap its peer, and expose the
# first plane as AIMEE_FIRST_EXIT. Both PIDs must be children of this shell.
aimee_supervise_plane_pair() {
    _aimee_server_pid=$1 _aimee_wfe_pid=$2
    _aimee_server_born=$(aimee_pid_start_time "$_aimee_server_pid")
    _aimee_wfe_born=$(aimee_pid_start_time "$_aimee_wfe_pid")

    while aimee_pid_is_live "$_aimee_server_pid" "$_aimee_server_born" &&
          aimee_pid_is_live "$_aimee_wfe_pid" "$_aimee_wfe_born"; do
        sleep 0.1
    done
    if ! aimee_pid_is_live "$_aimee_server_pid" "$_aimee_server_born"; then
        AIMEE_FIRST_EXIT=server
        _aimee_peer_pid=$_aimee_wfe_pid
        _aimee_peer_born=$_aimee_wfe_born
    else
        AIMEE_FIRST_EXIT=wfe
        _aimee_peer_pid=$_aimee_server_pid
        _aimee_peer_born=$_aimee_server_born
    fi

    kill "$_aimee_peer_pid" 2>/dev/null || true
    _aimee_wait=0
    while aimee_pid_is_live "$_aimee_peer_pid" "$_aimee_peer_born"; do
        [ "$_aimee_wait" -ge 50 ] && break
        _aimee_wait=$((_aimee_wait + 1))
        sleep 0.1
    done
    if aimee_pid_is_live "$_aimee_peer_pid" "$_aimee_peer_born"; then
        kill -KILL "$_aimee_peer_pid" 2>/dev/null || true
    fi
    wait "$_aimee_server_pid" 2>/dev/null || true
    wait "$_aimee_wfe_pid" 2>/dev/null || true
}

# Entrypoint-facing wrapper: supervision returning means one plane died, so the
# two-plane unit must exit nonzero and let restartPolicy recreate both planes.
aimee_supervise_plane_unit() {
    aimee_supervise_plane_pair "$1" "$2"
    return 1
}
