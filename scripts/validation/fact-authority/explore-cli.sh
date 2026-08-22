#!/bin/bash
# Exploratory sweep of the CLI surface.
#
# Split from explore.sh because the first sweep found the CLI answering
# "aimee-server unavailable / endpoint: none configured" for every command --
# it had never been pointed at the server, so the entire CLI surface was
# untested while the HTTP surface underneath it was green. A user reaches memory
# through this, so "e2e tested" that skips it is overclaiming.
#
# cli_v1_client_endpoint() takes AIMEE_API_ENDPOINT as "unix:/path" or
# "tcp:host:port"; the server's UDS is the local, same-user trusted transport.
# Run AS ROOT in the container.
set -u
export AIMEE_HOME=/root
export AIMEE_API_ENDPOINT="unix:/root/aimee-http.sock"
A=/usr/local/bin/aimee
fails=0

try() {
  local label="$1"; shift
  local out
  out="$("$@" 2>&1 | head -c 500)"
  local flag=""
  case "$out" in
    *unavailable*|*'no command catalogue'*|*'capability absent'*|*'no such table'*|\
    *panic*|*Segmentation*|*'status":"error'*) flag=" <-- LOOK"; fails=$((fails+1)) ;;
  esac
  printf '%-40s %s%s\n' "$label" "$(printf '%s' "$out" | tr '\n' ' ' | head -c 160)" "$flag"
}

echo "=== endpoint: $AIMEE_API_ENDPOINT ==="
try "status"            $A status
try "memory stats"      $A memory stats
try "memory list"       $A memory list --limit 3
try "memory search"     $A memory search deployment
try "memory store"      $A memory store cli-probe "The CLI probe stored this fact."
try "memory list after" $A memory list --limit 3
try "kb health"         $A kb health
try "describe"          $A describe

echo
echo "flagged: $fails"
