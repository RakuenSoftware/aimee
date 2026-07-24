#!/usr/bin/env bash
# check_bus_blast_radius.sh — enforce D7 of docs/dev/EVENT_BUS_DECISIONS.md.
#
# D7 INVARIANT (post step-3). Through the twelve-slice feature tree no shipping
# binary linked the bus. Delivery step 3 — the first real module migration —
# links it into EXACTLY ONE shipping binary, aimee-server, and ONLY to carry the
# per-action governed-action audit row: guardrails_action_audit.c publishes the
# row via modules/audit/audit_bus.c, which a consumer thread drains to the ledger.
# So the blast radius is now precisely:
#   - bus SOURCE:  only src/modules/bus/* and src/modules/audit/audit_bus.c may
#                  include a bus header (layer 3).
#   - bus OBJECTS: only the aimee-server link line may name the bus objects
#                  (BUS_SHIP_OBJS), and only the BUS_SHIP definition + its compile
#                  rules may name modules/bus in src/Makefile (layer 2).
#   - every OTHER shipping binary stays bus-free (layer 4, best-effort — see below).
# This runs on every PR, so a regression that links the bus into a second binary,
# or lets a non-audit module include it, trips immediately.
#
# The check is a whitelist of where the bus may be named, not a blacklist of
# shipping targets. An earlier version enumerated the shipping source-list
# variables and searched those; that is unsound, because a bus object reaching a
# variable nobody remembered to list would pass. Inverting it makes the textual
# layer complete by construction: the bus may appear in exactly the places below
# and nowhere else in the build.
#
# Layer 4 honesty note: the shipping binaries are stripped (-s) with LTO, so `nm`
# reports no static symbols for them — it cannot actually see a bus symbol linked
# into a stripped binary. Layer 4 is therefore a best-effort backstop for
# UNSTRIPPED/CI builds; the load-bearing guarantee is layers 1-3, which reason
# about source and build text and are complete by construction.
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
# src/Makefile builds every shipping binary. Post step-3 the bus IS named here,
# but only to carry the audit migration into aimee-server. Two textual invariants
# keep "only via audit, only aimee-server" complete by construction:
#   (a) modules/bus may be NAMED only in the BUS_SHIP source list and the two
#       compile rules that add -Imodules/bus per bus/audit_bus object. Anything
#       else naming modules/bus is a new, unaudited bus reference in the shipping
#       build.
#   (b) the bus objects (BUS_SHIP_OBJS) may be CONSUMED only by their own
#       definition and the aimee-server link line ($(SERVER):). A second binary
#       linking them would widen the blast radius past audit-in-the-server.
hits=$({ grep -n 'modules/bus' src/Makefile 2>/dev/null || true; } | strip_comments |
   { grep -vE 'BUS_SHIP_SRCS|C_FLAGS \+= -Imodules/bus' || true; })
if [ -n "$hits" ]; then
   note "FAIL: src/Makefile names modules/bus outside the BUS_SHIP definition/compile rules"
   printf '%s\n' "$hits" >&2
   fail=1
fi
hits=$({ grep -n 'BUS_SHIP_OBJS' src/Makefile 2>/dev/null || true; } | strip_comments |
   { grep -vE 'BUS_SHIP_OBJS =|\$\(SERVER\):' || true; })
if [ -n "$hits" ]; then
   note "FAIL: BUS_SHIP_OBJS is linked by a target other than aimee-server (\$(SERVER))"
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

# In the two test build files where the bus IS allowed, it may only feed bus
# test targets.
for f in src/tests/CMakeLists.txt src/tests/Rules.mk; do
   [ -f "$f" ] || continue
   hits=$({ grep -n 'modules/bus' "$f" 2>/dev/null || true; } | strip_comments |
      { grep -vE 'test_bus|unit-test-bus|bus-conformance-host|bus_conformance_host|bus-bench|bus_bench|audit_bus|audit_replay|guardrails_semantic|OBJDIR\)/modules/bus' || true; })
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
   src/tests/bus_conformance_host.c) continue ;; # slice-10 test harness
   src/tests/bus_bench.c) continue ;;             # slice-12 benchmark
   # The first real module migration onto the bus (delivery step 3): the audit
   # module is a bus consumer, so it includes bus headers. audit_bus.c publishes
   # /drains the governed-action row; audit_replay.c reads a capture file back for
   # the aimee-server --audit-replay operator tool. Both are linked into
   # aimee-server only (BUS_SHIP_OBJS); layer 1 proves no other target links them.
   src/modules/audit/audit_bus.c) continue ;;
   src/modules/audit/audit_replay.c) continue ;;
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
# The shipping set comes from the build's own `all:` target rather than a list
# kept in this script. `all` is the build's authoritative statement of what
# ships, so a renamed or newly added binary is covered the day it is added
# instead of the day someone remembers to edit here.
all_prereqs=$(sed -nE 's/^all:[[:space:]]*(.*)$/\1/p' src/Makefile | head -1)
if [ -z "$all_prereqs" ]; then
   note "FAIL: cannot find the 'all:' target in src/Makefile — the artefact layer"
   note "      has no authoritative list of what ships and must not report clean."
   exit 1
fi

# Keep only the $(VAR) prerequisites; phony helpers like retire-obsolete-binaries
# are not artefacts.
shipping_var_names=$(printf '%s\n' "$all_prereqs" | grep -oE '\$\([A-Z_]+\)' |
   tr -d '$()' | sort -u)
