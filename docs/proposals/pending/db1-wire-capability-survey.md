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
3. **Migrate T0/T1 families**, largest ready-count first: `workflow` (60),
   `delegation` (27, but see the ledger coupling below), `runtime` (22).
4. **T2 only when a family actually needs it** — 18 operations do not justify a
   frame change on their own.
5. **T3 last**, designed against the real shapes: 109 operations spread across
   struct arguments, struct and array out-parameters, and malloc'd returns.

Two constraints sit outside the wire and are unchanged by any of this:

- `delegation` is coupled to `agent_jobs` by `coupled_sources`, because
  `server_compute.c` keeps the reservation beside the launch. A reservation
  lookup that fails reads as "no reservation" and launches a second paid
  delegate. That family needs an idempotency story before it crosses, not a
  wire format.
- `identity` puts DB1 on the auth path.

## How to reproduce

`scripts/survey_db1_wire.py` produces every number above. It is a measurement,
not a gate: it reads the headers, the Makefile and the call sites, so its
answers move when any of those move. Re-run it before trusting these figures
again, rather than citing this document.

The two counting mistakes described earlier are designed out of it — signatures
are read from headers with line joins collapsed, and `CMD_SRCS` is excluded —
because both were made while arriving at these numbers.
