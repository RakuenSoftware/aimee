#!/bin/sh
# Validate the native crash-evidence destination before starting aimee-server.
# The kernel interprets core_pattern in the crashing process's mount namespace,
# so an appliance host path is useless unless the same path is mounted here.

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
            if [ "${PWD:-}" = "${AIMEE_HOME:-/var/lib/aimee}" ]; then
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
