#!/usr/bin/env bash
# Enforce the two shared-C-library link boundaries:
#   - libaimee-core-connection.a -> thin client, server, and KB
#   - libouroboros-ring.a        -> server and KB only
# The ring (github.com/RakuenSoftware/ouroboros-ring, an installed release in the
# range dependencies/ouroboros-ring.json names) is local shared memory. It is
# never an inter-machine transport.
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
   local want_ring="$2"
   local line
   line=$(grep -E "^\\$\\($target\\):" src/Makefile | head -1 || true)
   if [[ -z "$line" || "$line" != *'$(CORE_CONNECTION_LIB)'* ]]; then
      printf 'FAIL: $(%s) does not consume $(CORE_CONNECTION_LIB)\n' "$target" >&2
      fail=1
   fi
   if [[ "$want_ring" == 1 && "$line" != *'$(OURO_RING_LIB)'* ]]; then
      printf 'FAIL: $(%s) does not consume $(OURO_RING_LIB)\n' "$target" >&2
      fail=1
   fi
   if [[ "$want_ring" == 0 && "$line" == *'$(OURO_RING_LIB)'* ]]; then
      printf 'FAIL: $(%s) must not consume $(OURO_RING_LIB)\n' "$target" >&2
      fail=1
   fi
}

require_target_libraries BINARY 0
require_target_libraries SERVER 1
require_target_libraries KB 1

# OURO_RING_LIB may be defined/built once and consumed by a named set of
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
# client became a ring client when the C store module was retired: the store is a
# separate process now and the client's job is to reach it. A rig that links the
# production objects rather than reimplementing them necessarily inherits what
# those objects depend on -- that is the property that makes the rig worth
# having.
RING_LIB_OWNERS='(variable)
$(SERVER)
$(KB)
../write-tier-enforce-live'

ring_lib_refs=$(awk '
   /^[ \t]*#/ { next }
   /^[A-Za-z_][A-Za-z0-9_]*[ \t]*:?=/ { owner = "(variable)" }
   /^[^ \t#]/ && /:/ && !/^[A-Za-z_][A-Za-z0-9_]*[ \t]*:?=/ {
      t = $0; sub(/:.*/, "", t); gsub(/[ \t]+$/, "", t); owner = t
   }
   /OURO_RING_LIB/ { printf "%s\t%d\n", owner, NR }
' src/Makefile)

# A scan that found nothing must not read as a boundary that holds. The archive
# is named in this Makefile and consumed by both daemons, so zero references
# means the scan broke, not that the graph is clean.
if [[ -z "$ring_lib_refs" ]]; then
   printf 'FAIL: no $(OURO_RING_LIB) reference found in src/Makefile at all; this scan is not measuring anything\n' >&2
   fail=1
fi

while IFS=$'\t' read -r owner lineno; do
   [[ -z "$owner" ]] && continue
   if ! grep -Fxq -- "$owner" <<<"$RING_LIB_OWNERS"; then
      printf 'FAIL: ring archive referenced outside its permitted graph: src/Makefile:%s is owned by %s\n' \
         "$lineno" "$owner" >&2
      printf '   The repair is almost never to add %s to RING_LIB_OWNERS. That list is\n' "$owner" >&2
      printf '   the blast radius, not a way to quiet this: widening it ships the local\n' >&2
      printf '   shared-memory ring into another binary, and the check goes green having\n' >&2
      printf '   recorded the widening as permission for it.\n' >&2
      printf '   Ask first why that target needs the ring at all -- usually it links an\n' >&2
      printf '   object that became a ring client, and the answer is to stub the call or\n' >&2
      printf '   drop the object. Add a name only when the target genuinely must speak\n' >&2
      printf '   the ring, and say in the comment above the list why.\n' >&2
      fail=1
   fi
done <<<"$ring_lib_refs"

# A permitted owner that no longer references the archive is a stale allowance,
# and a stale allowance is how an exemption list stops describing the tree it
# guards. Every name above has to still be earning its place.
while IFS= read -r owner; do
   [[ -z "$owner" ]] && continue
   if ! grep -Fq -- "$owner"$'\t' <<<"$ring_lib_refs"; then
      printf 'FAIL: %s is permitted to link the ring archive but no longer does; drop it from RING_LIB_OWNERS\n' \
         "$owner" >&2
      fail=1
   fi
done <<<"$RING_LIB_OWNERS"

# aimee does not build the ring: it links the installed package. Any reference
# to the ring's sources or objects means the build reached inside the ring again.
while IFS= read -r hit; do
   printf 'FAIL: src/Makefile reaches into the ring instead of linking its package: %s\n' "$hit" >&2
   fail=1
done < <(grep -nE 'ouroboros-ring[^ ]*/c/(src|include)|OURO_RING_[A-Z_]*\)/c/|OURO_RING_NAMES|OURO_RING_SRCS|ouroboros-ring/[a-z_]+\.o' src/Makefile | grep -vE '^[0-9]+:[[:space:]]*#' || true)

# The CMake thin client must not consume the ring.
if grep -nE 'target_link_libraries\(aimee[^)]*ouroboros' CMakeLists.txt >/dev/null; then
   printf 'FAIL: CMake thin client links the local ring\n' >&2
   fail=1
fi

# Raw ring headers remain confined to the ring adapter, the audit replay reader,
# and ring tests. Product modules use the obs_bus API.
while IFS= read -r hit; do
   file="${hit%%:*}"
   case "$file" in
   src/build/*|src/modules/audit/obs_bus.c|src/modules/audit/audit_replay.c|src/tests/test_bus_*|src/tests/test_module_runtime.c) continue ;;
   esac
   printf 'FAIL: ring header escaped the local ring/adapter boundary: %s\n' "$hit" >&2
   fail=1
done < <(grep -rnE '#include[[:space:]]*[<"]ouroboros/(attach|arena|capture|client|endpoint|host|local_attach|region|ring|route|wire)\.h[">]' src 2>/dev/null || true)

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

# The ring's client package is what module processes link, so it must stay a
# client: no host entry point (ouro_host_*, the contract's host API) may be
# defined in it. Checked against the installation provided last, when there is one.
ring_client=src/build/deps/ouroboros-ring/current/lib/libouroboros-ring-client.a
if [[ -f "$ring_client" ]]; then
   host_symbols=$(nm --defined-only "$ring_client" 2>/dev/null | awk '$2 ~ /^[TDBR]$/ && $3 ~ /^ouro_host_/ { print $3 }' || true)
   if [[ -n "$host_symbols" ]]; then
      printf 'FAIL: the ring client package defines host entry points:\n%s\n' "$host_symbols" >&2
      fail=1
   fi
elif [[ "$allow_unbuilt" != 1 ]]; then
   printf 'FAIL: no provided ring client package at %s\n' "$ring_client" >&2
   fail=1
fi

if [[ "$fail" -ne 0 ]]; then
   exit 1
fi
printf 'check_bus_blast_radius: ok — connection shared by thin/server/KB; local ring shared by server/KB only\n'
