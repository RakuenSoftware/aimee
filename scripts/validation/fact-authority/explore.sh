#!/bin/bash
# Exploratory sweep: drive the surfaces a user actually touches and watch for
# anything broken, rather than confirming a hypothesis already held.
#
# Every other script here is a targeted probe -- it answers one question that was
# asked before it was written, which means it can only find what was already
# suspected. This one walks the memory/fact surfaces the CLI and /v1 expose and
# reports whatever comes back, looking for errors, empty answers where content
# was stored, capability refusals, and daemon or module death.
# Run AS ROOT in the container.
set -u
SOCK=/root/aimee-http.sock
B="$(cat /root/kb-bearer.txt)"
fails=0

hdr() { printf '\n=== %s ===\n' "$1"; }

# $1 = label, $2 = command. Flags anything that looks like a failure.
try() {
  local label="$1"; shift
  local out
  out="$("$@" 2>&1 | head -c 600)"
  local flag=""
  case "$out" in
    *'"status":"error"'*|*'"error"'*|*'capability absent'*|*'no such table'*|\
    *'permission_error'*|*'panic'*|*'Segmentation'*|*'unavailable'*)
      flag=" <-- LOOK"; fails=$((fails + 1)) ;;
  esac
  printf '%-46s %s%s\n' "$label" "$(printf '%s' "$out" | tr '\n' ' ' | head -c 150)" "$flag"
}

# For probes that are DELIBERATELY malformed. A validation refusal is the right
# answer there, so flagging it produces permanent noise and trains the reader to
# skim past the flags -- which is how a real one gets missed. This inverts the
# check: the probe is flagged when it SUCCEEDS, or when it fails with something
# other than a clean argument refusal.
try_bad() {
  local label="$1"; shift
  local out
  out="$("$@" 2>&1 | head -c 600)"
  local flag=""
  case "$out" in
    *invalid_argument*) ;;                       # refused, and said why
    *'"status":"ok"'*) flag=" <-- LOOK (accepted a malformed request)"; fails=$((fails + 1)) ;;
    *) flag=" <-- LOOK (refused, but not as an argument error)"; fails=$((fails + 1)) ;;
  esac
  printf '%-46s %s%s\n' "$label" "$(printf '%s' "$out" | tr '\n' ' ' | head -c 150)" "$flag"
}

api() {  # $1 = path, $2 = json
  curl -s -m 60 --unix-socket "$SOCK" -H 'content-type: application/json' \
       -X POST --data "$2" "http://localhost$1"
}
kb()  { curl -s -m 60 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
             -X POST --data "$2" "http://127.0.0.1:8741/v1/actions/$1"; }

hdr "daemons and modules before"
echo "kb=$(pgrep -cf aimee-kb) server=$(pgrep -cf aimee-server) modules=$(pgrep -cf aimee-module-memory)"

# The CLI needs AIMEE_API_ENDPOINT or every command answers "server unavailable"
# and the whole surface silently goes untested; it lives in explore-cli.sh.
hdr "CLI surfaces (see explore-cli.sh)"
export AIMEE_HOME=/root
export AIMEE_API_ENDPOINT="unix:/root/aimee-http.sock"
try "aimee --version"              /usr/local/bin/aimee --version
try "aimee memory stats"           /usr/local/bin/aimee memory stats

hdr "server /v1 memory surfaces"
try "memory/store"     api /v1/memory/store '{"key":"explore-1","content":"Explore probe: the release captain is Dana.","tier":"L2","kind":"fact"}'
try "memory/search"    api /v1/memory/search '{"keywords":["release","captain"]}'
try "memory/list"      api /v1/memory/list '{"limit":3}'
try "memory/stats(GET)" curl -s -m 30 --unix-socket "$SOCK" http://localhost/v1/memory/stats
try "memory/recall"    api /v1/memory/recall '{"task_hint":"release","limit_tokens":400}'

hdr "kb action surfaces"
try "memory.facts"          kb memory.facts '{"query":"what is my email"}'
try "memory.context_block"  kb memory.context_block '{"query":"release captain","block_type":"general","limit":3}'
try "memory.briefing"       kb memory.briefing '{"limit_tokens":300}'
try "memory.entity_profile" kb memory.entity_profile '{"entity":"user"}'
try "memory.query_health"   kb memory.query_health '{}'
try "relations.schema_list" kb relations.schema_list '{}'

hdr "typed-fact correction surface"
try "facts.retract no-authority" api /v1/facts/retract '{"source":"user","relation":"nonexistent_rel"}'
try "facts.retract as user"      api /v1/facts/retract '{"source":"user","relation":"nonexistent_rel","authority":"user"}'
try_bad "entities.merge bad ids" api /v1/entities/merge '{"from_id":0,"into_id":0}'

hdr "edge cases the probes never sent"
try "empty query"        kb memory.context_block '{"query":"","block_type":"general","limit":3}'
try "very long query"    kb memory.context_block "{\"query\":\"$(head -c 3000 /dev/zero | tr '\0' 'a')\",\"limit\":3}"
try "unicode + quotes"   kb memory.context_block '{"query":"café \"quoted\" — em dash 日本語","limit":3}'
try "sql-ish query"      kb memory.context_block "{\"query\":\"'; DROP TABLE memories; --\",\"limit\":3}"
try "negative limit"     kb memory.context_block '{"query":"release","limit":-5}'
try_bad "store empty content" api /v1/memory/store '{"key":"explore-empty","content":"","tier":"L2","kind":"fact"}'

hdr "daemons and modules after"
echo "kb=$(pgrep -cf aimee-kb) server=$(pgrep -cf aimee-server) modules=$(pgrep -cf aimee-module-memory)"

hdr "new errors in the logs during this sweep"
tail -40 /root/kb.log     | grep -aiE "error|panic|fatal" | tail -5
tail -40 /root/server.log | grep -aiE "error|panic|fatal" | tail -5

printf '\n%s\n' "flagged responses: $fails (each marked LOOK above)"
