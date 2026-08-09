#!/bin/sh
# Operator control over optional modules (deploy/container/optional-modules-lib.sh).
#
# The gate decides which processes attach to the bus, so the cases that matter
# are: an unset variable must not change the shipped default, "off" must remove a
# module the image shipped on, "on" must add one it shipped off, required modules
# must be untouchable, and the read-only shipped manifest must never be edited in
# place.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
lib="$root/deploy/container/optional-modules-lib.sh"
tmp=$(mktemp -d)
cleanup() { rm -rf "$tmp"; }
trap 'cleanup' EXIT HUP INT TERM

[ -r "$lib" ] || { echo "optional-modules: missing $lib" >&2; exit 1; }
# shellcheck source=/dev/null
. "$lib"

fails=0
check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf '  ok    %s\n' "$1"
    else
        printf '  FAIL  %s\n     expected: %s\n     actual:   %s\n' "$1" "$2" "$3" >&2
        fails=$((fails + 1))
    fi
}

# A stand-in for the shipped manifest: two required modules and one optional one
# that the image ships ON (sandbox), mirroring the real server.modules.
shipped="$tmp/shipped.modules"
{
    printf 'memory\t/usr/local/libexec/aimee-modules/aimee-module-memory\n'
    printf 'routing\t/usr/local/libexec/aimee-modules/aimee-module-routing\n'
    printf 'sandbox\t/usr/local/libexec/aimee-modules/aimee-module-sandbox\n'
} > "$shipped"
chmod 0444 "$shipped"          # read-only, as in the image
shipped_before=$(cat "$shipped")

ids() { cut -f1 "$1" | tr '\n' ' ' | sed 's/ $//'; }

# 1. Nothing set: the shipped manifest is used as-is, not copied or rewritten.
unset AIMEE_MODULE_SANDBOX AIMEE_MODULE_GOVERNANCE AIMEE_RUNTIME_WEB_ENABLED 2>/dev/null || true
out=$(apply_optional_modules server "$shipped" "$tmp")
check "unset leaves the shipped manifest untouched" "$shipped" "$out"

# 2. "off" drops a module the image shipped on.
AIMEE_MODULE_SANDBOX=0
export AIMEE_MODULE_SANDBOX
out=$(apply_optional_modules server "$shipped" "$tmp")
[ "$out" != "$shipped" ] || { echo "  FAIL  off did not produce an effective manifest" >&2; fails=$((fails + 1)); }
check "off removes sandbox" "memory routing" "$(ids "$out")"
unset AIMEE_MODULE_SANDBOX

# 3. Truthy spellings all mean the same thing.
for word in 0 false off no; do
    AIMEE_MODULE_SANDBOX="$word"; export AIMEE_MODULE_SANDBOX
    out=$(apply_optional_modules server "$shipped" "$tmp")
    check "off spelling '$word'" "memory routing" "$(ids "$out")"
    unset AIMEE_MODULE_SANDBOX
done

# 4. A required module is never gated, even if someone sets the variable.
AIMEE_MODULE_MEMORY=0; export AIMEE_MODULE_MEMORY
out=$(apply_optional_modules server "$shipped" "$tmp")
check "required memory survives AIMEE_MODULE_MEMORY=0" "memory routing sandbox" "$(ids "$out")"
unset AIMEE_MODULE_MEMORY

# 5. "on" for a module whose binary is absent must not fabricate an entry.
AIMEE_MODULE_GOVERNANCE=1; export AIMEE_MODULE_GOVERNANCE
out=$(apply_optional_modules server "$shipped" "$tmp" 2>/dev/null)
check "on with no binary present does not add governance" "memory routing sandbox" "$(ids "$out")"
unset AIMEE_MODULE_GOVERNANCE

# 6. "on" adds the module when the binary exists. Point the lookup at a fake
#    libexec by creating the expected path under a sandboxed root is not possible
#    (the lib hardcodes /usr/local/libexec), so assert the observable contract:
#    when the real binary happens to exist the id appears, otherwise it does not.
if [ -x /usr/local/libexec/aimee-modules/aimee-module-governance ]; then
    AIMEE_MODULE_GOVERNANCE=1; export AIMEE_MODULE_GOVERNANCE
    out=$(apply_optional_modules server "$shipped" "$tmp")
    check "on adds governance when installed" "memory routing sandbox governance" "$(ids "$out")"
    unset AIMEE_MODULE_GOVERNANCE
else
    printf '  skip  on-adds-governance (module binary not installed on this host)\n'
fi

