# Proposal: what happens to a transaction that spans several calls

- **State:** OPEN, and narrower than it was written. This blocks ONE source,
  wfe_store.c, which now has a reserved family of its own ('lifecycle'). The
  other six sources it was written as blocking are migrated and served.

Five DB1 families had migrated by preserving each function's contract exactly,
so a mistake showed up as a link error or a failing assertion. This was written
as the first case where every signature fits the wire and the migration would
still be wrong.

That is true of wfe_store. It was not true of the family: this document said the
transaction reached db1_plan_step_set_status_output and therefore tied
execution_plans in, and that function does not appear in wfe_engine.c at all.
Every write inside both critical sections -- lifecycle_event_add,
stage_attempt_inc, work_item_abandon_children, _add_cost, _set_pause,
_set_pr_ref, _set_stage, _set_terminal -- is defined in wfe_store.c and nowhere
else. Checking that took one grep; asserting it cost six sources several months
of being described as blocked.

## The shape

`wfe_engine.c` wraps several separate DB1 writes in one transaction:

    if (db1_lifecycle_txn_begin() != 0) ...
    WFE_CKW(db1_work_item_add_cost(work_item_id, r.cost_usd));
    WFE_CKW(db1_plan_step_set_status_output(...));
    WFE_CKW(db1_work_item_set_stage(...));
    db1_lifecycle_txn_commit();

with `goto txn_fail` rolling back on any failure. Sixteen writes ride the two
transactions in that one function, and the comment above them is explicit about
why: "atomic critical section: cost + step outcome + state update."

`db1_lifecycle_txn_begin`, `_commit` and `_rollback` are declared in
wfe_store.h, so the catalog would have to serve them. Across the boundary each
becomes its own request, and the module would have to hold an open transaction
between requests — on a shared connection, behind the gate mutex that
`db1_txn_begin` takes. A caller that died between BEGIN and COMMIT would leave
that mutex held and every other caller blocked.

That is not a thing to declare and find out about later. It is the reason the
family stops here.

## Three ways out

**Make each critical section one operation.** `db1_work_item_pause_with_event`,
and something like `db1_work_item_record_step_outcome` for the larger one. The
transaction then lives entirely inside the module, which is what a module
boundary is for, and the engine loses its BEGIN/COMMIT scaffolding along with
the `goto txn_fail` paths.

This is the right end state and it is a redesign of a workflow engine's
correctness-critical write path. Getting it wrong leaves work items in states
that no single write produced -- a cost recorded without the step outcome that
justified it, or a stage advanced without the cost. That belongs to whoever
owns that engine, reviewing it as a change to the engine.

**Carry transactions on the wire.** A begin returns a handle, later calls quote
it, and the module times out an abandoned one. This keeps every signature and
moves the problem into the protocol: leases, crash recovery, and a module that
can be wedged by a caller that stops talking. It is a large addition for one
family's benefit.

**Leave wfe_store in the daemon.** Honest, and it means the daemon keeps a
direct DB1 connection, which is what the migration exists to remove.

## It is not only wfe_engine

`db1_conn()` is called in twenty places under `src/server` and
`src/modules/workflows`, and `BEGIN IMMEDIATE` is opened directly by pki.c (six
sites), server_mgmt_status.c, server_dev_submit.c and server_mgmt_jwks_cache.c.
Some of those tables have no family yet, so they are not blocked today -- but
they are the same shape, and each will reach this question when its family
does.

Three DB1 sources also live under `src/server/` rather than
`src/modules/db1/` -- server_mgmt_jwks_cache.c, server_http_mgmt_read_routes.c
and server_mgmt_audit.c. They are compiled into DB1_SRCS from there, which
means the "one family claims every source" rule has never seen them.

## What was not blocked by this, and is now served

All six: execution_trace, wfe_binding, pipelines, roadmap_runtime,
execution_plans and roundtable_pipeline. The friction was ordinary, and the
predictions here were right about it -- the allocated-array lists needed nothing
new, and `db1_execution_plan_create` took the delegate_learning answer, with the
caller serialising and the module parsing.

Four things did need the wire to say something new, and none of them was a
transaction:

- `negatives: data`, so db1_wfe_bind's -2 (single-writer refusal) is an answer
  rather than a status mapped to FAILED.
- `null_when_empty`, so db1_roundtable_run_list's NULL filter still means
  "every non-terminal run" instead of "state equals the empty string".
- nested repeats, so a plan's 32 steps could each carry their own dependency
  array.
- c_returns int64 on six execution_plans writes that answer how many rows they
  changed, plus one already-served function with the same shape.

## What is actually left

wfe_store.c alone: 34 functions, sixteen of whose writes ride two transactions
that wfe_engine.c opens and commits across separate calls. The three ways out
below still stand, and the first is still the right one -- with the correction
that making each critical section a single operation is a change to
wfe_engine.c and wfe_store.c only, not to the six sources listed above.
