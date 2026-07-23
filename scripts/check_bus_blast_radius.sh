#!/usr/bin/env bash
# check_bus_blast_radius.sh — enforce D7 of docs/dev/EVENT_BUS_DECISIONS.md.
#
# The event bus is unproven until its conformance suite (slice 10) and its perf
# gate (slice 12) are green, so no shipping binary may link it while the feature
# tree is in flight. This runs on *every* slice PR, not only at tree level: a
# tree-level-only gate would let a regression between slices 10 and 12 link the
# bus into a shipping binary and still pass each slice's own checks.
#
# What it inspects, so the check is auditable rather than magic:
#   1. src/Makefile — the source-list variables the shipping targets are built
#      from must not name anything under modules/bus/.
#   2. CMakeLists.txt — the same, for the CMake targets.
#   3. Reverse direction — no file outside src/modules/bus/ and the bus tests
#      may include a bus header, so a link edge cannot appear by way of an
#      #include that a later refactor turns into a dependency.
#
# Exit 0 when the bus is still isolated; non-zero, with the offending lines,
# when it is not.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail=0
note() { printf '%s\n' "$*" >&2; }

# ---------------------------------------------------------------- 1. Makefile
# The shipping binaries are linked from these variables. If a bus object reaches
# one of them, the bus is in a shipped link line.
shipping_vars=(
   CORE_SRCS DATA_SRCS AGENT_SRCS CMD_SRCS SERVER_SRCS KB_SRCS
   GATEWAY_SRCS WEBCHAT_SRCS MCP_GIT_SRCS VAULT_CORE_SRCS KB_VAULT_SRCS
)
for var in "${shipping_vars[@]}"; do
   # Pull the (possibly backslash-continued) assignment and look for modules/bus.
   if awk -v v="$var" '
         $0 ~ "^"v"[ \t]*[+:]?=" { inblock = 1 }
         inblock { print }
         inblock && $0 !~ /\\$/ { inblock = 0 }
      ' src/Makefile | grep -q 'modules/bus/'; then
      note "FAIL: src/Makefile: \$$var names a source under modules/bus/"
      fail=1
   fi
done

# ------------------------------------------------------------------- 2. CMake
if grep -rn 'modules/bus/' CMakeLists.txt >/dev/null 2>&1; then
   note "FAIL: CMakeLists.txt references modules/bus/ (shipping targets live here)"
   grep -rn 'modules/bus/' CMakeLists.txt >&2
   fail=1
fi

# --------------------------------------------------------- 3. include reverse
# Only the bus itself and its tests may include a bus header. Catching this
# early means a stray include never gets the chance to become a link edge.
while IFS= read -r hit; do
   file="${hit%%:*}"
   case "$file" in
   src/modules/bus/*) continue ;;
   src/tests/test_bus_*) continue ;;
   esac
   note "FAIL: $hit"
   fail=1
done < <(grep -rn '#include *"bus_[a-z_]*\.h"' src/ 2>/dev/null || true)

if [ "$fail" -ne 0 ]; then
   note ""
   note "The event bus must not be linked into a shipping binary in this feature"
   note "tree (D7). See docs/dev/EVENT_BUS_DECISIONS.md."
   exit 1
fi

echo "check_bus_blast_radius: ok — no shipping binary links the bus"
