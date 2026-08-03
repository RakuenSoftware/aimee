#!/bin/sh
set -eu

: "${AIMEE_TEST_MODULE_COUNT:?}"
: "${AIMEE_TEST_MODULE_PID:?}"

count=0
[ ! -r "$AIMEE_TEST_MODULE_COUNT" ] || read -r count < "$AIMEE_TEST_MODULE_COUNT"
count=$((count + 1))
printf '%s\n' "$count" > "$AIMEE_TEST_MODULE_COUNT"
printf '%s\n' "$$" > "$AIMEE_TEST_MODULE_PID"

# The first process simulates an isolated module crash. The supervisor must
# restart it without involving the daemon or another module.
[ "$count" -ne 1 ] || exit 7

stopping=0
trap 'stopping=1' TERM INT
while [ "$stopping" -eq 0 ]; do
    sleep 0.1
done
