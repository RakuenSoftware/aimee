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
# variable nobody remembered to list would pass. Inverting it makes the textual
# layer complete by construction: the bus may appear in exactly the places below
# and nowhere else in the build.
#
# Four layers, and their honest limits:
#   1-2. Build-graph text. Complete for anything written as a literal path.
#        Line-oriented, so it cannot see through a multiline CMake command or a
#        variable a shipping target consumes later.
#   3.   Include graph. Catches a stray include before it can become a link edge.
#   4.   Built artefacts. The backstop that reasons about the result rather than
#        the intent — and the only layer that catches what 1-3 cannot see. It
#        needs a build to exist; when one does not, it says so rather than
#        passing silently, because a skipped check must never read as coverage.
set -euo pipefail

# The artefact layer is the only one that can see a link edge the build text
# hides, so "it did not run" must not read as "it found nothing". By default,
# inspecting zero shipping binaries is a failure. --allow-unbuilt exists for
# local iteration on a fresh checkout, where there is genuinely nothing to
# inspect yet; CI and `make lint` use the strict default.
allow_unbuilt=0
for arg in "$@"; do
   case "$arg" in
   --allow-unbuilt) allow_unbuilt=1 ;;
   *)
      printf 'unknown option: %s\n' "$arg" >&2
      exit 2
      ;;
   esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail=0
note() { printf '%s\n' "$*" >&2; }

# Build-graph references that are comments explain the boundary; they do not
# build anything. Applied uniformly to every build file — an earlier version
# exempted only the test files, which meant a comment in src/Makefile saying
# *why* the bus is excluded would have failed the gate.
#
# Every grep feeding this is wrapped in `|| true`: under `set -o pipefail` a
# grep that simply found nothing would otherwise take down the whole script,
# turning "clean" into an error.
# Drop whole-line comments, then cut any trailing comment off the rest, so an
# inline `# ... modules/bus ...` explaining the boundary does not read as a
# build reference. Deliberately lexical and deliberately limited:
#
#   NOT covered — a reference assembled across a line continuation, or hidden
#   behind a variable a shipping target expands later. Layer 4 is what catches
#   those, which is why layer 4 must actually run.
strip_comments() {
   grep -vE '^[0-9]+:[[:space:]]*#' | sed 's/[[:space:]]#.*$//' || true
}

# ------------------------------------------------------- 1. make build graph
# src/Makefile builds every shipping binary. The bus belongs to no shipping
# target, so it must not be named there — not in a source list, not on an
# include path, not in a link rule.
hits=$({ grep -n 'modules/bus' src/Makefile 2>/dev/null || true; } | strip_comments)
if [ -n "$hits" ]; then
   note "FAIL: src/Makefile names modules/bus — the bus must not reach a shipping target"
   printf '%s\n' "$hits" >&2
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
# Every CMakeLists.txt that could define or configure a target, not just the top
# level. Only genuinely non-build trees are excluded.
allowed_build_file() {
   case "$1" in
   src/tests/Rules.mk | src/tests/CMakeLists.txt) return 0 ;;
   *) return 1 ;;
   esac
}

while IFS= read -r f; do
   allowed_build_file "$f" && continue
   hits=$({ grep -n 'modules/bus' "$f" 2>/dev/null || true; } | strip_comments)
   if [ -n "$hits" ]; then
      note "FAIL: $f names modules/bus"
      printf '%s\n' "$hits" >&2
      fail=1
   fi
done < <(find . \( -name CMakeLists.txt -o -name '*.cmake' \) \
   -not -path './.git/*' -not -path '*/node_modules/*' -not -path './build/*' \
   -not -path '*/build/*' | sed 's|^\./||')

# In the two files where the bus IS allowed, it may only feed bus test targets.
for f in src/tests/CMakeLists.txt src/tests/Rules.mk; do
   [ -f "$f" ] || continue
   hits=$({ grep -n 'modules/bus' "$f" 2>/dev/null || true; } | strip_comments |
      { grep -vE 'test_bus|unit-test-bus|OBJDIR\)/modules/bus' || true; })
   if [ -n "$hits" ]; then
      note "FAIL: $f uses modules/bus outside a bus test target"
      printf '%s\n' "$hits" >&2
      fail=1
   fi
done

