#!/usr/bin/env bash
# Enforce the two shared-C-library link boundaries:
#   - libaimee-core-connection.a -> thin client, server, and KB
#   - libaimee-core-event-bus.a  -> server and KB only
# The event bus is local shared memory. It is never an inter-machine transport.
set -euo pipefail

allow_unbuilt=0
for arg in "$@"; do
   case "$arg" in
   --allow-unbuilt) allow_unbuilt=1 ;;
   *) printf 'unknown option: %s\n' "$arg" >&2; exit 2 ;;
   esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
fail=0

require_target_libraries() {
   local target="$1"
   local want_bus="$2"
   local line
   line=$(grep -E "^\\$\\($target\\):" src/Makefile | head -1 || true)
   if [[ -z "$line" || "$line" != *'$(CORE_CONNECTION_LIB)'* ]]; then
      printf 'FAIL: $(%s) does not consume $(CORE_CONNECTION_LIB)\n' "$target" >&2
      fail=1
   fi
   if [[ "$want_bus" == 1 && "$line" != *'$(CORE_EVENT_BUS_LIB)'* ]]; then
      printf 'FAIL: $(%s) does not consume $(CORE_EVENT_BUS_LIB)\n' "$target" >&2
      fail=1
   fi
   if [[ "$want_bus" == 0 && "$line" == *'$(CORE_EVENT_BUS_LIB)'* ]]; then
      printf 'FAIL: $(%s) must not consume $(CORE_EVENT_BUS_LIB)\n' "$target" >&2
      fail=1
   fi
}

require_target_libraries BINARY 0
require_target_libraries SERVER 1
require_target_libraries KB 1

# CORE_EVENT_BUS_LIB may be defined/built once and consumed by a named set of
# targets. This catches a future link through a target variable the source-path
# check below cannot see.
#
# Each reference is attributed to the Makefile TARGET that owns it rather than
# matched against the text of its own line, because a link line whose
# prerequisites wrap puts the archive on a continuation line that carries no
# target name at all. Matching text would report such a line as having escaped a
# graph it never left -- and, worse in the other direction, would admit any new
# target that happened to contain the string '$(SERVER):' somewhere in it.
#
# ../write-tier-enforce-live is a dev rig, deliberately not part of `all` and
# never installed. It is here because it links $(DB1_CLIENT_OBJS), and the DB1
# client became a bus client when the C store module was retired: the store is a
# separate process now and the client's job is to reach it. A rig that links the
# production objects rather than reimplementing them necessarily inherits what
# those objects depend on -- that is the property that makes the rig worth
# having.
BUS_LIB_OWNERS='(variable)
$(CORE_EVENT_BUS_LIB)
$(SERVER)
$(KB)
../write-tier-enforce-live'

bus_lib_refs=$(awk '
   /^[ \t]*#/ { next }
   /^[A-Za-z_][A-Za-z0-9_]*[ \t]*:?=/ { owner = "(variable)" }
   /^[^ \t#]/ && /:/ && !/^[A-Za-z_][A-Za-z0-9_]*[ \t]*:?=/ {
      t = $0; sub(/:.*/, "", t); gsub(/[ \t]+$/, "", t); owner = t
   }
   /CORE_EVENT_BUS_LIB/ { printf "%s\t%d\n", owner, NR }
' src/Makefile)

# A scan that found nothing must not read as a boundary that holds. The archive
# is defined in this Makefile and consumed by both daemons, so zero references
# means the scan broke, not that the graph is clean.
if [[ -z "$bus_lib_refs" ]]; then
   printf 'FAIL: no $(CORE_EVENT_BUS_LIB) reference found in src/Makefile at all; this scan is not measuring anything\n' >&2
   fail=1
fi

while IFS=$'\t' read -r owner lineno; do
   [[ -z "$owner" ]] && continue
   if ! grep -Fxq -- "$owner" <<<"$BUS_LIB_OWNERS"; then
      printf 'FAIL: event-bus archive referenced outside its permitted graph: src/Makefile:%s is owned by %s\n' \
         "$lineno" "$owner" >&2
      printf '   The repair is almost never to add %s to BUS_LIB_OWNERS. That list is\n' "$owner" >&2
      printf '   the blast radius, not a way to quiet this: widening it ships the local\n' >&2
      printf '   shared-memory bus into another binary, and the check goes green having\n' >&2
      printf '   recorded the widening as permission for it.\n' >&2
      printf '   Ask first why that target needs the bus at all -- usually it links an\n' >&2
      printf '   object that became a bus client, and the answer is to stub the call or\n' >&2
      printf '   drop the object. Add a name only when the target genuinely must speak\n' >&2
      printf '   the bus, and say in the comment above the list why.\n' >&2
      fail=1
   fi
done <<<"$bus_lib_refs"

# A permitted owner that no longer references the archive is a stale allowance,
# and a stale allowance is how an exemption list stops describing the tree it
# guards. Every name above has to still be earning its place.
while IFS= read -r owner; do
   [[ -z "$owner" ]] && continue
   if ! grep -Fq -- "$owner"$'\t' <<<"$bus_lib_refs"; then
      printf 'FAIL: %s is permitted to link the event-bus archive but no longer does; drop it from BUS_LIB_OWNERS\n' \
         "$owner" >&2
      fail=1
   fi
done <<<"$BUS_LIB_OWNERS"

# The bus implementation must enter shipping links only through its archive.
while IFS= read -r hit; do
   case "$hit" in
   *'C_FLAGS = '*' -Icore/event_bus/include'*|*'CORE_EVENT_BUS_SRCS ='*|*'CORE_EVENT_BUS_OBJS ='*|*'$(OBJDIR)/core/event_bus/%.o:'*|*'$(OBJDIR)/modules/audit/obs_bus.o:'*|*'$(OBJDIR)/modules/audit/audit_replay.o:'*) continue ;;
   esac
   printf 'FAIL: direct core/event_bus build reference outside event-bus archive: %s\n' "$hit" >&2
   fail=1
