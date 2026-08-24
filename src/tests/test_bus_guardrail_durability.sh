#!/bin/sh
# The guardrail event's durability across the bus, verification half.
#
# THE PROPERTY: every guardrail event the bus accepts reaches the store exactly
# once, and a graceful stop drains those in flight.
#
# WHY THIS IS A SHELL TEST. The emitter cannot check its own work. It stops the
# bus -- that is half the property -- and the store is reached OVER that bus, so
# after the stop there is no transport left to ask through, and restarting it
# does not bring the module back. A verification that runs inside the process
# being verified would have to ask before the stop, which is the moment the
# claim is about.
#
# So the read-back happens here, straight out of PostgreSQL, after the emitter
# has exited and its modules with it. The rows outlive both.
#
# This replaces the retired in-process test of the same name, whose read-back
# was db1_guardrail_event_list() against an in-process SQLite database.
set -u

BIN=${BIN:-$(dirname "$0")/unit-test-bus-guardrail-durability-emit}
[ -x "$BIN" ] || BIN="$(dirname "$0")/../unit-test-bus-guardrail-durability-emit"

fail() { echo "FAIL: $*" >&2; exit 1; }

if [ -z "${AIMEE_STORE_URL:-}" ]; then
   echo "  SKIP: AIMEE_STORE_URL is unset; this needs the store module and a PostgreSQL"
   exit 0
fi
if ! command -v psql >/dev/null 2>&1; then
   # The verification is SQL and there is no way to fake it: a run without psql
   # would be this script reporting success having checked nothing.
   echo "  SKIP: psql is not installed, and the read-back is SQL by design"
   exit 0
fi
[ -x "$BIN" ] || fail "the emitter is missing: $BIN"

echo "test_bus_guardrail_durability:"

# The baseline, which is 0 on a database the emitter has not yet touched.
#
# ORDER MATTERS HERE and the first version got it backwards: it demanded the
# table before running the emitter, and on a fresh database the table does not
# exist yet -- the emitter's own startup is what applies the schema. So an
# absent table is a legitimate "nothing yet", and the check that it EXISTS
# happens after the run, where its absence really would be a failure.
BEFORE=$(psql "$AIMEE_STORE_URL" -tAc \
   "SELECT count(*) FROM guardrail_events" 2>/dev/null | tr -d ' ')
[ -n "$BEFORE" ] || BEFORE=0

OUT=$("$BIN" 2>&1) || { echo "$OUT"; fail "the emitter failed"; }
echo "$OUT" | sed 's/^/  /'

case "$OUT" in
   *"SKIP:"*) echo "  SKIP: the emitter skipped; nothing to verify"; exit 0 ;;
esac

WRITTEN=$(echo "$OUT" | sed -n 's/.*WRITTEN=\([0-9]*\).*/\1/p' | head -1)
[ -n "$WRITTEN" ] || fail "the emitter printed no WRITTEN= line"

AFTER=$(psql "$AIMEE_STORE_URL" -tAc \
   "SELECT count(*) FROM guardrail_events" 2>/dev/null | tr -d ' ')
# Now the table must be there. Before the emitter ran, its absence meant "fresh
# database"; after, it means the schema never applied and there is nothing to
# verify -- which must not read as a passing run of zero rows.
[ -n "$AFTER" ] || fail "guardrail_events does not exist after the run: the store never applied its schema"
DELTA=$((AFTER - BEFORE))

# 1. EXACTLY-ONCE, by count. Compared against what the BUS accepted, not against
#    the emitter's N: a run that legitimately dropped an event still has to
#    satisfy "what was accepted is what is stored".
if [ "$DELTA" -ne "$WRITTEN" ]; then
   fail "the store gained $DELTA rows but the bus counted $WRITTEN written -- an event was lost or double-counted across the stop"
fi
echo "  PASS  the store gained exactly $DELTA rows, matching what the bus wrote"

# 2. EXACTLY-ONCE, by identity. A loss and a duplicate that net to the same
#    total pass the count above and fail here, which is the whole reason each
#    event carries a unique session_id.
DUPES=$(psql "$AIMEE_STORE_URL" -tAc "
   SELECT count(*) FROM (
      SELECT session_id FROM guardrail_events
       WHERE session_id ~ '^s[0-9]+\$'
       GROUP BY session_id HAVING count(*) > 1
   ) d" 2>/dev/null | tr -d ' ')
[ "${DUPES:-0}" -eq 0 ] || fail "$DUPES identity(ies) appear more than once: a duplicate crossed"
echo "  PASS  no identity appears twice"

# 3. THE FIELDS SURVIVED THE WIRE. overall_risk was set to the event's own index
#    as a double, so this checks the typed value round-tripped rather than
#    arriving as text and being parsed back into something plausible.
BADRISK=$(psql "$AIMEE_STORE_URL" -tAc "
   SELECT count(*) FROM guardrail_events
    WHERE session_id ~ '^s[0-9]+\$'
      AND overall_risk IS DISTINCT FROM substring(session_id from 2)::double precision" 2>/dev/null | tr -d ' ')
[ "${BADRISK:-0}" -eq 0 ] || fail "$BADRISK row(s) have an overall_risk that does not match their identity"
echo "  PASS  overall_risk round-tripped as a double on every row"

# 4. And a text field, and the boolean, because those are the two the wire has
#    historically got wrong in opposite directions: a BOOLEAN read through atoi
#    is false on every row, including the ones whose default is true.
BADACTION=$(psql "$AIMEE_STORE_URL" -tAc "
   SELECT count(*) FROM guardrail_events
    WHERE session_id ~ '^s[0-9]+\$'
      AND final_action IS DISTINCT FROM
          CASE WHEN substring(session_id from 2)::int % 2 = 1 THEN 'block' ELSE 'allow' END" 2>/dev/null | tr -d ' ')
[ "${BADACTION:-0}" -eq 0 ] || fail "$BADACTION row(s) have the wrong final_action for their identity"
echo "  PASS  final_action round-tripped as text on every row"

BADDRY=$(psql "$AIMEE_STORE_URL" -tAc "
   SELECT count(*) FROM guardrail_events
    WHERE session_id ~ '^s[0-9]+\$'
      AND dry_run IS DISTINCT FROM (substring(session_id from 2)::int % 2 = 1)" 2>/dev/null | tr -d ' ')
[ "${BADDRY:-0}" -eq 0 ] || fail "$BADDRY row(s) have the wrong dry_run for their identity"
echo "  PASS  dry_run round-tripped as a boolean on every row"

echo "test_bus_guardrail_durability: OK ($WRITTEN events, exactly once, fields intact)"
