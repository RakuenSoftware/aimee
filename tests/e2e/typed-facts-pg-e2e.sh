#!/bin/bash
# typed-facts-pg-e2e.sh — end-to-end validation of the typed-fact knowledge layer
# against REAL PostgreSQL and a running aimee-server + aimee-kb.
#
# Why this exists: every C unit test in this repo runs against the sqlite shim
# (db2_test_shim_open), and sqlite accepts SQL that Postgres rejects. That gap
# hid db2_entity_edge_two_hop_neighbors building an unparenthesised per-branch
# LIMIT inside a UNION -- a hard syntax error on Postgres -- for as long as the
# function has existed. A green `make unit-tests` is not evidence that the SQL
# in this subsystem runs at all.
#
# It also covers the interactions that only appear in a real maintenance cycle,
# where the lifecycle jobs, the orphan prune and weight normalisation all touch
# the same rows in one pass. Two defects were found exactly there.
#
# Usage:  ./typed-facts-pg-e2e.sh            (expects the env described below)
#   AIMEE_ROOT   dir holding aimee-server / aimee-kb / aimee   (default /root/aimee)
#   AIMEE_SRC    source tree holding the built config module    (default $AIMEE_ROOT/src)
#   PGURL        libpq URL for a database the schema can be applied to
#   KB_PORT      TCP port for aimee-kb                         (default 8911)
#
# Every assertion prints PASS or FAIL; the script exits non-zero if any failed.
set -uo pipefail

AIMEE_ROOT="${AIMEE_ROOT:-/root/aimee}"
AIMEE_SRC="${AIMEE_SRC:-$AIMEE_ROOT/src}"
KB_PORT="${KB_PORT:-8911}"
PGHOST_="${PGHOST_:-127.0.0.1}"
PGUSER_="${PGUSER_:-aimee}"
PGDB_="${PGDB_:-aimee_test}"
export PGPASSWORD="${PGPASSWORD:-aimee}"
PGURL="${PGURL:-postgresql://$PGUSER_:$PGPASSWORD@$PGHOST_:5432/$PGDB_}"
export AIMEE_DB2_URL="${AIMEE_DB2_URL:-$PGURL}"
export AIMEE_HOME="${AIMEE_HOME:-/root/.aimee}"
SOCK="$AIMEE_HOME/aimee-http.sock"
CONFIG_MODULE="$AIMEE_SRC/build/obj/aimee-module-config"
MODULE_BUNDLE="${TMPDIR:-/tmp}/typed-facts-modules.$$"
MOD_PIDS=""

PASS=0; FAIL=0
check() { # check <name> <expected> <actual>
  if [ "$2" = "$3" ]; then printf '  PASS  %s\n' "$1"; PASS=$((PASS+1))
  else printf '  FAIL  %s\n        expected: %s\n        actual:   %s\n' "$1" "$2" "$3"; FAIL=$((FAIL+1)); fi
}
section() { printf '\n=== %s\n' "$1"; }

q()    { psql -v ON_ERROR_STOP=1 -h "$PGHOST_" -U "$PGUSER_" -d "$PGDB_" -tA -c "$1"; }
kb()   { curl -s --max-time 25 -X POST "http://127.0.0.1:$KB_PORT/v1/actions/$1" \
              -H 'Content-Type: application/json' -d "$2"; }
srv()  { curl -s --max-time 25 --unix-socket "$SOCK" -X POST "http://localhost$1" \
              -H 'Content-Type: application/json' -d "$2"; }
