#!/bin/sh
set -eu

: "${AIMEE_TEST_MODULE_COUNT:?}"
: "${AIMEE_TEST_MODULE_PID:?}"

count=0
[ ! -r "$AIMEE_TEST_MODULE_COUNT" ] || read -r count < "$AIMEE_TEST_MODULE_COUNT"
count=$((count + 1))
# The count is what the test waits on, so it is published last: once the test
# can see this start, the pid for that same start is already durable. Bumping
# the count first would let the test read the pid file while it is still being
# rewritten (empty) or still holding the previous, dead process's pid.
# Rename complete files into place: truncating either visible file lets the
# polling reader observe EOF and abort under set -e before a value is written.
pid_tmp="$AIMEE_TEST_MODULE_PID.$$.tmp"
count_tmp="$AIMEE_TEST_MODULE_COUNT.$$.tmp"
printf '%s\n' "$$" > "$pid_tmp"
mv "$pid_tmp" "$AIMEE_TEST_MODULE_PID"
printf '%s\n' "$count" > "$count_tmp"
mv "$count_tmp" "$AIMEE_TEST_MODULE_COUNT"

# The first process simulates an isolated module crash. The supervisor must
# restart it without involving the daemon or another module.
[ "$count" -ne 1 ] || exit 7

stopping=0
trap 'stopping=1' TERM INT
while [ "$stopping" -eq 0 ]; do
    sleep 0.1
done
