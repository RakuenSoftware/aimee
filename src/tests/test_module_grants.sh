#!/bin/sh
# Module-grant seeding: telling a stale image default from an operator's policy.
#
# Seeding never overwrites a persisted grant, so a module that gains a stage
# cannot serve it under an older grant -- and the bus fails SILENTLY, refusing
# the kind with the daemon otherwise healthy. Blindly adopting the shipped stage
# list would fix that by trampling deliberate policy, which is a privilege
# expansion and worse than the bug.
#
# The rules under test:
#   1. a missing grant is seeded, and what was seeded is recorded
#   2. an UNMODIFIED seeded grant is refreshed when the image ships new stages
#   3. an OPERATOR-EDITED grant is never touched, only warned about
#   4. a grant identical to the shipped one is adopted as managed, so existing
#      installs come under management instead of staying stuck forever
#   5. a grant pinning an executable the image no longer ships is reconciled
#      (the upgrade that took a live server down)
#   6. exact historical defaults from before seed records are refreshable, but
#      a nearby operator edit is not
#
# Runs the REAL block out of the entrypoint rather than a copy of its logic:
# the region between the module-grant-seeding sentinels is extracted verbatim.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
entrypoint="$root/deploy/container/server-entrypoint.sh"
[ -f "$entrypoint" ] || { echo "missing $entrypoint" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

block="$tmp/seeding.sh"
sed -n '/^# >>> module-grant-seeding/,/^# <<< module-grant-seeding/p' "$entrypoint" > "$block"
[ -s "$block" ] || { echo "could not extract the seeding block; sentinels moved?" >&2; exit 1; }

fail=0
ok()  { echo "  ok    $1"; }
bad() { echo "  FAIL  $1" >&2; fail=1; }

# Each case gets a fresh AIMEE_HOME and a fresh "image" grants directory.
setup() {
    caseno=$((caseno + 1))
    AIMEE_HOME="$tmp/home$caseno"
    AIMEE_MODULE_GRANT_SRC="$tmp/image$caseno"
    export AIMEE_HOME AIMEE_MODULE_GRANT_SRC
    mkdir -p "$AIMEE_HOME/modules.d/server" "$AIMEE_MODULE_GRANT_SRC"
}
caseno=0

# The executable a grant pins must exist, or the reconciliation loop rewrites it.
real_exe="$tmp/aimee-wfe"
printf '#!/bin/sh\n' > "$real_exe"
chmod 0755 "$real_exe"

write_grant() { # <path> <executable> <serve>
    cat > "$1" <<EOF
version=1
principal_class=1
principal_ref=20
uid=self
executable=$2
publish=
subscribe=
request=
serve=$3
EOF
}

write_git_grant() { # <path> <executable> <serve> [subscribe]
    cat > "$1" <<EOF
version=1
principal_class=1
principal_ref=13
uid=self
executable=$2
publish=
subscribe=${4:-}
request=
serve=$3
EOF
}

write_module_grant() { # <path> <principal-ref> <executable> <serve>
    cat > "$1" <<EOF
version=1
principal_class=1
principal_ref=$2
uid=self
executable=$3
publish=
subscribe=
request=
serve=$4
EOF
}

run_seeding() { sh "$block" 2>"$tmp/err$caseno"; }
serve_of() { sed -n 's/^serve=//p' "$1"; }

echo "1. a missing grant is seeded and recorded"
setup
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217,9218"
run_seeding
target="$AIMEE_HOME/modules.d/server/workflows.grant"
[ -f "$target" ] && ok "seeded" || bad "grant was not seeded"
[ -f "$AIMEE_HOME/modules.d/server/.seeded/workflows.grant" ] && ok "seed recorded" || bad "no seed record"

echo "2. an unmodified seeded grant adopts new stages from a later image"
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217,9218,9219"
run_seeding
if [ "$(serve_of "$target")" = "9217,9218,9219" ]; then ok "adopted the shipped stages"
else bad "stale default was not refreshed (serve=$(serve_of "$target"))"; fi

echo "3. an operator-edited grant is never overwritten"
setup
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217,9218"
run_seeding
target="$AIMEE_HOME/modules.d/server/workflows.grant"
write_grant "$target" "$real_exe" "9217"          # operator tightens policy
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217,9218,9219"
run_seeding
if [ "$(serve_of "$target")" = "9217" ]; then ok "operator policy preserved"
else bad "operator policy was overwritten (serve=$(serve_of "$target"))"; fi
if grep -q 'treated as operator policy' "$tmp/err$caseno"; then ok "warned instead"
else bad "no warning explaining the refusal"; fi

echo "4. a grant identical to the shipped one is adopted as managed"
setup
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217"
target="$AIMEE_HOME/modules.d/server/workflows.grant"
write_grant "$target" "$real_exe" "9217"          # pre-existing, never recorded
run_seeding
[ -f "$AIMEE_HOME/modules.d/server/.seeded/workflows.grant" ] && ok "adopted as managed" \
    || bad "identical pre-existing grant stayed unmanaged"
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217,9218"
run_seeding
if [ "$(serve_of "$target")" = "9217,9218" ]; then ok "now refreshable"
else bad "still not refreshable (serve=$(serve_of "$target"))"; fi

echo "5. a grant pinning a vanished executable is reconciled, not left to brick boot"
setup
write_grant "$AIMEE_MODULE_GRANT_SRC/workflows.grant" "$real_exe" "9217,9218"
target="$AIMEE_HOME/modules.d/server/workflows.grant"
write_grant "$target" "$tmp/removed-by-this-image" "9217"
run_seeding
if grep -q "^executable=$real_exe$" "$target"; then ok "adopted the shipped grant"
else bad "stale pin survived: $(sed -n 's/^executable=//p' "$target")"; fi

echo "6. a module the image no longer ships loses its grant"
setup
target="$AIMEE_HOME/modules.d/server/gone.grant"
write_grant "$target" "$tmp/removed-by-this-image" "9999"
run_seeding
[ -f "$target" ] && bad "grant for a removed module survived" || ok "stale grant removed"

echo "7. exact pre-record defaults adopt their added stages"
for transition in \
    "git 13 7425 7425,7426" \
    "skills 14 7681 7681,7682" \
    "roundtable 21 9473 9473,9474" \
    "benchmarks 25 10497 10497,10498"
do
    set -- $transition
    setup
    write_module_grant "$AIMEE_MODULE_GRANT_SRC/$1.grant" "$2" "$real_exe" "$4"
    target="$AIMEE_HOME/modules.d/server/$1.grant"
    write_module_grant "$target" "$2" "$real_exe" "$3"
    run_seeding
    if [ "$(serve_of "$target")" = "$4" ]; then ok "$1 historical default refreshed"
    else bad "$1 historical default stayed stale (serve=$(serve_of "$target"))"; fi
    [ -f "$AIMEE_HOME/modules.d/server/.seeded/$1.grant" ] && ok "$1 default recorded" \
        || bad "$1 historical default was not recorded"
done

echo "8. a nearby pre-record operator edit is preserved"
setup
write_git_grant "$AIMEE_MODULE_GRANT_SRC/git.grant" "$real_exe" "7425,7426"
target="$AIMEE_HOME/modules.d/server/git.grant"
write_git_grant "$target" "$real_exe" "7425" "7001"
run_seeding
if [ "$(serve_of "$target")" = "7425" ] && grep -q '^subscribe=7001$' "$target"; then
    ok "nearby operator policy preserved"
else
    bad "nearby operator policy was overwritten"
fi
if grep -q 'treated as operator policy' "$tmp/err$caseno"; then ok "nearby edit warned"
else bad "nearby edit produced no policy warning"; fi

[ "$fail" -eq 0 ] && echo "test_module_grants: ok"
exit "$fail"
