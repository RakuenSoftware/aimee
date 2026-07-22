#!/bin/sh
# Validate the native crash-evidence destination before starting aimee-server.
# The kernel interprets core_pattern in the crashing process's mount namespace,
# so an appliance host path is useless unless the same path is mounted here.

aimee_core_pattern_has_pid() {
    # %% is a literal percent in core_pattern. Remove escaped pairs before
    # looking for a real %p conversion (%%%p correctly leaves one conversion).
    _pid_conversions=$(printf '%s' "$1" | sed 's/%%//g')
    printf '%s' "$_pid_conversions" | grep -q '%p'
}

aimee_is_elf_core() {
    [ -f "$1" ] || return 1
    _elf_magic=$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')
    [ "$_elf_magic" = 7f454c46 ] || return 1
    # e_type is the native-endian 16-bit field at ELF header offset 16;
    # a kernel core is ET_CORE (4), unlike ET_EXEC/ET_DYN binaries.
    _elf_type=$(od -An -tu2 -j16 -N2 "$1" 2>/dev/null | tr -d ' \n')
    [ "$_elf_type" = 4 ]
}

aimee_enable_core_dumps() {
    _required=${AIMEE_REQUIRE_PERSISTENT_CORES:-0}
    if ! ulimit -c unlimited 2>/dev/null; then
        printf '[server-entrypoint] %s: cannot raise the core-size resource limit\n' \
            "$([ "$_required" = 1 ] && printf fatal || printf warning)" >&2
        [ "$_required" = 1 ] && return 1
        return 0
    fi
    _core_limit=$(ulimit -c 2>/dev/null || printf 0)
    if [ "$_core_limit" = 0 ]; then
        printf '[server-entrypoint] %s: effective core-size resource limit is zero\n' \
            "$([ "$_required" = 1 ] && printf fatal || printf warning)" >&2
        [ "$_required" = 1 ] && return 1
        return 0
    fi
}

