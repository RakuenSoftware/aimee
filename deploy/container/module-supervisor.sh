#!/bin/sh
# Supervise the same-container processes attached to one daemon's local bus.
# Each worker is independently restarted; a module crash does not kill the
# daemon, another module, or the other machine's/container's bus.

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: module-supervisor PLACEMENT BUS_SOCKET MODULE_MANIFEST" >&2
    exit 2
fi

placement=$1
bus_socket=$2
manifest=$3
stopping=0
workers=""

log() {
    printf '[module-supervisor:%s] %s\n' "$placement" "$*" >&2
}

module_worker() {
    module_id=$1
    executable=$2
    child=""
    worker_stopping=0
    worker_stop() {
        worker_stopping=1
        [ -n "$child" ] && kill "$child" 2>/dev/null || true
    }
    trap 'worker_stop' TERM INT
    while [ "$worker_stopping" -eq 0 ]; do
        while [ ! -S "$bus_socket" ] && [ "$worker_stopping" -eq 0 ]; do
            sleep 0.1
        done
        [ "$worker_stopping" -eq 0 ] || break
        log "starting $module_id"
        AIMEE_MODULE_PLACEMENT="$placement" "$executable" "$bus_socket" &
        child=$!
        rc=0
        wait "$child" || rc=$?
        child=""
        [ "$worker_stopping" -eq 0 ] || break
        log "$module_id exited (status $rc); restarting"
        sleep 1
    done
}

stop_all() {
    stopping=1
    for worker in $workers; do
        kill "$worker" 2>/dev/null || true
    done
}
trap 'stop_all' TERM INT

if [ ! -r "$manifest" ]; then
    log "fatal: missing module manifest $manifest"
    exit 1
fi

while IFS="	" read -r module_id executable; do
    [ -n "$module_id" ] || continue
    if [ ! -x "$executable" ]; then
        log "fatal: $module_id executable is missing: $executable"
        stop_all
        exit 1
    fi
    module_worker "$module_id" "$executable" &
    workers="$workers $!"
done < "$manifest"

if [ -z "$workers" ]; then
    log "no enabled modules"
    exit 0
fi

for worker in $workers; do
    wait "$worker" 2>/dev/null || true
done