done < <(grep -n 'core/event_bus' src/Makefile | grep -vE '^[0-9]+:[[:space:]]*#' || true)

# CMake may define/package the core archive, but the thin-client target must not
# consume it. Server/KB use the canonical Make graph above.
if grep -nE 'target_link_libraries\(aimee[^)]*aimee-core-event-bus' CMakeLists.txt >/dev/null; then
   printf 'FAIL: CMake thin client links the local event bus\n' >&2
   fail=1
fi

# Raw bus headers remain confined to the bus implementation, the shared
# observability runtime, and bus tests. Product modules use the obs_bus API.
while IFS= read -r hit; do
   file="${hit%%:*}"
   case "$file" in
   src/core/event_bus/*|src/modules/audit/obs_bus.c|src/modules/audit/audit_replay.c|src/tests/test_bus_*|src/tests/test_module_runtime.c|src/tests/bus_conformance_host.c|src/tests/bus_bench.c) continue ;;
   esac
   printf 'FAIL: bus header escaped local bus/adapter boundary: %s\n' "$hit" >&2
   fail=1
done < <(grep -rnE '#include[[:space:]]*[<"]([^">]*/)?bus_(attach|arena|capture|client|endpoint|host|region|ring|route|wire)\.h[">]' src 2>/dev/null || true)

check_archive() {
   local archive="$1"
   local member_pattern="$2"
   local label="$3"
   if [[ ! -f "$archive" ]]; then
      if [[ "$allow_unbuilt" == 1 ]]; then
         printf 'check_bus_blast_radius: %s archive unbuilt (allowed)\n' "$label"
         return
      fi
      printf 'FAIL: required %s archive is unbuilt: %s\n' "$label" "$archive" >&2
      fail=1
      return
   fi
   local bad
   bad=$(ar t "$archive" | grep -Ev "$member_pattern" || true)
   if [[ -n "$bad" ]]; then
      printf 'FAIL: %s archive contains foreign objects:\n%s\n' "$label" "$bad" >&2
      fail=1
   fi
}

check_archive src/build/obj/libaimee-core-connection.a \
   '^(auth|control|endpoint|http1|socket|tls_openssl|native_tls_(identity|path|openssl|securetransport|schannel))\.o$' connection
check_archive src/build/obj/libaimee-core-event-bus.a \
   '^(bus_(attach|wire|ring|region|region_host|arena|route|runtime|host|client|capture|endpoint)|module_(client|protocol|runtime))\.o$' event-bus

# The separately packaged module-side archive must remain a client. In
# particular it cannot create memfds, admit peers, or route events.
client_target=$(sed -n \
   '/add_library(aimee-core-event-bus-client STATIC/,/^    )/p' \
   src/core/CMakeLists.txt)
for forbidden in bus_region_host.c bus_host.c bus_route.c bus_arena.c bus_capture.c; do
   if grep -Fq "$forbidden" <<<"$client_target"; then
      printf 'FAIL: event-bus client target contains host implementation: %s\n' "$forbidden" >&2
      fail=1
   fi
done

if [[ "$fail" -ne 0 ]]; then
   exit 1
fi
printf 'check_bus_blast_radius: ok — connection shared by thin/server/KB; local event bus shared by server/KB only\n'
