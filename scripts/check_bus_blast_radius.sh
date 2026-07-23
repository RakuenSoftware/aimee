#!/usr/bin/env bash
# check_bus_blast_radius.sh — enforce D7 of docs/dev/EVENT_BUS_DECISIONS.md.
#
# The event bus is unproven until its conformance suite (slice 10) and its perf
# gate (slice 12) are green, so no shipping binary may link it while the feature
# tree is in flight. This runs on *every* slice PR, not only at tree level: a
# tree-level-only gate would let a regression between slices 10 and 12 link the
# bus into a shipping binary and still pass each slice's own checks.
#
# The check is a whitelist of where the bus may be named, not a blacklist of
# shipping targets. An earlier version enumerated the shipping source-list
# variables and searched those; that is unsound, because a bus object reaching a
# variable nobody remembered to list would pass. Inverting it makes the check
# complete by construction: the bus may appear in exactly the places below and
# nowhere else in the build, so a new link edge has nowhere to hide.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail=0
note() { printf '%s\n' "$*" >&2; }

# Build files where naming the bus is legitimate. Everything else must not.
allowed_build_file() {
   case "$1" in
   src/tests/Rules.mk | src/tests/CMakeLists.txt) return 0 ;;
   *) return 1 ;;
   esac
}

# ------------------------------------------------------- 1. make build graph
# src/Makefile builds every shipping binary. The bus belongs to no shipping
# target, so it must not be named there at all — not in a source list, not on an
# include path, not in a link rule.
if grep -n 'modules/bus' src/Makefile >/dev/null 2>&1; then
   note "FAIL: src/Makefile names modules/bus — the bus must not reach a shipping target"
   grep -n 'modules/bus' src/Makefile >&2
   fail=1
fi

# A global -Imodules/bus would put bus headers on every shipping translation
# unit. That is a boundary leak even before a link edge exists, and it is how an
# accidental dependency usually starts.
if grep -nE '^[A-Z_]*C_FLAGS[[:space:]]*[+:]?=.*-Imodules/bus' src/Makefile >/dev/null 2>&1; then
   note "FAIL: src/Makefile puts -Imodules/bus on a global C_FLAGS"
   fail=1
fi

# ------------------------------------------------------ 2. cmake build graph
# Traverse every CMakeLists.txt, not just the top level: a shipping target may
# be defined in a subdirectory.
while IFS= read -r f; do
   allowed_build_file "$f" && continue
   if grep -n 'modules/bus' "$f" >/dev/null 2>&1; then
      note "FAIL: $f names modules/bus"
      grep -n 'modules/bus' "$f" >&2
      fail=1
   fi
done < <(find . -name CMakeLists.txt -not -path './.git/*' -not -path './frontend/*' \
   -not -path './webchat/*' -not -path './node_modules/*' | sed 's|^\./||')

# In the files where the bus IS allowed, it may only feed bus test targets —
# never a shipping one. Any bus source on a line that also names a non-bus
# target is suspicious enough to review by hand.
if grep -n 'modules/bus' src/tests/CMakeLists.txt 2>/dev/null | grep -v 'test_bus' >/dev/null; then
   note "FAIL: src/tests/CMakeLists.txt uses modules/bus outside a bus test target"
   fail=1
fi

# --------------------------------------------------------- 3. include graph
# Only the bus itself and its tests may include a bus header. Catching a stray
# include early means it never gets the chance to become a link edge.
while IFS= read -r hit; do
   file="${hit%%:*}"
   case "$file" in
   src/modules/bus/*) continue ;;
   src/tests/test_bus_*) continue ;;
   esac
   note "FAIL: $hit"
   fail=1
done < <(grep -rn '#include *"bus_[a-z_]*\.h"' src/ 2>/dev/null || true)

# ------------------------------------------------------- 4. built artefacts
# If a build tree is present, confirm no shipping binary actually carries a bus
# symbol. Textual checks reason about intent; this reasons about the result.
for bin in aimee aimee-server aimee-kb aimee-gateway aimee-webchat; do
   [ -f "$bin" ] || continue
   if nm -C --defined-only "$bin" 2>/dev/null | grep -qE '\bbus_(wire|ring|region|arena|host|client)_'; then
      note "FAIL: built binary '$bin' defines a bus symbol"
      fail=1
   fi
done

if [ "$fail" -ne 0 ]; then
   note ""
   note "The event bus must not be linked into a shipping binary in this feature"
   note "tree (D7). See docs/dev/EVENT_BUS_DECISIONS.md."
   exit 1
fi

echo "check_bus_blast_radius: ok — no shipping target names, includes, or links the bus"