aimee_prepare_core_storage() {
    _core_dir=${AIMEE_CORE_DIR:-/mnt/media/cores}
    _pattern_file=${AIMEE_CORE_PATTERN_FILE:-/proc/sys/kernel/core_pattern}
    _required=${AIMEE_REQUIRE_PERSISTENT_CORES:-0}

    if [ "$_required" = 1 ]; then
        case "$_core_dir" in
            /*) ;;
            *)
                printf '[server-entrypoint] fatal: required core directory must be absolute: %s\n' \
                    "$_core_dir" >&2
                return 1
                ;;
        esac
    fi

    if ! mkdir -p "$_core_dir" 2>/dev/null ||
       ! chmod 1777 "$_core_dir" 2>/dev/null ||
       ! [ -w "$_core_dir" ]; then
        printf '[server-entrypoint] %s: persistent core directory is not writable: %s\n' \
            "$([ "$_required" = 1 ] && printf fatal || printf warning)" "$_core_dir" >&2
        [ "$_required" = 1 ] && return 1
        return 0
    fi

    if ! _core_pattern=$(cat "$_pattern_file" 2>/dev/null) || [ -z "$_core_pattern" ]; then
        printf '[server-entrypoint] %s: cannot read kernel core_pattern\n' \
            "$([ "$_required" = 1 ] && printf fatal || printf warning)" >&2
        [ "$_required" = 1 ] && return 1
        return 0
    fi

    case "$_core_pattern" in
        /*)
            if [ "$_required" = 1 ]; then
                _core_root=$(readlink -f "$_core_dir" 2>/dev/null || true)
                _core_parent=$(readlink -f "$(dirname "$_core_pattern")" 2>/dev/null || true)
                if [ -z "$_core_root" ] || [ -z "$_core_parent" ]; then
                    printf '[server-entrypoint] fatal: cannot canonicalize core storage path: %s\n' \
                        "$_core_pattern" >&2
                    return 1
                fi
                case "$_core_parent" in
                    "$_core_root") ;;
                    *)
                        printf '[server-entrypoint] fatal: core_pattern escapes persistent core storage: %s\n' \
                            "$_core_pattern" >&2
                        return 1
                        ;;
                esac
                if ! aimee_core_pattern_has_pid "$_core_pattern"; then
                    printf '[server-entrypoint] fatal: required core_pattern must contain %%p for attribution: %s\n' \
                        "$_core_pattern" >&2
                    return 1
                fi
            else
                _core_root=$(readlink -f "$_core_dir" 2>/dev/null || true)
                _core_parent=$(readlink -f "$(dirname "$_core_pattern")" 2>/dev/null || true)
                if [ -z "$_core_root" ] || [ "$_core_parent" != "$_core_root" ]; then
                    printf '[server-entrypoint] warning: core_pattern is outside persistent core storage: %s\n' \
                        "$_core_pattern" >&2
                    return 0
                fi
            fi
            printf '[server-entrypoint] native crash evidence: %s (%s)\n' \
                "$_core_pattern" "$_core_dir"
            ;;
        \|*)
            printf '[server-entrypoint] %s: core_pattern uses an external collector, not %s: %s\n' \
                "$([ "$_required" = 1 ] && printf fatal || printf warning)" \
                "$_core_dir" "$_core_pattern" >&2
            [ "$_required" = 1 ] && return 1
            return 0
            ;;
        *)
            # A relative pattern is resolved from the process cwd. The image's
            # WORKDIR is AIMEE_HOME; plugin deployments persist that directory.
            if [ "$_required" = 1 ]; then
                printf '[server-entrypoint] fatal: required core_pattern must be absolute beneath %s: %s\n' \
                    "$_core_dir" "$_core_pattern" >&2
                return 1
            elif [ "${PWD:-}" = "${AIMEE_HOME:-/var/lib/aimee}" ]; then
                printf '[server-entrypoint] native crash evidence: %s/%s\n' \
                    "$PWD" "$_core_pattern"
            else
                printf '[server-entrypoint] %s: relative core_pattern has a non-persistent cwd: %s/%s\n' \
                    "$([ "$_required" = 1 ] && printf fatal || printf warning)" \
                    "${PWD:-unknown}" "$_core_pattern" >&2
                [ "$_required" = 1 ] && return 1
            fi
            return 0
            ;;
    esac
}

# Produce a real core as the final unprivileged launch user. Appliance profiles
# enable this: startup is refused unless the kernel writes a non-empty file into
# the mounted persistent directory. The retained core is direct boot evidence.
aimee_verify_core_dump_strict() {
    _core_dir=${AIMEE_CORE_DIR:-/mnt/media/cores}
    _pattern_file=${AIMEE_CORE_PATTERN_FILE:-/proc/sys/kernel/core_pattern}
    _core_pattern=$(cat "$_pattern_file" 2>/dev/null) || return 1
    _core_dir=$(readlink -f "$_core_dir" 2>/dev/null) || return 1
    case "$_core_pattern" in
        /*)
            _pattern_parent=$(readlink -f "$(dirname "$_core_pattern")" 2>/dev/null) || return 1
            _core_pattern="$_pattern_parent/$(basename "$_core_pattern")"
            ;;
    esac
    if ! aimee_core_pattern_has_pid "$_core_pattern"; then
            if [ "$(cat /proc/sys/kernel/core_uses_pid 2>/dev/null || printf 0)" = 1 ]; then
                _core_pattern="$_core_pattern.%p"
            else
                printf '[server-entrypoint] core self-test error: controlled core cannot be attributed without %%p\n' >&2
                return 1
            fi
    fi
    _marker=$(mktemp "$_core_dir/.core-selftest.XXXXXX") || return 1
    (
        cd "$_core_dir" || exit 1
        ulimit -c unlimited || exit 1
        exec sh -c 'kill -SEGV $$'
    ) >/dev/null 2>&1 &
    _crash_pid=$!
    wait "$_crash_pid" 2>/dev/null || true
    case "$_core_pattern" in
        /*) _core_glob=$_core_pattern ;;
        *) _core_glob=$_core_dir/$_core_pattern ;;
    esac
    _core_glob=$(printf '%s' "$_core_glob" | sed \
        -e 's/%%/__AIMEE_LITERAL_PERCENT__/g' -e "s/%p/$_crash_pid/g" \
        -e 's/%[A-Za-z]/*/g' -e 's/__AIMEE_LITERAL_PERCENT__/%/g')
    _wait=0
    _produced=
    while [ "$_wait" -lt 300 ]; do
        _produced=$(find "$_core_dir" -maxdepth 1 -type f -path "$_core_glob" -newer "$_marker" \
            ! -name '.core-selftest.*' -size +4095c -print 2>/dev/null | head -n 1)
        [ -n "$_produced" ] && break
        _wait=$((_wait + 1))
        sleep 0.1
    done
    rm -f "$_marker"
    if [ -z "$_produced" ]; then
        printf '[server-entrypoint] core self-test error: controlled SIGSEGV produced no persistent core in %s\n' \
            "$_core_dir" >&2
        return 1
    fi
    if ! aimee_is_elf_core "$_produced"; then
        printf '[server-entrypoint] core self-test error: controlled SIGSEGV output is not an ELF core: %s\n' \
            "$_produced" >&2
        return 1
    fi
    printf '[server-entrypoint] controlled SIGSEGV verified persistent core: %s\n' \
        "$_produced"
}

aimee_verify_core_dump() {
    _required=${AIMEE_REQUIRE_PERSISTENT_CORES:-0}
    # Required mode owns the guarantee and cannot be weakened by independently
    # disabling the optional-mode diagnostic switch.
    if [ "$_required" != 1 ] && [ "${AIMEE_CORE_SELFTEST:-0}" != 1 ]; then
        return 0
    fi
    if aimee_verify_core_dump_strict; then
        return 0
    fi
    if [ "$_required" = 1 ]; then
        printf '[server-entrypoint] fatal: persistent core self-test failed\n' >&2
        return 1
    fi
    printf '[server-entrypoint] warning: optional persistent core self-test failed\n' >&2
    return 0
}
