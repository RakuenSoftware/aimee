# A rejected resource plane must stay unavailable without a container crash
# loop. Keeping PID 1 alive also retains normal Docker boot restart behavior.
hold_startup_failure() {
    startup_failure_wait_pid=
    trap '[ -z "$startup_failure_wait_pid" ] || kill "$startup_failure_wait_pid" 2>/dev/null || true; exit 0' TERM INT
    while :; do
        sleep 60 &
        startup_failure_wait_pid=$!
        wait "$startup_failure_wait_pid" || true
    done
}
