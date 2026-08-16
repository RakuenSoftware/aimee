# Proposal: what the rest of DB1 needs from the wire

Two DB1 families now cross the event bus, and both halves of that boundary are
generated from `src/modules/db1/eventcontract/operations.json`. Adding a family
is a catalog edit rather than hand-written wire.

Every family since the second has still cost a surprise, and each surprise was
the same kind: a capability the wire lacked, found only by reading the callers
of the family being migrated. Integers were found that way. Optional fields were
found the same way, one family later. Rather than keep discovering them one at a
time, this surveys all 281 remaining operations at once and says what is
actually left.

## Thesis

The remaining work is not one problem. It is 54% already reachable, 6% behind a
small addition, and 39% behind a real design question — and the largest practical
risk is not in that 39% at all, but in a field-size cap that lets 33 already
reachable operations pass their tests and fail on production data.

## What was counted, and what was wrong before

The survey set is every function declared in a reserved family's header that has
at least one caller **linked into a binary**: 281 operations across 98 caller
files.

Two earlier counts were wrong, both by trusting an extractor that looked right:

- Reading definitions line by line missed every wrapped signature. Declarations
  are now read from the headers with the line joins collapsed.
- `CMD_SRCS` was counted as production. It is not linked into anything — it
  feeds `cmd-srcs-compile-check`, which compiles those files so they cannot rot
  silently. **57 of its 109 files are named nowhere else in the Makefile.** That
  moved the surface from 326 operations to 281, and left 45 operations whose
  only callers are compile-only.

Those 45 are not necessarily dead: the question of whether those commands should
be linked is separate, and is not answered here. But they should not be migrated
on the strength of a caller that never runs, which is how `project_clones` came
to sit inside an active family without being served.

## What the 281 need

