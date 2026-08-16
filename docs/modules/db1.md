# db1 module

## Purpose and non-goals

`db1` owns the server's SQLite store. It is a module because a store is state, and
the module doctrine puts state behind the event bus rather than in a library every
component links.

This is the boundary, not the port. The implementation is the same C that lived at
`src/db1`, now serving a stage; callers cross the bus one domain at a time and the
language changes afterwards, against a contract already settled. The three phases
and the measured surface are written up in the `db1-as-a-go-module` proposal.

It does not own DB2, which is the KB's store reached over typed `/v1` HTTP, and it
does not own schema migration policy, which stays with the server's bootstrap.

## Public contracts

The process serves principal 30 on the server bus. The wire contract is `src/modules/db1/db1_module_api.h`. It is deliberately
PRIVATE rather than published under `include/aimee/db1/`: a module with a public
include root must retire its flat one, and db1's flat root is what resolves the
bare `#include "db1.h"` in several hundred files. The contract becomes public
when Phase B has moved those callers, not before.

| Stage | Event kind | Owns |
| --- | --- | --- |
| 1 `db1-economizer-state` | `11777` | the economizer's per-conversation reducer state |
| 2 `db1-git-ownership` | `11778` | which session owns which branch, for the MCP git flows |

Stage 1 is first because it has exactly one production caller, so it proves the
boundary without a wide cutover. Its callers now reach it over the bus: the
economizer loads and saves its own reducer state and no longer has that blob
carried in and back out by the seam.

The remaining domains are declared as reserved families in
`src/modules/db1/eventcontract/operations.json`, the same shape DB2 uses. A
family owns one event kind and its operations dispatch on an op id inside the
payload, so DB1 needs one stage per domain rather than one per call -- roughly
sixty domains against a ceiling of 255.

Reserving the kinds up front is what keeps the numbering stable: a family
activated later answers the kind it was always going to answer, so a migration
that has already shipped cannot be renumbered by one that follows. Each
reservation names the sources it will cover, which makes the outstanding work
countable from the catalog instead of rediscovered each time. Regrouping or
renaming a RESERVED family is free; an active one is a contract callers already
speak. `scripts/gen_db1_contract.py` enforces all of that, and refuses wire
constants for a family nothing serves yet.

| Family | Event kind | State |
| --- | --- | --- |
| 1 `economizer_state` | `11777` | active |
| 2 `git_ownership` | `11778` | active |
| 3 `sessions` | `11779` | reserved |
| 4 `agent_work` | `11780` | reserved |
| 5 `delegation` | `11781` | reserved |
| 6 `workflow` | `11782` | reserved |
| 7 `conversation` | `11783` | reserved |
| 8 `identity` | `11784` | reserved |
| 9 `telemetry` | `11785` | reserved |
| 10 `runtime` | `11786` | reserved |

## Dependencies and consumers

- `config` supplies the database path and the store's runtime settings.
- `module-runtime` authenticates the executable, UID, principal and event-kind
  grant on the server bus, and owns attach, deadlines and cancellation.

Most consumers still reach DB1 by including its headers rather than over the bus.
Those crossings are declared in `scripts/check_module_bus_boundary.py` under
`PRIVATE_HEADER_REACH`, `FLAT_ROOT_REACH` and `CORE_LINKED_REACH`. The size of
those blocks is the honest measure of how far the migration has to go.

## Providers and readiness

The store is ready when the process has attached and opened its database. An
unreachable module answers `CAPABILITY_ABSENT`, which a caller treats the same way
it treats any absent module: it proceeds without the value rather than failing.

## Configuration and activation

- `runtime_toggle.supported`: `false`. A deployment without its store is not a
  smaller deployment, it is a broken one, so db1 is required and not
  operator-toggleable. Activation is the presence of the module process.

Choosing that classification cost nothing, because a principal ref is declared
rather than derived from inventory position, so a required module no longer
renumbers every module after it.

## Surfaces

One bus stage, `db1-economizer-state`. No HTTP surface, no CLI and no socket of
its own: the host takes a bus socket path as its only argument and the shared
runtime owns the connection. The SQLite file is the only other surface, and this
process becomes its sole opener once callers have moved across.

## Data and migrations

Owns `schema.sql` and `roadmap_runtime.sql` in its own directory. Schema creation
runs at server bootstrap, which is deliberately not this module's job while the
implementation is still linked into the server.

## Security and privacy

The store holds session state, checkpoints, token audit rows and delegate
records. `src/headers/db1_optional.h` is the seam that lets DB2-only binaries such
as `aimee-kb` avoid linking DB1 at all, using weak references under
`AIMEE_DB1_DISABLED`.

That seam does not survive the bus cutover unchanged: "absent because unlinked"
becomes "absent because unreachable", and the guard must be carried across rather
than dropped, or a kb-hosted MCP plugin silently loses its OSV supply-chain check.

## Supported journeys

A gateway turn loads its reducer state before reducing and saves it afterwards, so
the fold's freeze boundary survives between turns. A load miss is reported as
`MISSING` rather than an error, because the first turn of a conversation has no
state and the caller should start cold rather than fail.

## Tests and failure behavior

The stage's request parsing is covered by `src/tests/test_db1_module_stage.c`: a
truncated field, an over-long value and an unknown opcode are each rejected rather
than guessed at, and a save round-trips through a load.

Failures are reported in the response status rather than as bus errors, so a
caller distinguishes "no state" from "the store refused" without inspecting
transport.

## Operational diagnostics

The module logs attach and shutdown through the shared runtime. A caller seeing
`CAPABILITY_ABSENT` should check that the process is running and that its grant
lists event kind `11777`.

## Compatibility

The wire contract is versioned by its magic and length constants. Event kinds are
fixed by the process contract at `4096 + ref*256 + stage`, so they move only if the
declared ref moves, which it does not.

## Extension and removal

A new domain becomes a new stage with its own event kind, declared in
`src/modules/process-contracts.json` and advertised by the host. Removing the
module is not currently possible: it is required, and the ref it holds is retired
rather than reissued if it ever were.
