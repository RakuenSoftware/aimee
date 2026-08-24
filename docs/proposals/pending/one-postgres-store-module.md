# Proposal: one PostgreSQL store module, and the C store deleted

- **State:** IN PROGRESS. The module's shared serving layer is built and
  mutation-verified. **12 families are native (137 of 463 operations)**, each
  with unit tests against a scripted database and a suite against a real
  PostgreSQL 17. 7 families remain.

## What this is

`aimee-server` stored its state in a C module over SQLite: 71 files, ~46k lines,
3,978 `sqlite3_` call sites. This replaces it with Go over PostgreSQL and deletes
the C.

**There is no db1/db2/db3 any more.** That split was historical — SQLite on the
server, PostgreSQL on the KB — and it is gone. There is **one PostgreSQL store
module**, and both daemons run it against their own database. Nothing in the
module is named for a daemon, and the DSN is `AIMEE_STORE_URL` rather than
anything per-process: the module is the same, the database it points at is not.

A different engine is a **different module**. Swapping PostgreSQL out means
installing that engine's module, not configuring this one — which is what makes
the store pluggable without this package having to know it might be replaced.

It is a replacement, not a port: no translation layer, no dual-run comparator,
no compatibility shim. An earlier attempt built exactly that apparatus — a SQL
translator, five dual-run suites, a dialect ledger, two semantic audits — and it
has been deleted. It was machinery for preserving SQLite's behaviour, and some
of that behaviour (NULLs-first ordering, case-insensitive `LIKE`) is SQLite's
quirk rather than anything worth carrying forward.

## Why this is tractable

**The callers already speak the bus.** All 19 families are active and all 62
declared daemon-side C sources are retired. Nothing calls the C store directly
any more — callers exchange frames. This changes an implementation behind a
contract that already exists.

**`bus_host_serve_kind` binds one kind to exactly one serving slot,** so the Go
module takes over kinds 11777–11795 one at a time. There is never a moment where
both implementations answer.

**461 of 463 operations share one wire format,** so the framing is written once.

## Structure

```
server-go/modules/postgres/
  health.go        the health stage
  wire.go          the fields-v2 codec
  family.go        dispatch, arity, transactions, the shared pool
  families/        one file and one schema per served family
```

`family.go` enforces three things once rather than per family:

- **A malformed frame is a bus-level refusal; everything answerable is in-band.**
  A bad argument is a well-formed question with an answer; a truncated frame is
  not a question.
- **Arity comes from the catalog,** so an operation never indexes past its own
  fields.
- **A refusal rolls back.** An operation reporting `missing` or `invalid` after
  writing something would otherwise commit that write.

### Wide rows are declared, not transcribed

The roundtable family's four entities carry 36, 33, 22 and 15 columns. Writing
those out by hand at every read and write is 106 chances to transpose two of
them — and a transposition would be *consistently* wrong in the SELECT, the row
reader and the UPDATE at once, so no behavioural test would notice.

So the shape is declared once per entity as a column spec, and the SELECT list,
the row reader and the UPDATE statement are all derived from it. The spec is
checked against the C's own column lists in a test, which is the only place the
two can be compared; transposing two columns in it fails that test immediately.

Operations whose commit/rollback decision is *part of the answer* declare
`RunDB` and manage their own transaction. Two families need this, both because
they commit a garbage-collection pass while rolling back the attempt that
triggered it.

One pool serves the whole process. Nineteen families opening nineteen pools
would multiply the connection count against a `max_connections` that is a hard
ceiling. The health probe is the one exception, keeping a small pool of its own
so it can still answer "can this process reach its database" when the shared
pool is what is broken.

## Families migrated

