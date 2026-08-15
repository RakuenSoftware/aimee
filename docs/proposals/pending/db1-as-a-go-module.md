# Proposal: DB1 as a Go module on the event bus

- **State:** PENDING — scope only; no implementation has started.
- **Date:** 2026-08-15.
- **Charter roles:** Constrain-Verify / Gate-Promote.
- **Thesis:** DB1 is state, so by the module doctrine it belongs behind a module reached over
  the event bus rather than a C library every component links. The boundary moves FIRST, with
  the existing C behind it; the language port follows, per domain, against a contract that is
  already settled. This proposal measures that surface and names the constraints that decide
  the module's shape. It does not propose a schema change, a second store, or any new policy
  surface.

## 1. Why this exists

The doctrine is that C owns four things — the event bus, outside-world communication, the
HTTP surface, and the audit tap — and that **all logic and all state** live in Go modules.
DB1 is state. Today it is 63 C translation units linked into `aimee-server`, and callers
reach it by direct function call.

This came up concretely while moving the economizer gateway seam. After the verdict, the
breaker, the post-dispatch decision and the counters had all moved into the module, what
remained in C at that seam was the reducer state: `db1_economizer_state_load` before the
call and `db1_economizer_state_save` after it, plus a conversation fingerprint computed
from the loaded blob. That is not seam logic that resisted moving — it is DB1 access, and
it cannot move until DB1 does. **The gateway seam is blocked on this proposal**, and so is
every other seam whose last C residue is a load/save pair.

## 2. The measured surface

Taken from `origin/testing` at `2687`:

| Measure | Count |
| --- | --- |
| `src/db1` C and header lines | 22,160 |
| Exported `db1_*` symbols | 409 |
| Call sites outside `src/db1` | 2,888 |
| — of those, in `src/tests` | 1,975 |
| — in `src/server` | 360 |
| — in `src/modules` | 241 |
| — in `src/posix`, `src/kb`, `src/modules/db2/c`, platform | 46 |
| **Distinct symbols used in production** (non-test) | **242** |

The gap between 409 exported and 242 production-used is the first useful finding: **167
exported symbols are reached only by tests.** Whatever shape the module takes, that
difference should be resolved before porting, not carried across — either the tests are
exercising internals that need no module surface, or they are the only coverage of
functions nothing calls.

## 3. Four constraints that decide the shape

### 3.1 The stage ceiling is 255, and 242 is not headroom

Event kinds are fixed at `4096 + ordinal*256 + stage`, so a module has stage ids 1–255
before it collides with the next inventory ordinal. A one-stage-per-function module would
need 242 of them. It fits, and it is still the wrong answer: it leaves 13 slots for the
lifetime of the module, makes the process contract 242 entries long, and turns every new
query into an inventory change.

**The module must group.** The two candidate shapes:

- **Domain stages.** One stage per table family — sessions, turns, economizer state, token
  audit, workspace, and so on — each taking an operation discriminator in its request. Maybe
  15–25 stages. Keeps the contract readable and the grants meaningful, because a grant can
  say which domains a caller may reach.
- **One generic query stage.** A single stage taking a statement identifier and arguments.
  One contract entry, but the grant then says only "may talk to DB1", which is the whole
  database, and governance sees one event kind for every access.

Domain stages are the better fit for a system whose grants are per event kind, but the
boundary between domains is a real design question and is not settled here.

### 3.2 Modules build without cgo

`c-repositories.yml` runs `CGO_ENABLED=0 go test ./bus ./modules/... ./cmd/aimee-module`,
and the one existing database module, `server-go/modules/postgres`, uses `pgxpool` — pure
Go. A DB1 module must therefore use a **pure-Go SQLite driver** (`modernc.org/sqlite`), not
`mattn/go-sqlite3`. That is a dependency decision with its own consequences for
performance, WAL behaviour and busy-timeout semantics, and it should be validated against
DB1's actual concurrency before the port, not after.

### 3.3 There is no data-access precedent yet

`server-go/modules/postgres` is 168 lines serving a single health-probe stage. It proves a
Go module can open a pool and answer over the bus; it proves nothing about transactions,
prepared statements, migrations, or a 242-symbol surface. This will be the first real
data-access module, so the first slice should be chosen to establish that pattern rather
than to move the most code.

### 3.4 A module owns its sources, so the directory moves

`validate_module_descriptors` resolves each ownership field against a fixed root and rejects
anything outside it (`ownership-role-boundary`): `sources` and `private_headers` must live
under `src/modules/<id>/`, `public_headers` under `src/modules/<id>/include/aimee/<id>/`. No
module today owns a source outside those roots.

So db1 cannot become a module while its C sits in `src/db1/`. The move is mechanical and its
cost is bounded: the 587 include sites across 349 files are bare includes resolved by `-Idb1`,
so the build's include path changes and the call sites do not. What it is NOT is a boundary:
moving the directory leaves 349 files including db1 headers exactly as before. That is why
Phase A and Phase B are separate.

## 4. What has to be decided before any code