# ids returned by a fusion-enabled recall, comma-joined and sorted
recall_ids() {
  kb memory.find_facts "{\"query\":\"$1\",\"limit\":10,\"graph_code_fusion_state\":\"on\"}" |
    python3 -c 'import sys,json
d=json.load(sys.stdin); print(",".join(sorted(str(f.get("id")) for f in d.get("facts",[]))))'
}
# Run a real maintenance cycle with PRUNE (bit 2 = 4). That is the mode whose
# block calls memory_run_maintenance, and therefore the two section 5 jobs.
#
# Deliberately the KB action, NOT the server's memory_maintain MCP tool. That
# tool is a model-facing door which strips prune unconditionally ("the prune
# mode permanently deletes memories ... it is an operator action"), so driving
# the lifecycle through it runs an empty cycle and every assertion below then
# reads as a product failure. memory.maintenance_run is the operator path, and
# is what `aimee memory maintain` calls underneath.
maintain() {
  kb memory.maintenance_run '{"modes":4,"force":true,"dry_run":false}' |
    python3 -c 'import sys,json
d=json.load(sys.stdin).get("summary", {})
print("%s,%s"%(d.get("promoted"),d.get("expired")))'
}
# These tests deliberately seed pre-mutation-era rows so they can exercise the
# public recall, maintenance and correction paths.  Current databases reject raw
# semantic writes.  Suspend every user trigger in one transaction (matching the
# upgrade-fixture strategy in scripts/validation/fact-authority/seed-facts.sh),
# then restore the guards before commit.  A failed statement aborts the
# transaction, so a fixture failure cannot leave the guards disabled.
fixture_edges_sql() { # fixture_edges_sql <SQL touching semantic entity_edges>
  q "BEGIN;
     ALTER TABLE entity_edges DISABLE TRIGGER USER;
     $1
     ALTER TABLE entity_edges ENABLE TRIGGER USER;
     COMMIT;"
}
seed_fact() { # seed_fact src rel tgt class conf evidence-count asserted
  local rank=10 lifecycle=candidate
  if [ "$4" = A ]; then rank=30; lifecycle=persistent; fi
  fixture_edges_sql "WITH assertion AS (
       INSERT INTO entity_edges
         (source,relation,target,weight,relation_id,edge_class,confidence_class,
          confidence,asserted_at,superseded_at,invalidated_at,suppressed,
          authority_rank,lifecycle_state)
       VALUES ('$1','$2','$3',$6,
               (SELECT id FROM rel_types WHERE rel_type='$2' LIMIT 1),
               'semantic','$4',$5,'$7','','',0,$rank,'$lifecycle')
       RETURNING id
     )
     INSERT INTO fact_evidence
       (assertion_id,source_kind,source_id,observed_at,stance)
     SELECT id,'e2e-fixture','$1:$2:$3:' || n,'$7','supports'
       FROM assertion CROSS JOIN generate_series(1,$6) AS n;" >/dev/null || {
    echo "failed to seed fact: $1 $2 $3" >&2
    exit 1
  }
}
scrub() {
  q "DELETE FROM memory_entities WHERE entity LIKE 'e2e-%';
     DELETE FROM memories WHERE key LIKE 'e2e-%';" >/dev/null
  fixture_edges_sql "DELETE FROM entity_edges
       WHERE source LIKE 'e2e-%' OR target LIKE 'e2e-%';" >/dev/null
  q "DELETE FROM memory_rejection_tombstones
       WHERE object_kind='fact' AND (source LIKE 'e2e-%' OR target LIKE 'e2e-%');" >/dev/null
}

stop_stack() {
  pkill -f aimee-server 2>/dev/null
  pkill -f aimee-kb 2>/dev/null
  for p in $MOD_PIDS; do
    kill "$p" 2>/dev/null
    wait "$p" 2>/dev/null
  done
  MOD_PIDS=""
}

cleanup() {
  stop_stack
  rm -rf "$MODULE_BUNDLE"
}
trap cleanup EXIT INT TERM

prepare_config_modules() {
  if [ ! -x "$CONFIG_MODULE" ]; then
    echo "missing config module: $CONFIG_MODULE" >&2
    echo "build it with: make -C $AIMEE_SRC build/obj/aimee-module-config" >&2
    return 1
  fi

  rm -rf "$MODULE_BUNDLE"
  python3 "$AIMEE_ROOT/scripts/export_c_repositories.py" \
    --runtime-bundle "$MODULE_BUNDLE" >/dev/null || return 1
  mkdir -p "$AIMEE_HOME/modules.d/kb" "$AIMEE_HOME/modules.d/server"
  sed "s|^executable=.*|executable=$CONFIG_MODULE|" \
    "$MODULE_BUNDLE/grants/kb/config.grant" > "$AIMEE_HOME/modules.d/kb/config.grant"
  sed "s|^executable=.*|executable=$CONFIG_MODULE|" \
    "$MODULE_BUNDLE/grants/server/config.grant" > "$AIMEE_HOME/modules.d/server/config.grant"
  chmod 0600 "$AIMEE_HOME/modules.d/kb/config.grant" \
    "$AIMEE_HOME/modules.d/server/config.grant"
}

