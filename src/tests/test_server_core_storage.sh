#!/bin/sh
set -eu

. ../deploy/container/core-storage.sh

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
pattern_file="$tmp/core_pattern"
core_dir="$tmp/cores"

if aimee_is_elf_core /bin/sh; then
    echo "core-storage: accepted an ELF executable as ET_CORE" >&2
    exit 1
fi

AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_enable_core_dumps
if (
    ulimit -c 0
    ulimit -Hc 0
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_enable_core_dumps 2>/dev/null
); then
    echo "core-storage: accepted an immutable zero core-size limit" >&2
    exit 1
fi
(
    ulimit -c 0
    ulimit -Hc 0
    AIMEE_REQUIRE_PERSISTENT_CORES=0 aimee_enable_core_dumps 2>/dev/null
)
AIMEE_CORE_DIR="$tmp/missing" AIMEE_CORE_SELFTEST=1 \
    AIMEE_REQUIRE_PERSISTENT_CORES=0 aimee_verify_core_dump 2>/dev/null
if AIMEE_CORE_DIR="$tmp/missing" AIMEE_CORE_SELFTEST=1 \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_verify_core_dump 2>/dev/null; then
    echo "core-storage: required self-test failure did not fail closed" >&2
    exit 1
fi
if AIMEE_CORE_DIR="$tmp/missing" AIMEE_REQUIRE_PERSISTENT_CORES=1 \
    AIMEE_CORE_SELFTEST=0 aimee_verify_core_dump 2>/dev/null; then
    echo "core-storage: required mode allowed the self-test to be disabled" >&2
    exit 1
fi

printf '%s/core.%%e.%%p\n' "$core_dir" > "$pattern_file"
AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage
[ -d "$core_dir" ] && find "$core_dir" -prune -perm 1777 -print | grep -q .

ln -s "$core_dir" "$tmp/core-alias"
AIMEE_CORE_DIR="$tmp/core-alias/" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage

printf '/tmp/not-persistent/core.%%p\n' > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: accepted a non-persistent absolute core_pattern" >&2
    exit 1
fi
AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=0 aimee_prepare_core_storage 2>/dev/null

printf '|/usr/bin/collector %%p\n' > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: accepted an unverifiable external core collector" >&2
    exit 1
fi
AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=0 aimee_prepare_core_storage 2>/dev/null

printf 'core.%%p\n' > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: required mode accepted a relative core_pattern" >&2
    exit 1
fi
AIMEE_HOME=/not-the-current-directory AIMEE_CORE_DIR="$core_dir" \
    AIMEE_CORE_PATTERN_FILE="$pattern_file" AIMEE_REQUIRE_PERSISTENT_CORES=0 \
    aimee_prepare_core_storage 2>/dev/null

printf '%s/../outside/core.%%p\n' "$core_dir" > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: required mode accepted a traversal outside core storage" >&2
    exit 1
fi

printf '%s/core.%%%%p\n' "$core_dir" > "$pattern_file"
if AIMEE_CORE_DIR="$core_dir" AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: required mode accepted escaped %%p as PID attribution" >&2
    exit 1
fi

printf '%s/core.%%p\n' "$core_dir" > "$pattern_file"
if AIMEE_CORE_DIR=relative/cores AIMEE_CORE_PATTERN_FILE="$pattern_file" \
    AIMEE_REQUIRE_PERSISTENT_CORES=1 aimee_prepare_core_storage 2>/dev/null; then
    echo "core-storage: required mode accepted a relative core directory" >&2
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
        # The production verifier performs the real crash and expands kernel
        # tokens with its captured PID; do not duplicate that logic in the test.
        AIMEE_CORE_DIR="$tmp/core-alias/" AIMEE_CORE_SELFTEST=1 aimee_verify_core_dump
        ;;
esac

echo "server-core-storage: ok"