| tier | needs | ops | cumulative |
| --- | --- | --- | --- |
| T0 | nothing — fits the wire today | 63 | 63 (22%) |
| T1 | integer arguments | 75 | 138 (49%) |
| T2 | a richer reply — counted, multi-value, or a status beyond ok/miss/fail | 34 | 172 (61%) |
| T3 | structured payloads (struct in/out, arrays, malloc'd returns) | 109 | 281 (100%) |

The first version of this table said 76/78/18 and put 154 in reach. It was
wrong, and the way it was wrong is the more useful fact: it classified
**parameters** and never the **return contract**. The wire answers a write with
0 or -1 and a read with found, not-found or error, so an operation that returns
a count, or a distinguished refusal like `db1_wfe_bind`'s -2 for a single-writer
conflict, has nowhere to put the distinction it exists to make. Migrating one of
those would have quietly reported a conflict as an error.

Two mechanical cases were mis-tiered the same way: an operation taking no
arguments cannot be framed at all, and one with two out buffers has one reply
value to put them in. Sixteen operations were affected across the three.

T1 shipped, which is what took reachability from 27% to 54%. T2 buys 18
operations, which is a poor return for a frame change. T3 is the real remainder
and the only part that needs a design rather than an addition.

Per family, ready means T0 + T1:

| family | ready | total | behind T2 | behind T3 |
| --- | --- | --- | --- | --- |
| workflow | 51 | 91 | 15 | 25 |
| delegation | 26 | 37 | 4 | 7 |
| runtime | 20 | 35 | 4 | 11 |
| agent_work | 15 | 38 | 4 | 19 |
| sessions | 8 | 22 | 0 | 14 |
| conversation | 7 | 20 | 2 | 11 |
| telemetry | 7 | 33 | 4 | 22 |
| identity | 4 | 5 | 1 | 0 |

No family is fully reachable now that return contracts count -- `identity` came
closest at 4 of 5, and it is the one family that should go last
rather than first: what is reachable in it is `db1_secret_*` and
`db1_remote_client_*`, whose caller is `server_bearer_auth.c`.

## The two gaps that are not about tiers

### Optional fields — smaller than it looked (CLOSED)

The wire refused an empty field, and the generated client refused a NULL one. A
call-site scan of all 154 reachable operations finds **16** that pass `NULL` or
`""` as a literal. The remaining 138 need nothing.

This is a lower bound: it sees literals, not a variable that happens to hold
NULL. It is also cheap to close — the domains already collapse the two
themselves, so a per-field `required` flag in the catalog is enough, with the
client mapping NULL to empty and the stage enforcing which fields may be blank.

### Field size — the one that will ship green and fail in production (CLOSED)

`AIMEE_DB1_FIELD_MAX` was 512 bytes. **33 of the 154 reachable operations carry a
prompt, a result, an output, or a JSON blob**, and DB1's own columns run to 16
KB. A 512-byte cap truncates none of them: the client refuses the call and
returns the same -1 it returns for a broken store.

That failure is data-dependent, which is what makes it worse than the other
gaps. Short strings pass every test; the first long prompt fails, and fails
looking like something else.

The cap is self-imposed. `AIMEE_MODULE_MESSAGE_MAX_BODY` is 16 MB, so the bus
has room. The obstacle is that the generated wire uses stack arrays on both
sides — `char field[AIMEE_DB1_FIELDS_MAX][AIMEE_DB1_FIELD_MAX]` in the stage,
and a request buffer of the same shape in the client. Raising the constant to
anything useful puts hundreds of kilobytes on the stack. **Large payloads
therefore require the generated code to allocate, which is a change to the
shape of the generated wire rather than to a number in it.**

Both are closed as of the pull requests that follow this document. The counts
above are kept as they were measured, because they are what justified the order
below — not because either still blocks a call.

## The unit that migrates is a source, not an operation

Everything above counts operations, and that is the wrong unit for planning.
The daemon swaps a whole `.c` out of its link and puts the generated client in
its place, so a source keeps ALL of its operations in-process until EVERY one
of them can cross — the client and the domain would otherwise both define the
symbols that did move. `git_ownership` worked because all five of its functions
moved together.

Counted that way:

| capability | whole sources ready | operations |
| --- | --- | --- |
| today | 7 of 48 | 22 |
| + T2 | 14 of 48 | 41 |
| + T3 | 48 of 48 | 281 |

**240 of 281 operations sit in sources that need T3.** Per-operation readiness
overstates what can be done by roughly six times, and the order below was
written against that overstatement.

Of the 22 that can move today, 9 are `delegations` — coupled to `agent_jobs` and
blocked on idempotency, not on wire — and 2 are `secrets`. What is genuinely
available is `diagnose` (4), `web_page_cache` (3), `fsnap` (2), `cost_fold` (1)
and `decisions` (1): **eleven operations across five small sources.**

That reframes the whole remainder. T2 doubles the ready sources but adds only
nineteen operations; it is not the thing standing in the way. The migration is
a T3 problem — structured payloads, struct and array out-parameters, malloc'd
returns — and no amount of small additions changes that.

A source could be split so its ready functions migrate alone. That is a real
option and it is not free: it edits DB1's own layout, its descriptor, and the
ownership rules that keep the catalog honest. Worth considering deliberately
rather than reaching for once a cutover stalls.

## Recommended order

1. ~~**Field sizing first.**~~ Done: requests are no longer capped. The frame is
   sized from the arguments and the decoder allocates from the frame, so a
   prompt or a document crosses whole, as it always has in-process.
   `AIMEE_DB1_VALUE_MAX` now bounds only the reply a stage builds.
1. ~~**Optional fields.**~~ Done: a field declares whether it is required. An
   absent optional value travels as empty, which is how the domains already read
   NULL. A scoped operation still cannot make its key optional -- a key that may
   be absent is not a key.
1. **Superseded — kept for the reasoning.** It blocks 33 already-reachable operations and is the
   only gap whose failure mode is invisible until production. It is also the one
   that changes the generated code's shape, so doing it before more families
   migrate means fewer files regenerate later.
2. **Optional fields.** 16 operations, a per-field flag, no frame change.
3. **Migrate whole ready SOURCES**, not families: `diagnose`, `web_page_cache`,
   `fsnap`, `cost_fold`, `decisions`. Eleven operations, and the only ones the
   wire can carry end to end today.
4. **T3 next, not last** — designed in
   `docs/proposals/pending/db1-structured-payloads.md`, where it turns out to be
   a counted reply plus a rule that a struct is its members, rather than the
   redesign the name suggests. It is what 240 of 281 operations are waiting on, and
   the earlier order had it at the end because operations were counted where
   sources should have been.
5. **T2 when a source it completes is actually wanted** — it finishes seven more
   sources, several of them in the coupled or auth-exposed families.

Two constraints sit outside the wire and are unchanged by any of this:

- `delegation` is coupled to `agent_jobs` by `coupled_sources`, because
  `server_compute.c` keeps the reservation beside the launch. A reservation
  lookup that fails reads as "no reservation" and launches a second paid
  delegate. That family needs an idempotency story before it crosses, not a
  wire format.
- `identity` puts DB1 on the auth path.

## What it says now, and why the ladder is gone

Everything above is the measurement as taken. Re-run after struct flattening,
the instrument disagreed with itself: it still counted a struct as blocked, and
its T0..T3 ladder had three rungs that had shipped. Worse, the ladder asserted a
dependency that does not exist — rows and callee-allocated out-parameters are
independent, and neither waits on the other.

It now reports a **set** of missing capabilities per operation, unioned per
source, because a source is ready exactly when that union is empty. Retiering
also dissolved the "unknown" bucket, which had been hiding four distinct
answers behind one word: `char (*out)[N]` is rows of one text column, an enum
passed by value is an integer, `long long *` is an out-integer the pattern
simply missed, and `cJSON *` is a document. Three of the four were already
reachable and were being counted as blocked.

As measured on the day this section was written: **203 of 261 operations fit the
wire, and 18 of 43 sources are ready** — against 7 of 48 when the body of this
document was written. What remains is rows (40 operations), alloc (10), a
per-operation status contract (6) and json (2).

**Rows is the next step and it is not close.** It alone takes 18 ready sources
to 34 — 134 more operations — where alloc adds one source and status adds
three. That is the whole argument for doing it next.

## How to reproduce

`scripts/survey_db1_wire.py` produces every number above. It is a measurement,
not a gate: it reads the headers, the Makefile and the call sites, so its
answers move when any of those move. Re-run it before trusting these figures
again, rather than citing this document.

The two counting mistakes described earlier are designed out of it — signatures
are read from headers with line joins collapsed, and `CMD_SRCS` is excluded —
because both were made while arriving at these numbers.