arm_config_module() { # socket placement log
  local bus="$1" placement="$2" log="$3"
  (
    local deadline=$((SECONDS + 30))
    while (( SECONDS < deadline )); do
      if [ -S "$bus" ]; then
        exec env AIMEE_HOME="$AIMEE_HOME" \
          AIMEE_MODULE_POLICY_DIR="$AIMEE_HOME/modules.d/$placement" \
          "$CONFIG_MODULE" "$bus"
      fi
      sleep 0.1
    done
    echo "config module: bus socket never appeared: $bus" >&2
  ) >"$log" 2>&1 &
  MOD_PIDS="$MOD_PIDS $!"
}

start_stack() { # start_stack [extra-yaml]
  stop_stack
  sleep 2
  rm -f "$SOCK" "$AIMEE_HOME/kb-module-bus.sock" "$AIMEE_HOME/server-module-bus.sock"
  mkdir -p "$AIMEE_HOME"
  { echo "db2_url: \"$AIMEE_DB2_URL\""
    echo 'kb_mode: "local"'
    echo "kb_client_url: \"http://127.0.0.1:$KB_PORT\""
    echo 'embedder_dims: 768'
    echo "${1:-typed_facts_enabled: true}"; } > "$AIMEE_HOME/aimee.yaml"
  prepare_config_modules || return 1
  arm_config_module "$AIMEE_HOME/kb-module-bus.sock" kb /tmp/typed-facts-kb-config.log
  arm_config_module "$AIMEE_HOME/server-module-bus.sock" server /tmp/typed-facts-server-config.log
  cd "$AIMEE_ROOT"
  nohup ./aimee-kb --http-port="$KB_PORT" > /tmp/kb.log 2>&1 &
  sleep 22
  nohup ./aimee-server > /tmp/server.log 2>&1 &
  for _ in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 1; done
  sleep 3
}

########################################################################
start_stack
section "0. harness liveness (a red result below means nothing until these pass)"
pgrep -f aimee-kb >/dev/null && K=up || K=down
check "aimee-kb running"      up "$K"
[ -S "$SOCK" ] && S=up || S=down
check "server socket present" up "$S"
[ "$K$S" = "upup" ] || { echo "--- kb.log:"; tail -20 /tmp/kb.log; echo "--- server.log:"; tail -20 /tmp/server.log; exit 1; }
check "kb answers a known-good read" ok \
  "$(kb memory.stats '{}' | python3 -c 'import sys,json; print(json.load(sys.stdin).get("status"))')"
check "server answers a known-good op" ok \
  "$(srv /v1/mcp/call '{"tool":"memory_maintain","arguments":{"modes":"prune","dry_run":true}}' |
     python3 -c 'import sys,json; print(json.load(sys.stdin).get("status"))')"

