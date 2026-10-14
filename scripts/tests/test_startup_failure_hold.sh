#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
holder=
cleanup() {
    [ -z "$holder" ] || kill "$holder" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
sh -c '. "$1"; printf "attempt\n" > "$2"; hold_startup_failure' sh \
   "$root/deploy/container/startup-failure-lib.sh" "$work/attempts" &
holder=$!
for tick in 1 2 3 4 5 6 7 8 9 10; do
    [ ! -f "$work/attempts" ] || break
    sleep 0.05
done
test -f "$work/attempts"
sleep 0.1
kill -0 "$holder"
test "$(wc -l < "$work/attempts")" -eq 1
kill -TERM "$holder"
wait "$holder"
holder=
printf '%s\n' 'Startup failure remains quiescent and terminates cleanly.'