1. **Domain stages or one generic query stage** (§3.1).
2. **Who owns writes.** DB1 has one writer today because one process links it. Over a bus,
   the module is the writer and callers queue — which is stricter, and changes the failure
   mode of anything that assumed a synchronous local call.
3. **Transactions across calls.** Several call sites do read-modify-write. A module that
   holds a transaction open across bus calls is holding a caller's state, which the module
   doctrine forbids; so either those sequences become single stage calls that carry the
   whole operation, or they are identified and reshaped first.
4. **What happens to the 167 test-only symbols** (§2).
5. **Whether `aimee-kb` and the thin client are in scope.** The counts above show a handful
   of `src/kb` and platform call sites; they may be accidental rather than intended.

## 5. Proposed sequence

**Corrected 2026-08-15.** An earlier draft of this section had the first slice port
`db1_economizer_state_load/_save` into Go behind a stage. That is the wrong order: it moves a
boundary and rewrites an implementation in one step, so a failure has two candidate causes.
The boundary moves first, with the C behind it unchanged.

### Phase A — db1 becomes a module, still C

`src/db1/` is a source boundary, not a module: 129 files, 64 headers, no descriptor, no
`docs/modules/db1.md`, absent from the canonical inventory. Making it a module means:

- **The directory moves** to `src/modules/db1/`. Descriptors enforce `sources` under
  `src/modules/<id>/` (`ownership-role-boundary`), so this is not optional. The 587 include
  sites across 349 files survive it: they are bare (`#include "db1.h"` against `-Idb1`), so
  the build's include path changes and the call sites do not.
- **It is a `process` component with `runtime: "c"`.** The contract already permits this —
  `validate_module_process_contracts` asserts `(id in GO_PROCESSES) != (runtime == "go")`, so
  a component simply absent from that set must be C. No process module is C today; db1 would
  be the first, and its grant pins its own executable the way every module's does.
- **It is `required`**, which is now free of consequence: since the principal ref became a
  declared identity rather than a position, classification no longer renumbers anything.

Phase A moves no logic and changes no caller. It is packaging plus a bus surface.

### Phase B — callers cross the bus, domain by domain

This is the phase that makes it a boundary rather than a directory. 349 files stop including
db1 headers and call stages instead. It is the bulk of the work and the only phase that
changes behaviour, so it goes one domain at a time, each with its C wrapper deleted in a
following change rather than the same one.

The economizer reducer state is still the right first domain: two functions, one caller, and
it finishes a seam that is otherwise complete. `gw_state_key` / `gw_fnv1a` /
`gw_state_next_turn` follow the blob they key.

#### Phase B ordering, measured

Counted on `testing` at 2695, excluding `db_schema.h`, which exists in both `src/db1` and
`src/modules/db2/c` so a name match cannot tell the two apart:

| Module | db1 includes |
| --- | --- |
| workflows | 14 |
| delegates | 5 |
| roundtable | 4 |
| roadmap | 3 |
| config, tools | 2 each |
| audit, benchmarks, execution-policy, git, guardrails, kb_client, learning, memory, protocols | 1 each |

Fifteen modules, 39 include sites. Nine have a single include, so most of Phase B is small
cutovers and the ordering falls out of the table: the single-include modules establish the
pattern, then `roadmap`, `roundtable`, `delegates`, and `workflows` last.

**The coupling is direct, not inherited.** 271 files include a db1 header themselves; only 31
gain the dependency solely through another header, and 16 of those are through
`src/headers/db1_optional.h`. So there is no tangle to unpick first: Phase B is a large number
of individually small cutovers, not a small number of load-bearing ones.

#### `db1_optional.h` is the existing optionality seam, and Phase B must replace it

`src/headers/db1_optional.h` already answers "which DB1 calls may be absent". Server builds
link the real objects; KB builds do not, and under `AIMEE_DB1_DISABLED` the optional calls
become null pointers that callers must guard. `src/kb/kb_mcp_osv_stub.c` is the same mechanism
by hand: it stubs three DB1 symbols so a kb-hosted MCP plugin still runs the OSV supply-chain
scan without linking DB1.

Two consequences. The set of calls that file marks optional is a ready-made first cut at which
DB1 surface is genuinely server-only, which informs the stage grouping in §3.1. And the
weak-reference trick does not survive the move: once a caller reaches DB1 over the bus, "absent
because unlinked" becomes "absent because unreachable", which is the availability answer every
other module already gives. Phase B has to carry that guard across rather than delete it, or
`aimee-kb` silently loses a security gate.

### Phase C — port the implementation to Go, per domain

Only now does the language change, against a stage contract already proven by Phase B. This
is where the pure-Go SQLite driver (§3.2) and the transaction questions (§4.3) actually land,
and where the 167 test-only symbols (§2) should already have been resolved.

Splitting B from C matters: a caller that has already crossed the bus does not care which
language answers, so Phase C can proceed domain by domain without touching a single call site.

## 6. What this proposal does not do

No schema change. No second store. No change to DB2 or the KB boundary. No new policy,
approval, or CLI surface. The stage grouping in §3.1 is presented as a decision, not taken.