########################################################################
section "A. the recall walk admits typed facts (the change under test)"
scrub
A=$(q "INSERT INTO memories (tier,kind,key,content,confidence,created_at,updated_at)
       VALUES ('L2','fact','e2e-a','zorblex is the project codename',0.9,
               '2026-01-01T00:00:00Z','2026-01-01T00:00:00Z') RETURNING id" | head -1)
B=$(q "INSERT INTO memories (tier,kind,key,content,confidence,created_at,updated_at)
       VALUES ('L2','fact','e2e-b','the deployment runs in frankfurt',0.9,
               '2026-01-01T00:00:00Z','2026-01-01T00:00:00Z') RETURNING id" | head -1)
q "INSERT INTO memory_entities (memory_id,entity,role,weight)
   VALUES ($A,'e2e-alice','mention',1.0), ($B,'e2e-acme','mention',1.0);" >/dev/null

check "no edge: only the lexical hit"            "$A"        "$(recall_ids zorblex)"
seed_fact e2e-alice works_for e2e-acme A 1.0 1 '2026-01-01T00:00:00Z'
check "semantic edge bridges to the 2nd memory"  "$A,$B"     "$(recall_ids zorblex)"
fixture_edges_sql "UPDATE entity_edges SET lifecycle_state='superseded',
  superseded_at='2026-08-01T00:00:00Z' WHERE source='e2e-alice';" >/dev/null
check "superseded fact leaves the walk"          "$A"        "$(recall_ids zorblex)"
fixture_edges_sql "UPDATE entity_edges SET lifecycle_state='persistent',
  superseded_at='', suppressed=1 WHERE source='e2e-alice';" >/dev/null
check "tombstoned fact leaves the walk"          "$A"        "$(recall_ids zorblex)"
fixture_edges_sql "UPDATE entity_edges SET suppressed=0 WHERE source='e2e-alice';" >/dev/null
check "restored fact returns to the walk"        "$A,$B"     "$(recall_ids zorblex)"

# Regression: co-occurrence edges must still traverse. The change adds a
# population, it must not remove one.
fixture_edges_sql "DELETE FROM entity_edges WHERE source='e2e-alice';" >/dev/null
q "INSERT INTO entity_edges (source,relation,target,weight,edge_class)
   VALUES ('e2e-alice','co_discussed','e2e-acme',3,'cooccurrence');" >/dev/null
check "co-occurrence edge still bridges"         "$A,$B"     "$(recall_ids zorblex)"

########################################################################
section "B. listing surfaces still exclude typed facts (R1-A1 result boundary)"
scrub
seed_fact e2e-zoe works_for e2e-initech A 1.0 1 '2026-01-01T00:00:00Z'
q "INSERT INTO entity_edges (source,relation,target,weight,edge_class)
   VALUES ('e2e-zoe','co_discussed','e2e-quux',3,'cooccurrence');" >/dev/null
EXPLAIN_RELS=$(srv /v1/graph/explain '{"entity":"e2e-zoe"}' |
  python3 -c 'import sys,json
d=json.load(sys.stdin)
rows=d.get("edges") or d.get("rows") or []
print(",".join(sorted(r.get("relation","") for r in rows)))' 2>/dev/null)
check "graph.explain shows co-occurrence only"   "co_discussed" "$EXPLAIN_RELS"

########################################################################
section "C. §5 lifecycle jobs fire in a real maintenance cycle"
scrub
seed_fact e2e-spec      frobnicates e2e-thing C 0.4 1 '2000-01-01T00:00:00Z'
seed_fact e2e-spec-conf wibbles     e2e-y     C 0.4 2 '2000-01-01T00:00:00Z'
seed_fact e2e-durable   works_for   e2e-ecorp B 0.6 5 '2000-01-01T00:00:00Z'
seed_fact e2e-classa    works_for   e2e-corp  A 1.0 1 '2026-01-01T00:00:00Z'
COUNTS=$(maintain)
check "cycle reports promoted,expired"           "1,1" "$COUNTS"
check "aged unconfirmed C is invalidated"        "t"   "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-spec'")"
check "confirmed C survives"                     "f"   "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-spec-conf'")"
check "class B promoted to durable"              "0.8" "$(q "SELECT confidence FROM entity_edges WHERE source='e2e-durable'")"
check "class A untouched"                        "1"   "$(q "SELECT confidence FROM entity_edges WHERE source='e2e-classa'")"
check "all typed facts survive the orphan prune" "4"   "$(q "SELECT count(*) FROM entity_edges WHERE source LIKE 'e2e-%'")"

########################################################################
section "D. weight is a confirmation count, not a co-occurrence tally"
scrub
seed_fact e2e-w-a works_for e2e-w-corp  A 1.0 1 '2026-01-01T00:00:00Z'
seed_fact e2e-w-b works_for e2e-w-ecorp B 0.6 5 '2000-01-01T00:00:00Z'
# A heavy co-occurrence edge on the same relation, anchored to a memory so the
# orphan prune cannot delete it before normalisation runs.
W=$(q "INSERT INTO memories (tier,kind,key,content,confidence,created_at,updated_at)
       VALUES ('L2','fact','e2e-wmem','anchor for e2e-w-noise and e2e-w-heavy',0.9,
               pg_now_text(),pg_now_text()) RETURNING id" | head -1)
q "INSERT INTO memory_entities (memory_id,entity,role,weight) VALUES ($W,'e2e-w-heavy','mention',1.0);
   INSERT INTO entity_edges (source,relation,target,weight,edge_class)
   VALUES ('e2e-w-heavy','works_for','e2e-w-noise',50,'cooccurrence');" >/dev/null
maintain >/dev/null
check "class A keeps its single confirmation"    "1"   "$(q "SELECT weight FROM entity_edges WHERE source='e2e-w-a'")"
check "class B keeps its five confirmations"     "5"   "$(q "SELECT weight FROM entity_edges WHERE source='e2e-w-b'")"
check "co-occurrence IS still normalised to 100" "100" "$(q "SELECT weight FROM entity_edges WHERE source='e2e-w-heavy'")"

# A co-occurrence observation on a fact's triple must not count as a confirmation.
scrub
seed_fact e2e-share works_for e2e-share-t B 0.6 1 '2026-01-01T00:00:00Z'
q "INSERT INTO entity_edges (source,relation,target,weight) VALUES ('e2e-share','works_for','e2e-share-t',1)
   ON CONFLICT (source,relation,target) DO UPDATE SET weight = entity_edges.weight + 1
   WHERE entity_edges.edge_class <> 'semantic';" >/dev/null
check "co-occurrence upsert does not bump a fact" "1"  "$(q "SELECT weight FROM entity_edges WHERE source='e2e-share'")"
check "the row is still the typed fact"     "semantic"  "$(q "SELECT edge_class FROM entity_edges WHERE source='e2e-share'")"

########################################################################
section "E. the correction surface"
scrub
seed_fact e2e-r-b works_for e2e-r-corp B 0.6 1 '2026-01-01T00:00:00Z'
check "retract reports one edge"    '{"status":"ok","retracted":1}' \
  "$(srv /v1/facts/retract '{"source":"e2e-r-b","relation":"works_for","target":"e2e-r-corp","authority":"user"}')"
check "retract retains the row"     "1"   "$(q "SELECT count(*) FROM entity_edges WHERE source='e2e-r-b'")"
check "retract is idempotent"       '{"status":"ok","retracted":0}' \
  "$(srv /v1/facts/retract '{"source":"e2e-r-b","relation":"works_for","target":"e2e-r-corp","authority":"user"}')"

seed_fact e2e-r-a works_for e2e-r-acme A 1.0 1 '2026-01-01T00:00:00Z'
srv /v1/facts/retract '{"source":"e2e-r-a","relation":"works_for","target":"e2e-r-acme","authority":"model"}' >/dev/null
check "model may not retract class A" "f"  "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-r-a'")"
# This harness intentionally uses a plaintext server -> KB hop.  The body may
# request user authority, but without an authenticated human principal the KB
# must downgrade it to model authority rather than trust a self-declaration.
check "anonymous request cannot escalate to user authority" '{"status":"ok","retracted":0}' \
  "$(srv /v1/facts/retract '{"source":"e2e-r-a","relation":"works_for","target":"e2e-r-acme","authority":"user"}')"
check "class A remains after refused escalation" "f"  \
  "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-r-a'")"

