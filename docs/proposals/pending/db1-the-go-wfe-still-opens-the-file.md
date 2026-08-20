# Proposal: the Go WFE opens DB1 directly, and it is now the last one that does

- **State:** OPEN. The C migration is finished; this is the other half nobody
  measured, because the surface that was measured was a count of C call sites.

## What is true

`aimee-server` no longer opens DB1 — the symbol is not in the binary. One
process in a deployed appliance still does, and it is not the module:

- `Dockerfile.server` builds `server-go/cmd/aimee-server` as `/out/aimee-wfe`.
- `server-entrypoint.sh` defaults `AIMEE_WFE_ENGINE=go` and rejects anything
  else ("WFE is Go-only"), then launches `aimee-wfe --home "$AIMEE_HOME" ...`
  with no `--db`, so its `dbPath` falls back to `$home/aimee.db`.
- That is the same file `AIMEE_DB1_PATH` gives the module, defaulted in the same
  script a few lines earlier.
- `server-go/internal/db1/store.go` opens it with
  `sql.Open("sqlite", …_txlock=immediate)` and exposes 47 methods.

The tables it reads and writes are `lifecycle_work_item`, `lifecycle_event`,
`lifecycle_stage_attempt`, `lifecycle_delegate_job`, `agent_jobs` and
`wfe_convergence` -- which is to say, precisely the tables the `lifecycle` and
`delegation` families now serve.

## Why this is not a regression

Nothing here changed. The Go WFE has always opened the file, and SQLite in WAL
mode makes concurrent process access safe at the storage level: this is not
corruption waiting to happen.

What it does mean is that the doctrine's claim -- all state behind the module --
is true of C and not yet true of the appliance. Two implementations of the same
tables run side by side, and the atomicity the C side gained does not extend
across the Go writer. `db1_work_item_record_outcome` applies a step's whole
outcome in one transaction; a Go writer touching `lifecycle_work_item` between
that transaction's statements sees a consistent database, but the two engines'
notions of "a step is applied" are enforced independently rather than jointly.

## Why it was invisible

`db1-as-a-go-module.md` measured the surface as "63 C translation units linked
into aimee-server" and "2,888 call sites outside src/db1", all of them C. A Go
service with its own SQLite handle is not a call site in that count, so the
proposal that scoped this migration could not see it, and neither could any of
the sweeps I ran -- until I stopped grepping C and asked which processes open
the file.

## Which module owns workflow state?

Before costing the work, there is a question underneath it that decides which
work to do, and getting it wrong means doing a large migration twice.

`aimee-wfe` is not the daemon. It is a separate process, launched by the same
supervisor, with its own grant and its own principal -- which is to say it is a
module by every structural test this codebase applies. The doctrine says state
lives in modules, and the WFE is one. The defect is not that a non-C process
holds state; it is that **two modules claim the same tables**, and they resolve
that claim by sharing a file rather than by one of them owning it.

So there are two ways to close this, and they are not variations on each other:

**DB1 owns it; the WFE becomes a client.** Port `internal/db1/store.go` onto the
Go bus client. 1738 lines, ~40 live methods, **18 transactions** and **17
recursive CTEs**. Every one of those transactions has to become a single module
operation -- the same redesign `wfe_engine.c` needed when its sixteen writes
across two transactions became `db1_work_item_record_outcome` -- and every
recursive CTE has to move into C. The methods at risk are the ones that matter:
budget reservation with leases, cancellation trees, orphan reconciliation.

**The WFE owns it; the daemon becomes a client.** Remove `lifecycle` and
`delegation` from DB1 -- they were migrated *into* it on this branch -- give the
WFE its own store, and point the daemon's work-item reads at the WFE over the
bus. That is **~106 C call sites**: 45 in `wfe_autonomy.c`, 35 in
`server_workflow_api.c`, and the rest across `server_ci_route.c`,
`trigger_scheduler.c`, `server_dev_submit.c`, `cmd_hooks.c`,
`router_advise.c` and `s2_native_gate_hook.c`.

Neither is cheap and both are mechanical-with-sharp-edges. The first keeps the
store single and makes the execution engine a client of it. The second follows
the grain of "the service that owns a domain owns its state", and would mean
this branch migrated two families into the wrong module.

