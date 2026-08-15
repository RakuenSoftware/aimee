# Proposal: DB1 as a Go module on the event bus

- **State:** PENDING — scope only; no implementation has started.
- **Date:** 2026-08-15.
- **Charter roles:** Constrain-Verify / Gate-Promote.
- **Thesis:** DB1 is state, so by the module doctrine it belongs in a Go module reached over
  the event bus rather than a C library every component links. This proposal measures that
  surface, names the three constraints that decide the module's shape, and proposes a
  sequence. It does not propose a schema change, a second store, or any new policy surface.

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
| — in `src/posix`, `src/kb`, `src/db2`, platform | 46 |
| **Distinct symbols used in production** (non-test) | **242** |

The gap between 409 exported and 242 production-used is the first useful finding: **167
exported symbols are reached only by tests.** Whatever shape the module takes, that
difference should be resolved before porting, not carried across — either the tests are
exercising internals that need no module surface, or they are the only coverage of
functions nothing calls.

## 3. Three constraints that decide the shape

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

1. **Establish the pattern on one domain.** Economizer reducer state is the natural first
   slice: two functions, one caller, and it unblocks a seam that is otherwise finished. Port
   `db1_economizer_state_load` / `_save` behind one stage, with the pure-Go driver, and let
   that settle the transaction and error-mapping questions on something small.
2. **Move the fingerprint with it.** `gw_state_key` / `gw_fnv1a` / `gw_state_next_turn`
   exist only to key and sequence that blob; they follow it into the module and the gateway
   seam is then done.
3. **Resolve the test-only surface** before porting further, so the module is not shaped by
   symbols nothing calls.
4. **Then domain by domain**, largest caller-count first, each with the C wrapper deleted in
   a following change rather than the same one — the pattern this migration has used
   throughout: add the owner, cut the caller over, delete the C, each reviewable alone.

## 6. What this proposal does not do

No schema change. No second store. No change to DB2 or the KB boundary. No new policy,
approval, or CLI surface. The stage grouping in §3.1 is presented as a decision, not taken.