# target omitted -> retract every current value of (source, relation)
scrub
seed_fact e2e-multi has_role e2e-role1 B 0.6 1 '2026-01-01T00:00:00Z'
seed_fact e2e-multi has_role e2e-role2 B 0.6 1 '2026-01-01T00:00:00Z'
check "omitting target retracts all values" '{"status":"ok","retracted":2}' \
  "$(srv /v1/facts/retract '{"source":"e2e-multi","relation":"has_role","authority":"user"}')"

# immutable relation: refused for model, allowed for user (born_in is immutable)
scrub
seed_fact e2e-imm born_in e2e-berlin B 0.6 1 '2026-01-01T00:00:00Z'
IMM=$(srv /v1/facts/retract '{"source":"e2e-imm","relation":"born_in","target":"e2e-berlin","authority":"model"}' |
      python3 -c 'import sys,json; print(json.load(sys.stdin).get("kind",""))')
check "immutable relation refuses a model retraction" "invalid_argument" "$IMM"
check "immutable fact untouched"                      "f" "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-imm'")"
IMM=$(srv /v1/facts/retract '{"source":"e2e-imm","relation":"born_in","target":"e2e-berlin","authority":"user"}' |
      python3 -c 'import sys,json; print(json.load(sys.stdin).get("kind",""))')
check "anonymous user claim cannot override immutable" "invalid_argument" "$IMM"
check "immutable fact remains after refused escalation" "f" \
  "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-imm'")"

