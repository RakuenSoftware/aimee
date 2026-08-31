#!/bin/bash
# validate-memory-activation-live.sh — production recall activation, end to end.
#
# Starts the real server, DB1 store module, kb, and DB2 Postgres module against
# one disposable PostgreSQL database.  It then proves that the native
# /v1/memory/recall route carries a caller's session into DB1, sends the bounded
# activation snapshot to the DB2-only kb, applies cooldown there, and records
# only rows that kb actually returned.  A server restart in the middle proves
# the state is durable rather than worker-local.
#
# MUST RUN AS ROOT on a disposable host with PostgreSQL.
# Usage: validate-memory-activation-live.sh [--keep] [postgres://superuser@host:port/db]
set -uo pipefail
export LC_ALL=C

LIVE_KB_PORT=18931
LIVE_SRV_PORT=18933
LIVE_SERVER_ID=activation-live

. "$(cd "$(dirname "$0")" && pwd)/lib/aimee-live-env.sh"

live_env_init "memory-activation" "$@"
live_env_pg_create
live_env_start_kb
live_env_start_server

step "Seeding one DB2-managed preference with a one-turn cooldown"
memory_id=$(pg_val "INSERT INTO memories
  (tier,kind,key,content,confidence,provenance_category,
   activation_sticky_turns,activation_cooldown_turns,activation_delay_turns,
   activation_suppressed)
 VALUES ('L2','preference','activation:e2e','Prefer bounded activation proof',0.8,
         'agent_message',2,1,0,0)
 RETURNING id" | head -1)
if printf '%s' "$memory_id" | grep -Eq '^[1-9][0-9]*$'; then
   pass "seeded DB2 memory $memory_id"
else
   echo "memory-activation: could not seed the DB2 fixture (got '$memory_id')" >&2
   exit 2
fi

SERVER="http://127.0.0.1:$LIVE_SRV_PORT"
SESSION_A="activation-e2e-session-a"
SESSION_B="activation-e2e-session-b"

recall() { # recall <output-name> <session-id>
   local name=$1 sid=$2 code
   code=$(curl -sS -o "$LIVE_WORK/$name.json" -w '%{http_code}' -X POST \
      -H "x-api-key: $LIVE_SRV_BEARER" \
      -H "aimee-session-id: $sid" \
      -H 'content-type: application/json' \
      --data '{"task_hint":"activation validation","limit_tokens":2048}' \
      "$SERVER/v1/memory/recall")
   if [ "$code" = "200" ]; then
      pass "$name recall returned HTTP 200"
   else
      fail "$name recall returned HTTP $code: $(head -c240 "$LIVE_WORK/$name.json")"
   fi
}

memory_is_present() { # memory_is_present <response> <id>
   python3 - "$1" "$2" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    body = json.load(f)
mid = int(sys.argv[2])
rows = body.get("recall", {}).get("preferences", [])
raise SystemExit(0 if any(int(r.get("memory_id", 0)) == mid and
                          r.get("activation_managed") is True for r in rows) else 1)
PY
}

held_count() { # held_count <response>
   python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    print(int(json.load(f).get("recall", {}).get("activation_held", -1)))
PY
}

assert_state() { # assert_state <session> <turn> <events> <last-event-turn>
   local sid=$1 turn=$2 events=$3 last=$4 got
   got=$(pg_val "SELECT current_turn FROM context_activation_turns WHERE session_id='$sid'")
   if [ "$got" = "$turn" ]; then
      pass "$sid persisted current_turn=$turn"
   else
      fail "$sid current_turn is '${got:-missing}', expected $turn"
   fi
   got=$(pg_val "SELECT count(*) FROM context_activation_events
                 WHERE session_id='$sid' AND memory_id=$memory_id")
   if [ "$got" = "$events" ]; then
      pass "$sid has exactly $events selected-row activation event(s)"
   else
      fail "$sid has ${got:-missing} activation events, expected $events"
   fi
   got=$(pg_val "SELECT coalesce(max(turn_index),0) FROM context_activation_events
                 WHERE session_id='$sid' AND memory_id=$memory_id")
   if [ "$got" = "$last" ]; then
      pass "$sid latest selected turn is $last"
   else
      fail "$sid latest selected turn is ${got:-missing}, expected $last"
   fi
}

step "The first turn fires; the next turn is held by cooldown"
recall turn1 "$SESSION_A"
if memory_is_present "$LIVE_WORK/turn1.json" "$memory_id"; then
   pass "turn 1 returned the DB2 row and marked it activation_managed"
else
   fail "turn 1 omitted the DB2 activation row"
fi
assert_state "$SESSION_A" 1 1 1

recall turn2 "$SESSION_A"
if memory_is_present "$LIVE_WORK/turn2.json" "$memory_id"; then
   fail "turn 2 returned a row still inside its one-turn cooldown"
else
   pass "turn 2 withheld the cooling-down row"
fi
held=$(held_count "$LIVE_WORK/turn2.json")
if [ "$held" -ge 1 ]; then
   pass "turn 2 reports activation_held=$held"
else
   fail "turn 2 activation_held is $held, expected at least 1"
fi
assert_state "$SESSION_A" 2 1 1

step "Eligibility returns after cooldown, then survives a server restart"
recall turn3 "$SESSION_A"
if memory_is_present "$LIVE_WORK/turn3.json" "$memory_id"; then
   pass "turn 3 returned the row after cooldown elapsed"
else
   fail "turn 3 did not return the newly eligible row"
fi
assert_state "$SESSION_A" 3 2 3

live_env_restart_server activation
recall turn4_after_restart "$SESSION_A"
if memory_is_present "$LIVE_WORK/turn4_after_restart.json" "$memory_id"; then
   fail "turn 4 returned the row despite persisted cooldown after restart"
else
   pass "turn 4 honored persisted cooldown after server restart"
fi
assert_state "$SESSION_A" 4 2 3

step "A different caller session gets an independent activation timeline"
recall other_session_turn1 "$SESSION_B"
if memory_is_present "$LIVE_WORK/other_session_turn1.json" "$memory_id"; then
   pass "session B independently returned the row on its first turn"
else
   fail "session B inherited session A's cooldown"
fi
assert_state "$SESSION_B" 1 1 1

effectiveness_rows=$(pg_val "SELECT count(*) FROM context_snapshots")
if [ "$effectiveness_rows" = "0" ]; then
   pass "activation writes did not contaminate context-effectiveness snapshots"
else
   fail "activation writes created $effectiveness_rows context-effectiveness row(s)"
fi

live_env_verdict "real HTTP -> server DB1 -> kb DB2 activation is bounded, durable, and session-local"
