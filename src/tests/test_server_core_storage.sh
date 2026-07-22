#!/bin/sh
set -eu

. ../deploy/container/core-storage.sh

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
pattern_file="$tmp/core_pattern"
core_dir="$tmp/cores"

AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_enable_core_dumps
if (
    ulimit -c 0
    ulimit -Hc 0
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_enable_core_dumps 2>/dev/null
); then
    echo "core-storage: accepted an immutable zero core-size limit" >&2
    exit 1
fi

printf '%s/core.%%e.%%p\n' "$core_dir" > "$pattern_file"
AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage
[ -d "$core_dir" ] && [ "$(stat -c %a "$core_dir")" = 1777 ]

printf '/tmp/not-persistent/core.%%p\n' > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: accepted a non-persistent absolute core_pattern" >&2
    exit 1
fi

printf '|/usr/bin/collector %%p\n' > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: accepted an unverifiable external core collector" >&2
    exit 1
fi

# Exercise the kernel path when the test host exposes a relative core_pattern.
# Hosted runners may route crashes to systemd-coredump; policy coverage above is
# deterministic there, while appliance acceptance performs this same crash with
# its absolute mounted pattern.
host_pattern=$(cat /proc/sys/kernel/core_pattern 2>/dev/null || true)
case "$host_pattern" in
    ''|\|*|/*) echo "core-storage: actual crash check skipped for host pattern: $host_pattern" ;;
    *)
        (
            cd "$tmp"
            ulimit -c unlimited
            sh -c 'kill -SEGV $$' >/dev/null 2>&1 || true
        )
        # core_uses_pid may append .PID even when the pattern contains no token.
        # shellcheck disable=SC2086 # expansion is intentional for the probe
        set -- "$tmp"/$host_pattern "$tmp"/$host_pattern.*
        if [ ! -f "$1" ] && { [ "$#" -lt 2 ] || [ ! -f "$2" ]; }; then
            echo "core-storage: controlled SIGSEGV produced no core ($host_pattern)" >&2
            exit 1
        fi
        for produced in "$@"; do
            [ -f "$produced" ] && { echo "core-storage: controlled SIGSEGV produced $produced"; rm -f "$produced"; break; }
        done
        AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_SELFTEST=1 aimee_verify_core_dump
        ;;
esac

echo "server-core-storage: ok"