# Corrections are retained and auditable rather than physically deleted.
scrub
seed_fact e2e-aka also_known_as e2e-alias B 0.6 1 '2026-01-01T00:00:00Z'
srv /v1/facts/retract '{"source":"e2e-aka","relation":"also_known_as","target":"e2e-alias","authority":"user"}' >/dev/null
check "correction invalidates the assertion" "t" "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-aka'")"
check "correction still RETAINS the row"     "1" "$(q "SELECT count(*) FROM entity_edges WHERE source='e2e-aka'")"

# entity merge / unmerge
q "DELETE FROM entity_merges WHERE from_id IN (SELECT canonical_id FROM entity_registry WHERE kind=5);
   DELETE FROM entity_registry WHERE kind=5;" >/dev/null
F=$(q "INSERT INTO entity_registry (kind,status) VALUES (5,'active') RETURNING canonical_id" | head -1)
T=$(q "INSERT INTO entity_registry (kind,status) VALUES (5,'active') RETURNING canonical_id" | head -1)
MID=$(srv /v1/entities/merge "{\"from_id\":$F,\"into_id\":$T}" |
      python3 -c 'import sys,json; print(json.load(sys.stdin).get("merge_id",""))')
check "merge returns an audit id"    "yes" "$([ -n "$MID" ] && [ "$MID" != "None" ] && echo yes || echo no)"
check "merged entity is marked"      "merged" "$(q "SELECT status FROM entity_registry WHERE canonical_id=$F")"
srv /v1/entities/unmerge "{\"merge_id\":$MID}" >/dev/null
check "unmerge restores active"      "active" "$(q "SELECT status FROM entity_registry WHERE canonical_id=$F")"
check "double unmerge is refused"    "not_found" \
  "$(srv /v1/entities/unmerge "{\"merge_id\":$MID}" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("kind",""))')"

# argument validation
check "retract needs a source"  "invalid_argument" \
  "$(srv /v1/facts/retract '{"relation":"works_for"}' | python3 -c 'import sys,json; print(json.load(sys.stdin).get("kind",""))')"
check "authority is closed"     "invalid_argument" \
  "$(srv /v1/facts/retract '{"source":"x","relation":"y","authority":"nonsense"}' | python3 -c 'import sys,json; print(json.load(sys.stdin).get("kind",""))')"
check "merge rejects self-merge" "invalid_argument" \
  "$(srv /v1/entities/merge '{"from_id":5,"into_id":5}' | python3 -c 'import sys,json; print(json.load(sys.stdin).get("kind",""))')"

########################################################################
section "F. PII relations participate in the walk (the §7 decision)"
# Decision on record: the PII gate stays at INJECTION, so a sensitive edge may
# influence ranking without the fact itself being rendered. 'lives_in' is a
# seeded pii relation.
scrub
P=$(q "INSERT INTO memories (tier,kind,key,content,confidence,created_at,updated_at)
       VALUES ('L2','fact','e2e-p','quibblewick is the seed term',0.9,
               '2026-01-01T00:00:00Z','2026-01-01T00:00:00Z') RETURNING id" | head -1)
P2=$(q "INSERT INTO memories (tier,kind,key,content,confidence,created_at,updated_at)
        VALUES ('L2','fact','e2e-p2','unrelated content here',0.9,
                '2026-01-01T00:00:00Z','2026-01-01T00:00:00Z') RETURNING id" | head -1)
q "INSERT INTO memory_entities (memory_id,entity,role,weight)
   VALUES ($P,'e2e-person','mention',1.0), ($P2,'e2e-city','mention',1.0);" >/dev/null
seed_fact e2e-person lives_in e2e-city A 1.0 1 '2026-01-01T00:00:00Z'
check "a pii-tier fact still bridges the walk" "$P,$P2" "$(recall_ids quibblewick)"

########################################################################
section "G. lifecycle runs even with typed_facts_enabled=false"
# The master gate is retired and the typed-fact layer is unconditional.  A stale
# deployment config must not resurrect the old behaviour or freeze candidates.
scrub
seed_fact e2e-gate frobnicates e2e-gt C 0.4 1 '2000-01-01T00:00:00Z'
start_stack "typed_facts_enabled: false"
maintain >/dev/null
check "class C still expires with the gate off" "t" \
  "$(q "SELECT (invalidated_at<>'') FROM entity_edges WHERE source='e2e-gate'")"

########################################################################
scrub
stop_stack
printf '\n===============================\n  PASS: %d   FAIL: %d\n===============================\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
