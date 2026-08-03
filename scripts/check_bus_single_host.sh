#!/usr/bin/env bash
# check_bus_single_host.sh — enforce D8: there is exactly one bus host, and the
# Go client is a client only.
#
# The conformance suite's credibility rests on two INDEPENDENT client
# implementations agreeing about ONE host. If a second host implementation
# existed, or if the Go "client" actually created regions and admitted peers, the
# agreement would be a codec agreeing with itself. Four assertions, each
# mechanical:
#
#   1. bus_host_create is defined in exactly one translation unit.
#   2. memfd_create (region creation) appears only in the bus's host-only region
#      code, never in a second would-be host.
#   3. no Go file in server-go/bus creates regions or accepts attaches — it maps
#      what it is handed and connects; it never listens or memfds.
#   4. no Go test regenerates or shadows the committed wire vectors, so a drift
#      in the C host's frame bytes is caught by the C<->Go vector agreement rather
#      than papered over by a Go-local override.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail=0
note() { printf '%s\n' "$*" >&2; }

# 1. exactly one definition of bus_host_create.
defs=$({ grep -rlE 'bus_host_result_t[[:space:]]+bus_host_create[[:space:]]*\(' \
   src/core/event_bus/*.c 2>/dev/null || true; })
n=$(printf '%s\n' "$defs" | grep -c . || true)
if [ "$n" != "1" ]; then
   note "FAIL: bus_host_create is defined in $n translation units (want 1):"
   printf '%s\n' "$defs" >&2
   fail=1
fi

# 2. Within the bus module, a region (memfd) is created in exactly one place —
#    bus_region_host.c. This is about a second BUS host, not global memfd policy:
#    other subsystems (git credential fds, ssh-agent) use memfd_create for their
#    own reasons and are none of the bus's business. So the scan is scoped to
#    src/core/event_bus/ and matches the CALL (with a paren), not comments.
while IFS= read -r hit; do
   [ -z "$hit" ] && continue
   file="${hit%%:*}"
   case "$file" in
   src/core/event_bus/bus_region_host.c) continue ;;
   esac
   note "FAIL: a second region creator — memfd_create called outside bus_region_host.c: $hit"
   fail=1
done < <(grep -rn 'memfd_create[[:space:]]*(' src/core/event_bus/*.c 2>/dev/null || true)

# 3. the Go client is a client only: it must not create regions (memfd) or
#    accept/listen (a host does that).
if [ -d server-go/bus ]; then
   for forbidden in 'MemfdCreate' 'unix.Listen' 'unix.Accept' 'unix.Bind'; do
      if grep -rn "$forbidden" server-go/bus/*.go 2>/dev/null | grep -v '_test.go' >/dev/null; then
         note "FAIL: server-go/bus uses '$forbidden' — a client does not host"
         grep -rn "$forbidden" server-go/bus/*.go | grep -v '_test.go' >&2
         fail=1
      fi
   done

   # 4. no Go file writes the committed vector table (only the Python generator
   #    may). A Go-local vector override would hide a real wire drift.
   if grep -rn 'wire_vectors\.tsv' server-go/ 2>/dev/null | grep -iE 'Create|OpenFile|WriteFile|os\.O_WRONLY|os\.O_CREATE' >/dev/null; then
      note "FAIL: a Go file writes wire_vectors.tsv — the vectors are the single authority"
      fail=1
   fi
fi

if [ "$fail" -ne 0 ]; then
   note ""
   note "D8: exactly one host, and the Go side is a client only. See"
   note "docs/dev/EVENT_BUS_DECISIONS.md."
   exit 1
fi

echo "check_bus_single_host: ok — one host_create, memfd only in the host, Go is client-only"
