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

Verified by reading the build and the entrypoint: the binary is built, launched,
given no `--db`, and defaults to the module's path; the store opens that path;
the tables are the ones listed above.

NOT verified: the two processes writing the same row concurrently under load.
That would want a running appliance, and the claim here does not depend on it --
the point is that a second writer exists, not how often it collides.
