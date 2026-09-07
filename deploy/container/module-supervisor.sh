#!/bin/sh
# Compatibility entry point; Go Server/KB modules own role composition.
set -eu
[ "$#" -eq 3 ] || { echo 'usage: module-supervisor ROLE BUS_SOCKET MANIFEST' >&2; exit 2; }
case "$1" in server|kb) ;; *) echo 'invalid instance role' >&2; exit 2 ;; esac
: "${AIMEE_HOME:?}"
exec "/usr/local/libexec/aimee-modules/aimee-module-$1" __aimee_supervise_modules "$AIMEE_HOME" "$2" "$3"