# --------------------------------------------------------- 3. include graph
# Only the bus itself and its tests may include a bus header, in any include
# form — bare, angle-bracketed, or path-qualified.
while IFS= read -r hit; do
   file="${hit%%:*}"
   case "$file" in
   src/modules/bus/*) continue ;;
   src/tests/test_bus_*) continue ;;
   esac
   note "FAIL: $hit"
   fail=1
done < <(grep -rnE '#include[[:space:]]*[<"][^">]*bus_[a-z_]+\.h[">]|#include[[:space:]]*[<"][^">]*modules/bus/' \
   src/ 2>/dev/null || true)

# ------------------------------------------------------- 4. built artefacts
# The backstop that reasons about the compiled result rather than the build
# text, and the only layer that catches what 1-3 structurally cannot see.
#
# Three ways this layer can fail to do its job, all of which must be loud:
#   - make cannot tell us what the shipping binaries are  -> gate failure
#   - the list comes back empty                           -> gate failure
#   - a binary exists but nm cannot read it               -> gate failure
# Only "the binary was never built" is a legitimate skip, and even that is
# reported rather than folded into a clean result.
# Each variable is resolved on its own and required to be non-empty. Asking for
# them in one echo would let a name that has drifted out of src/Makefile expand
# to nothing while its neighbours still produce a plausible list — the gate
# would go on inspecting five binaries and never mention the sixth it lost.
shipping_var_names="BINARY SERVER WEBCHAT KB KB_RESOLVER GATEWAY FORWARDER"
shipping_bins=""
for v in $shipping_var_names; do
   if ! val=$(make -C src --no-print-directory \
      --eval "bus-print-var:; @echo \$($v)" bus-print-var 2>/dev/null); then
      note "FAIL: cannot ask make to resolve \$$v — the artefact layer cannot run,"
      note "      and a gate that cannot run must not report clean."
      exit 1
   fi
   val=$(printf '%s' "$val" | tr -d '[:space:]')
   if [ -z "$val" ]; then
      note "FAIL: src/Makefile variable \$$v resolved to nothing."
      note "      The shipping-binary list in this gate has drifted from the build;"
      note "      a binary it believes it is covering is invisible to it."
      exit 1
   fi
   shipping_bins="$shipping_bins $val"
done

# shellcheck disable=SC2086
set -- $shipping_bins
if [ "$#" -eq 0 ]; then
   note "FAIL: the shipping binary list is empty — the artefact layer is blind."
   exit 1
fi

# Any symbol the bus actually exports, read from the bus sources rather than a
# hand-kept prefix list, so a new bus_* entry point is covered the day it is
# written. Falls back to the module prefix if the headers cannot be read.
bus_syms=$({ grep -hoE 'bus_[a-z_]+[[:space:]]*\(' src/modules/bus/*.h 2>/dev/null || true; } |
   grep -oE 'bus_[a-z_]+' | sort -u || true)
[ -n "$bus_syms" ] || bus_syms='bus_'
sym_pattern=$(printf '%s|' $bus_syms | sed 's/|$//')

checked=0
missing=0
for bin in "$@"; do
   path="src/$bin"
   [ -f "$path" ] || path="$bin"
   if [ ! -f "$path" ]; then
      missing=$((missing + 1))
      continue
    fi
   # Defined symbols catch a linked bus object; undefined ones catch a shipping
   # object that references the bus and is only waiting for someone to satisfy
   # it. Both tables are queried, and an unreadable binary fails closed.
   if ! syms=$(nm -A "$path" 2>/dev/null) && ! syms=$(nm -D -A "$path" 2>/dev/null); then
      note "FAIL: cannot read symbols from '$path' — the artefact check cannot"
      note "      clear a binary it is unable to inspect."
      fail=1
      continue
   fi
   checked=$((checked + 1))
   if printf '%s\n' "$syms" | grep -qE "\b(${sym_pattern})"; then
      note "FAIL: built binary '$path' references a bus symbol"
      printf '%s\n' "$syms" | grep -E "\b(${sym_pattern})" | head -5 >&2
      fail=1
   fi
done

if [ "$fail" -ne 0 ]; then
   note ""
   note "The event bus must not be linked into a shipping binary in this feature"
   note "tree (D7). See docs/dev/EVENT_BUS_DECISIONS.md."
   exit 1
fi

# Report what actually ran. A skipped layer must never read as coverage, so the
# artefact count is stated rather than implied — but its absence is not itself a
# failure, because a fresh checkout has no build and lint must still run there.
if [ "$checked" -eq 0 ]; then
   if [ "$allow_unbuilt" -eq 0 ]; then
      note "FAIL: the artefact layer inspected no shipping binaries ($missing not built)."
      note ""
      note "Layers 1-3 reason about the build text; only this one reasons about the"
      note "compiled result, and it is the only layer that can catch a link edge the"
      note "text hides. A run in which it inspected nothing has not established D7."
      note "Build first, or pass --allow-unbuilt for local iteration."
      exit 1
   fi
   echo "check_bus_blast_radius: build graph clean; artefact layer SKIPPED ($missing not built, --allow-unbuilt)"
   exit 0
fi

if [ "$missing" -gt 0 ]; then
   echo "check_bus_blast_radius: ok — build graph clean; $checked binary(s) carry no bus symbol, $missing not built"
else
   echo "check_bus_blast_radius: ok — build graph clean; all $checked shipping binary(s) carry no bus symbol"
fi
