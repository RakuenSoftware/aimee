#!/bin/sh
# Validate the native crash-evidence destination before starting aimee-server.
# The kernel interprets core_pattern in the crashing process's mount namespace,
# so an appliance host path is useless unless the same path is mounted here.

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
    fi
}

aimee_prepare_core_storage() {
    _core_dir=${AIMEE_CORE_DIR:-/mnt/media/cores}
    _pattern_file=${AIMEE_CORE_PATTERN_FILE:-/proc/sys/kernel/core_pattern}
    _required=${AIMEE_REQUIRE_PERSISTENT_CORES:-0}

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
        "$_core_dir"/*)
            if [ "$_required" = 1 ] && ! printf '%s' "$_core_pattern" | grep -q '%p'; then
                printf '[server-entrypoint] fatal: required core_pattern must contain %%p for attribution: %s\n' \
                    "$_core_pattern" >&2
                return 1
            fi
            printf '[server-entrypoint] native crash evidence: %s (%s)\n' \
                "$_core_pattern" "$_core_dir"
            ;;
        \|*)
            printf '[server-entrypoint] %s: core_pattern uses an external collector, not %s: %s\n' \
                "$([ "$_required" = 1 ] && printf fatal || printf warning)" \
                "$_core_dir" "$_core_pattern" >&2
            [ "$_required" = 1 ] && return 1
            ;;
        /*)
            printf '[server-entrypoint] %s: core_pattern is outside persistent core storage: %s\n' \
                "$([ "$_required" = 1 ] && printf fatal || printf warning)" \
                "$_core_pattern" >&2
            [ "$_required" = 1 ] && return 1
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
            ;;
    esac
}

# Produce a real core as the final unprivileged launch user. Appliance profiles
# enable this: startup is refused unless the kernel writes a non-empty file into
# the mounted persistent directory. The retained core is direct boot evidence.
aimee_verify_core_dump() {
    [ "${AIMEE_CORE_SELFTEST:-0}" = 1 ] || return 0
    _core_dir=${AIMEE_CORE_DIR:-/mnt/media/cores}
    _pattern_file=${AIMEE_CORE_PATTERN_FILE:-/proc/sys/kernel/core_pattern}
    _core_pattern=$(cat "$_pattern_file" 2>/dev/null) || return 1
    case "$_core_pattern" in
        *%p*) ;;
        *)
            if [ "$(cat /proc/sys/kernel/core_uses_pid 2>/dev/null || printf 0)" = 1 ]; then
                _core_pattern="$_core_pattern.%p"
            else
                printf '[server-entrypoint] fatal: controlled core cannot be attributed without %%p\n' >&2
                return 1
            fi
            ;;
    esac
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
        -e "s/%p/$_crash_pid/g" -e 's/%%/%/g' -e 's/%[A-Za-z]/*/g')
    _wait=0
    _produced=
    while [ "$_wait" -lt 50 ]; do
        _produced=$(find "$_core_dir" -maxdepth 1 -type f -path "$_core_glob" -newer "$_marker" \
            ! -name '.core-selftest.*' -size +0c -print 2>/dev/null | head -n 1)
        [ -n "$_produced" ] && break
        _wait=$((_wait + 1))
        sleep 0.1
    done
    rm -f "$_marker"
    if [ -z "$_produced" ]; then
        printf '[server-entrypoint] fatal: controlled SIGSEGV produced no persistent core in %s\n' \
            "$_core_dir" >&2
        return 1
    fi
    printf '[server-entrypoint] controlled SIGSEGV verified persistent core: %s\n' \
        "$_produced"
}