# 7. runtime-web follows the browser-UI switch when not named explicitly.
rw="$tmp/rw.modules"
printf 'runtime-web\t/usr/local/libexec/aimee-modules/aimee-module-runtime-web\n' > "$rw"
AIMEE_RUNTIME_WEB_ENABLED=0; export AIMEE_RUNTIME_WEB_ENABLED
out=$(apply_optional_modules server "$rw" "$tmp")
check "runtime-web module follows AIMEE_RUNTIME_WEB_ENABLED=0" "" "$(ids "$out")"
# An explicit module setting wins over the UI switch.
AIMEE_MODULE_RUNTIME_WEB=1; export AIMEE_MODULE_RUNTIME_WEB
out=$(apply_optional_modules server "$rw" "$tmp")
check "explicit AIMEE_MODULE_RUNTIME_WEB=1 overrides the UI switch" "runtime-web" "$(ids "$out")"
unset AIMEE_RUNTIME_WEB_ENABLED AIMEE_MODULE_RUNTIME_WEB

# 8. kb placement gates its own set, and does not accept a server-only module.
kb="$tmp/kb.modules"
printf 'control-web\t/usr/local/libexec/aimee-modules/aimee-module-control-web\n' > "$kb"
AIMEE_MODULE_CONTROL_WEB=0; export AIMEE_MODULE_CONTROL_WEB
out=$(apply_optional_modules kb "$kb" "$tmp")
check "kb: off removes control-web" "" "$(ids "$out")"
unset AIMEE_MODULE_CONTROL_WEB

# 9. A caller whose log() writes to STDOUT must not corrupt the return value.
#
#    apply_optional_modules echoes the manifest path, so a diagnostic printed to
#    stdout lands inside the caller's command substitution. In production that is
#    exactly what happened: server-entrypoint.sh's log() printed to stdout, so
#    MODULE_MANIFEST became the log line followed by the real path,
#    module-supervisor.sh could not read that as a file, and EVERY module died
#    instead of just the one being toggled.
#
#    Every case above leaves log() undefined, so the lib's
#    `command -v log ... && log ...` guard short-circuits and the logging path is
#    never exercised. That is precisely why the suite passed while the bug
#    shipped. This case defines log() the way a real entrypoint does -- writing to
#    stdout, the wrong stream on purpose -- and asserts the return value survives.
log() { printf '[test-entrypoint] %s\n' "$*"; }   # deliberately stdout
AIMEE_MODULE_SANDBOX=0; export AIMEE_MODULE_SANDBOX
out=$(apply_optional_modules server "$shipped" "$tmp")
unset AIMEE_MODULE_SANDBOX
if [ -r "$out" ]; then
    printf '  ok    stdout log() does not corrupt the returned manifest path\n'
    check "returned path is the rewritten manifest" "$tmp/server.modules" "$out"
    check "diagnostic did not leak into the manifest" "memory routing" "$(ids "$out")"
else
    printf '  FAIL  stdout log() corrupted the returned manifest path\n     got: %s\n' \
        "$out" >&2
    fails=$((fails + 1))
fi
unset -f log

# 10. The caller captures this function's stdout as the manifest path, so stdout
# must carry NOTHING but that path. Every case above ran without a `log` defined,
# and the library only logs when one exists — so the diagnostics were never on
# stdout to begin with and the contract went untested. The real caller
# (server-entrypoint.sh) does define log(), and when its log() wrote to stdout the
# captured "path" became the diagnostic plus the path. The module supervisor was
# handed that and died with "fatal: missing module manifest", taking down every
# module — the whole point of the gate — the moment any optional module was gated.
#
# Tested behaviourally against the entrypoint's OWN log() definition, extracted
# and evaluated here, so this pins the real caller rather than a restatement of
# it. A log() that prints to stdout fails this.
entrypoint="$root/deploy/container/server-entrypoint.sh"
[ -r "$entrypoint" ] || { echo "optional-modules: missing $entrypoint" >&2; exit 1; }
log_def=$(grep -m1 '^log() {' "$entrypoint")
[ -n "$log_def" ] || { echo "optional-modules: no log() in $entrypoint" >&2; exit 1; }
eval "$log_def"
check "the entrypoint's log() writes nothing to stdout" "" "$(log probe 2>/dev/null)"

# And with that same log() in scope, the captured value must be a usable path.
AIMEE_MODULE_GOVERNANCE=1
export AIMEE_MODULE_GOVERNANCE
out=$(apply_optional_modules server "$shipped" "$tmp" 2>/dev/null)
check "captured stdout is a readable manifest path" \
    "1" "$(test -r "$out" && echo 1 || echo 0)"
check "captured stdout carries no diagnostic text" \
    "0" "$(printf '%s' "$out" | grep -c 'server-entrypoint' || true)"
unset AIMEE_MODULE_GOVERNANCE
unset -f log 2>/dev/null || true

# 11. The shipped manifest itself is never modified. Last, so it covers every
# case above.
check "shipped manifest is unmodified" "$shipped_before" "$(cat "$shipped")"

if [ "$fails" -ne 0 ]; then
    echo "test_optional_modules: $fails failure(s)" >&2
    exit 1
fi
echo "test_optional_modules: ok"