| Family | Kind | Ops | Notable |
|---|---|---|---|
| `economizer_state` | 11777 | 2 | keyed-blob wire; delete-then-insert → upsert |
| `git_ownership` | 11778 | 7 | literal prefix matching; stable single-row reads |
| `identity` | 11789 | 4 | first-user ownership; bind-transfer refusal |
| `checkpoints` | 11790 | 4 | `RETURNING` replaces a three-statement insert |
| `jti_replay` | 11791 | 2 | replay guarantee moved to the primary key |
| `mgmt_jwks` | 11793 | 3 | install-once, not upsert |
| `mgmt_nonce` | 11794 | 5 | challenge verdict ladder; monotonic high-water mark |
| `guardrail_state` | 11785 | 7 | 387-slot fixed-width wire; six child collections |
| `pki` | 11795 | 10 | roster hash and the two-state mTLS enforcement ramp |
| `sessions` | 11782 | 24 | race-free persona claim; five tables from five sources |
| `roundtable` | 11788 | 25 | four entities, 106 columns, driven from one spec |
| `delegation` | 11781 | 44 | spawn tree, job leases, reservations, learnings |

### Changed on purpose

**Invariants moved into the database.** "Exactly one row per session" was
`DELETE`-then-`INSERT`, a convention nothing enforced with a window where the row
was absent; it is a primary key and one atomic upsert. The jti replay guarantee
was `BEGIN IMMEDIATE` plus constraint-error classification; it is the primary key
and `ON CONFLICT DO NOTHING`, where zero rows affected *is* the replay answer.

**Three latent bugs fixed.** The session-prefix lookup built `"<prefix>%"` by
concatenation and bound it into `LIKE` unescaped, so a prefix containing `%` or
`_` matched more than it asked for — and a bare `%` resolved an abbreviation to
an arbitrary session. It now escapes and matches literally. Separately, several
`LIMIT 1` reads had no `ORDER BY`, so they could return a different row on
successive calls with no write in between. And the guardrail family's collection
counts were trusted as sent: a count larger than the fixed number of wire slots
read past the frame, which mutation testing confirmed panics the module on a
request a caller can construct. And the sessions list emitted ten cells per row
from a query that selected eight, so `source` and `chat_key` were structurally
always blank — the query had not been updated when the columns were added.

**Six statements stopped being built by string concatenation.** Age thresholds
were spliced into SQL text with `snprintf` — the session expiry sweep, the spawn
and job stale sweeps, the unassigned cancel, the heartbeat staleness check — and
so was a `LIMIT`. All of it because a TEXT timestamp column left no interval
arithmetic to do. With `TIMESTAMPTZ` every threshold is a bound parameter and
every statement is a constant.

**The recursive tree walks were unbounded — and this one is fixed in the C too.**

Both `delegation_spawns.parent_delegation_id` and `lifecycle_work_item.parent_id`
are walked by recursive CTEs, and neither column has any cycle prevention on
write: `db1_work_item_set_parent` will set a work item's parent to itself. Nine
of the ten recursive CTEs in the DB1 module used `UNION ALL` with no termination
predicate, so one bad parent makes the walk spin forever holding a connection.

That is a live hang rather than a migration concern, so it is repaired in the C
rather than left until each family is ported:

- Eight of the nine collect rows that repeat exactly on a cycle, so `UNION` is
  sufficient — a recursive CTE stops when an iteration yields no *new* rows, and
  on a well-formed tree `UNION` and `UNION ALL` return the same set.
- The ninth carries an incrementing `depth`, so its rows never repeat and
  `UNION` cannot help it. That one takes an explicit bound.

Measured on the SQLite the C actually runs on: `UNION ALL` on a self-parent did
not return in 120 seconds; `UNION` returns immediately; and on an honest
four-node tree both forms answer 4, so the change is behaviour-preserving on
well-formed data. `scripts/probe-c-recursion-bound.sql` is that check.

The Go replacements carry the same bounds, and their suite asserts the negative
half — that the *unguarded* form gets cancelled — because otherwise the positive
half would pass whether or not the bound did anything.

**One search stayed case-insensitive on purpose.** SQLite's `LIKE` ignores ASCII
case, so the session title search silently matched regardless of case. That is a
person typing into a search box, so it is `ILIKE` rather than `LIKE` — one of
the few places where preserving SQLite's behaviour is the right call, because
the behaviour is what the feature is.

**One value needed an exact 64-bit path.** The guardrail family's `content_hash`
is an FNV-1a value the C prints with `%llu`, so it runs past 2^63-1. The
SQLite-derived schema made the column TEXT, storing a number as a string. It is
`BIGINT` holding the bits, converted at the boundary and exact in both
directions — the value is opaque and compared only for equality, so its
magnitude never matters, only its round trip.