**The first, on the codebase's own precedent.** `server-go/db1/client.go`
already exists for exactly this shape: a Go module that needs DB1 state and
reaches it over the bus rather than through the file. It carries two methods
today, `LoadState` and `SaveState`, because the economizer's keyed blob was the
first Go client DB1 had. That is the pattern this repo already chose for Go
modules needing DB1 state, and the WFE is the next one, not an exception to it.
DB1 is the state service; modules are its clients; a module that opens the state
service's file instead of calling it is the anomaly regardless of which language
it is written in.

The second direction would also undo deliberate work: `lifecycle` and
`delegation` were migrated into DB1 on this branch precisely so the daemon's
API routes could read work items through the module rather than through a
handle of their own. Splitting them back out to make the WFE self-sufficient
trades one shared file for one shared domain across two stores, which is the
same problem with an extra hop.

So: grow `server-go/db1/client.go` to cover lifecycle and delegation, and move
`internal/db1/store.go` onto it. The contract those methods are written against
is settled -- 19 families, 425 operations -- which is what makes this a port
rather than a design. The 18 transactions are the work: each becomes one
operation, the way `db1_work_item_record_outcome` did, and that is a redesign of
the Go engine's write path rather than a translation of it. That is why it is
the next project and not the tail of this one.

Everything measured above holds either way: today the file has two owners with
two schema authorities, and it is not corrupting anything.

## What closing it costs

Not small, and not this change. `server-go/db1/client.go` -- the Go bus client
`cmd/aimee-module` already uses -- exposes two methods, `LoadState` and
`SaveState`, the economizer keyed-blob pair. `server-go/internal/db1/store.go`
exposes 47. Closing the gap means growing the Go client to cover the lifecycle
and delegation families and moving the WFE onto it.

That is the phase the original proposal already named: "the boundary moves
FIRST, with the existing C behind it; the language port follows, per domain,
against a contract that is already settled." The contract is settled now -- 19
families, 425 operations -- which is exactly what such a port would be written
against. It is the next project, not the tail of this one.

## What was verified, and what was not

Originally: verified by reading the build and the entrypoint, with the
concurrent-writer behaviour explicitly NOT verified, on the grounds that the
claim did not depend on it.

It has since been measured. `scripts/validation/db1-module-wfe-coexistence.sh`
runs the three-process topology on a clean container -- C daemon, DB1 module,
and `aimee-wfe` started the way `server-entrypoint.sh` starts it, with the
`wfe.grant` the bundle generates. Results:

**Two processes hold the store.** With all three up, `/proc/*/fd` shows
`aimee-module-db` and `aimee-wfe` both holding `aimee.db`. This is the doctrine
claim failing in the shipped configuration, stated as a measurement rather than
an inference: the module is not the store's sole owner in the appliance.

**The Go side amends the module's schema.** Starting the WFE against a store the
module had already created took it from 102 tables to 105, and added five
columns to `lifecycle_work_item` -- `source_path`, `reserved_cost_usd`,
`reservation_state`, `reservation_owner`, `reservation_lease_until`. The C
module references none of the five, so this is additive rather than conflicting,
but the ALTER ladder that produced them belongs to a different codebase than the
one that created the table.

**It happens even when the WFE cannot work.** The first run had no `wfe.grant`
installed, so the WFE died on `bus: attach denied` -- after opening the store
and running its migrations. A WFE that fails to start still rewrites the schema.

**Either process will create the store.** Started first on an empty home, the Go
WFE creates seven tables on its own; the module then joins and completes the
schema to the same 105. The store works in both orders and
`lifecycle_work_item` ends up with the same column *set* -- but in a different
column *order*, which is a fair summary of the situation: two authorities, and
which one got there first is visible in the file afterwards.

**Contention did not bite at the rates tested.** Both sides are configured for
multi-process access deliberately -- the module uses `journal_mode=WAL` with
busy timeouts of 5s and 15s, the Go store uses WAL with a 5s busy timeout,
`MaxOpenConns(1)` and `_txlock=immediate`. `db1-module-write-contention.sh`
drives external writers against `lifecycle_work_item` while the module takes
writes through the daemon, and checks for lost rows, lock failures and
`PRAGMA integrity_check`. This is not evidence that no rate collides; it is
evidence that the arrangement is not corrupting anything at the rates a
validation script can produce.

So the finding stands and is now quantified rather than argued: the second
writer exists, it is live in the shipped topology, it shares schema authority,
and it is not currently breaking anything.
