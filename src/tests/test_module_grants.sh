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

[ "$fail" -eq 0 ] && echo "test_module_grants: ok"
exit "$fail"