**Types are native.** `created_at` columns were `TEXT DEFAULT to_char(now() AT
TIME ZONE 'utc', ...)` — timestamps that sort correctly only by accident of
format, cannot be compared against an interval, and drop the zone. They are
`TIMESTAMPTZ`, formatted back to the wire's spelling at the boundary. Digests are
`BYTEA` with a length constraint rather than hex text.

**Singleton tables say what they mean.** Three declared `singleton BIGINT
GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY CHECK (singleton = 1)` — an identity
sequence on a column whose every generated value past the first violates its own
CHECK. They are now `BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton)`.

**Surrogate keys dropped where they were never used.** Both git-ownership tables
carried an identity `id` *and* a UNIQUE on the pair that actually identifies a
row, and nothing selected, joined or deleted by id.

Token epochs deliberately stay `BIGINT`: they are supplied by the caller and
compared against each other, never against the database's clock.

### Deliberately unchanged

The wire, because the wire is the contract — the C *clients* are still on the
other end and stay there.

The status conventions, including the awkward ones. Several families answer
every outcome with wire-OK and put the real result in a reply field, because the
C client maps any non-OK wire status to `-1`, which is not one of the result
codes.

## Testing

**Go unit tests** drive each family through its real handler with a scripted
database: dispatch, arity, validation, commit/rollback. The economizer family
additionally drives the **real shipped client** against the real handler, so the
two sides agreeing on bytes is tested rather than assumed.

**PostgreSQL suites** (`scripts/postgres-family-suites.sh`) cover what a scripted
database structurally cannot: that `ON CONFLICT` reports a replay as zero rows,
that CHECK constraints hold, that a sweep is bounded and ordered, that an escaped
`LIKE` matches literally where an unescaped one over-matches. Statements are
`PREPARE`d from the module's own text. The runner creates and provisions its own
container, because the test host reaps them between runs.

### What a Go test cannot check

The depth guard exposed a real limit. The Go test asserts the guard is textually
present and its bound is a parameter — and a mutation writing `true OR d.depth <
$2` satisfies every one of those checks while removing the guard entirely. It
went unnoticed.

Whether a recursion terminates is a database behaviour, so the assertion with
teeth lives in the SQL suite: it builds an actual cycle, runs the walk *without*
the guard under a short statement timeout and requires it to be cancelled, then
runs the guarded one and requires an answer. The negative half is the part that
matters — without it the positive half would pass whether or not the guard did
anything.

**Every guard is mutation-tested.** Three findings worth recording: a mutation that
removed a check made a variable unused, so the *build* failed rather than the
test — a false "caught" — and dropping `peer_fingerprint` from the nonce binding
comparison went **unnoticed**, revealing a real coverage gap that is now closed.
A fixture bug surfaced the same way: an all-digit bearer digest is unchanged by
`ToUpper`, so the uppercase-rejection case was asserting nothing. And the
column-spec test earns its place: transposing two adjacent columns in the
roundtable pass spec is caught at column 29, where nothing behavioural would
have noticed.

## Remaining

7 families, 326 operations:

| | | | |
|---|---|---|---|
| `ensemble` (6) | `conversation` (45) | `telemetry` (45) | `workflow` (49) |
| `agent_work` (53) | `runtime` (60) | `lifecycle` (68) | |

The remaining families are large in operation count rather than in novelty, and
the column-spec approach the roundtable family introduced is what makes them
tractable: the wide ones are the same shape, only wider.

`ensemble` is the one that is not merely SQL: its 987 lines are a phase/turn
state machine, so it is a port of logic rather than of statements.

Downstream of finishing:

- **Test fixtures.** `server-go/internal/db1/db1test` reaches into the store
  *file* to move the clock. Under PostgreSQL there is no path, only a DSN. They
  are the last SQLite in Go — production Go is already clean.
- **Deleting the C.** `src/modules/db1` goes once every kind is served natively,
  along with the 54 files in `src/tests` that test it.
- **The `db1` names in the contract itself** — `server-go/db1`, the
  `AIMEE_DB1_*` wire constants, the catalog's own vocabulary — are the caller-side
  contract and its generator. Renaming them is a separate change with a much
  wider blast radius than the store.