if [ -z "$shipping_var_names" ]; then
   note "FAIL: the 'all:' target names no variables — the shipping set is unknown."
   exit 1
fi

shipping_bins=""
for v in $shipping_var_names; do
   if ! val=$(make -C src --no-print-directory \
      --eval "bus-print-var:; @echo \$($v)" bus-print-var 2>/dev/null); then
      note "FAIL: cannot ask make to resolve \$$v ($v) — the artefact layer cannot run,"
      note "      and a gate that cannot run must not report clean."
      exit 1
   fi
   # Emptiness is tested without destroying the value: an earlier version
   # squeezed out whitespace to check for empty, which silently welded a
   # multi-path variable into one nonsense filename.
   case "$val" in
   *[![:space:]]*) ;;
   *)
      note "FAIL: src/Makefile variable \$$v resolved to nothing, but 'all:' names it."
      note "      A binary this gate believes it covers is invisible to it."
      exit 1
      ;;
   esac
   shipping_bins="$shipping_bins $val"
done

# shellcheck disable=SC2086
set -- $shipping_bins
expected=$#
if [ "$expected" -eq 0 ]; then
   note "FAIL: the shipping binary list is empty — the artefact layer is blind."
   exit 1
fi

# Any symbol the bus actually exports, read from the bus sources rather than a
# hand-kept prefix list, so a new bus_* entry point is covered the day it is
# written. Both .c and .h are scanned: a bus entry point defined in a .c file
# but not declared in a header would otherwise be outside the pattern, and a
# stray linked symbol of that form would slip through. Falls back to the module
# prefix if the sources cannot be read.
bus_syms=$({ grep -hoE 'bus_[a-z_]+[[:space:]]*\(' src/modules/bus/*.c src/modules/bus/*.h \
   2>/dev/null || true; } | grep -oE 'bus_[a-z_]+' | sort -u || true)
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
   # aimee-server carries the bus on purpose (D7 step 3: the audit migration).
   # It is EXPECTED to reference bus symbols; layers 1-3 and the src/Makefile
   # confinement above prove no other target links them, and nm cannot see static
   # bus symbols in this stripped binary in any case. Exempt it explicitly rather
   # than leaning on that blindness.
   case "$(basename "$path")" in
   aimee-server | aimee-server.exe)
      checked=$((checked + 1))
      continue
      ;;
   esac
   # Defined symbols catch a linked bus object; undefined ones catch a shipping
   # object that references the bus and is only waiting for someone to satisfy
   # it. Both tables are queried, and an unreadable binary fails closed.
   #
   # nm -A / nm -D -A are GNU binutils. This gate assumes the project's Linux/GNU
   # toolchain, in line with the rest of the build. On a non-GNU nm the reads
   # fail and the binary is reported unreadable — which is the safe direction
   # (fail closed), though it would mask a toolchain problem as a D7 violation.
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
   note "The event bus may be linked only into aimee-server, and only to carry the"
   note "audit migration (D7 step 3). A bus symbol in any other shipping binary, or"
   note "a bus reference outside modules/bus + audit_bus, is a blast-radius"
   note "regression. See docs/dev/EVENT_BUS_DECISIONS.md."
   exit 1
fi

# Report what actually ran. A skipped layer must never read as coverage, so the
# artefact count is stated rather than implied — but its absence is not itself a
# failure, because a fresh checkout has no build and lint must still run there.
# The artefact layer must cover the WHOLE shipping set to establish D7. There
# are exactly three states:
#
#   full     (missing == 0)                 -> clean, unconditionally.
#   unbuilt  (checked == 0, all missing)     -> nothing to inspect. --allow-unbuilt
#                                               permits it for local iteration; the
#                                               strict default (lint/CI) fails.
#   partial  (checked > 0, some missing)     -> ALWAYS a failure, flag or not.
#
# Partial is the trap the escape hatch must never cover: the binaries that were
# built came back clean, which reads like reassurance, while the one that was
# not built is exactly where a link edge the textual layers cannot see would
# hide. --allow-unbuilt exists for "I have not built anything yet", not for
# "I built most of it".
if [ "$missing" -eq 0 ]; then
   echo "check_bus_blast_radius: ok — build graph clean; all $expected shipping binary(s) carry no bus symbol"
   exit 0
fi

if [ "$checked" -eq 0 ] && [ "$allow_unbuilt" -eq 1 ]; then
   echo "check_bus_blast_radius: build graph clean; artefact layer SKIPPED (nothing built, --allow-unbuilt)"
   exit 0
fi

if [ "$checked" -gt 0 ]; then
   note "FAIL: partial artefact coverage — inspected $checked of $expected shipping"
   note "      binaries, $missing not built. --allow-unbuilt does not cover this:"
   note "      the built binaries came back clean, but the unbuilt one is exactly"
   note "      where a link edge the textual layers cannot see would hide."
   note "      Build the whole shipping set, or none of it."
else
   note "FAIL: the artefact layer inspected no shipping binaries ($missing not built)."
   note ""
   note "Layers 1-3 reason about the build text; only this one reasons about the"
   note "compiled result, and it is the only layer that can catch a link edge the"
   note "text hides. A run in which it inspected nothing has not established D7."
   note "Build first, or pass --allow-unbuilt for local iteration."
fi
exit 1
