# Proposal: one result convention, with specific results, across the project

- **State:** OPEN — the convention needs ratifying before the code moves, because
  every subsystem that adopts it stops being able to mean what it means today.
- **Date:** 2026-08-22.
- **Thesis:** A function's answer should say three things in one integer: whether
  it is finished, whether it succeeded, and *which* success or failure it was.
  The project has thirty-five separate answers to that question and none of them
  agrees with another.

## 1. Scope

Every function in this project that returns a number as its outcome.

Not a storage convention, not a boundary convention, not one subsystem's house
style. If a call answers with an integer and that integer is how the caller
learns what happened, it follows this.

**Unless another standard already owns that number.** Where the value is defined
outside this project and read by something outside it, the external standard
wins and this one does not apply:

    HTTP status codes            an HTTP route answers 403, not -3
    POSIX errno                  a libc wrapper passes errno through as errno
    SQLSTATE                     a database driver reports what the database said
    process exit status          0 is success there, and that is not ours to change
    signal numbers               likewise
    protocol wire status         AIMEE_MODULE_STATUS_*, bus.ModuleStatus

The test is not "is this number small" but "did someone else define it". A
function translating between the two worlds does exactly that — translates — and
its own return value follows this convention even when its argument does not.

## 2. The convention

    < 0   failed, and the specific value says which failure
    = 0   not finished: running, queued, in flight — there is no outcome yet
    > 0   succeeded, and the specific value says which success

Three properties follow, and each is doing work:

**Zero is not an outcome.** It means the statement is still running, the job is
still queued, the process is alive with nothing decided. A caller holding a zero
has not been told the result, because there is not one yet. This inverts current
practice, where zero is the most common way to say "fine".

**A success that changed nothing is still a success, with its own number.** An
upsert whose row already held what it would have written, a delete of something
already gone, an update whose `WHERE` matched no rows: all three ran correctly.
They are not zero, because zero means unfinished; they are a distinct positive,
so a caller can tell "I wrote it" from "it was already so" without a second
query.

**A determination is a success, not a failure.** An operation asked to decide
something, which decides *no*, has succeeded. `TYPED_FACT_REJECTED_REL` is the
clearest case in the tree today: it is negative, so it reads as a malfunction,
when in fact the ontology check ran and answered. Under this convention a
refusal the operation was asked to make is a positive; only a refusal that
prevented the operation from running is a negative.

Sign alone is not enough and never was. `-1` appears in thirty-five headers and
means a different thing in each, so a caller that gets one has learned only that
something went wrong. The specific number is the point of the convention; the
sign is how you read it without a table.

## 3. What exists now

A survey of `src/**/*.h`, excluding tests:

    thirty-five headers declaring a result enum or sentinel
    eighty-five distinct negative constants

The collisions are systematic rather than incidental:

    -1   "error", in thirty-five headers, meaning thirty-five things
    -2   DENIED in six organization headers, INTEGRITY in three status headers,
         NOT_BUILT in the TPM header, POOL_EXHAUSTED in the KB client
    -3   CANCELLED, EXPIRED, BUSY, CONFLICT, NOT_FOUND, TOOBIG, INVALID,
         AUTH_REQUIRED, HASH_MISMATCH, and REJECTED_KIND

And zero is load-bearing as a success in at least:

    AIMEE_PG_DONE = 0                  the statement completed
    DB2_WITNESS_CP_OK = 0              a checkpoint was signed and persisted
    DB2_WITNESS_EMIT_OK = 0            the run completed
    TYPED_FACT_OK = 0                  the fact was asserted
    DB2_WRITE_TIER_GRANT_NONE = 0      no live grant — a real answer
    CODE_PROJECT_LIFECYCLE_OK = 0      and every `return 0` meaning success

There is one existing enum that already follows the shape this proposes, and it
is the precedent for the banding below: `db2_tenant.h` reserves `-100` to
`-105`, out of the way of everything else, with one value per distinct refusal.

## 4. Allocation

    0              PENDING. Reserved project-wide. Never a domain code.

    1   .. 99      universal successes
    100 .. 999     domain successes, one hundred per domain

    -1  .. -99     universal failures
    -100 .. -999   domain failures, one hundred per domain, same band number

### 4.1 Universal successes

    1   DONE              completed; the effect was applied
    2   DONE_NO_CHANGE    completed; nothing needed changing
    3   DONE_ALREADY      completed; it was already so before the call
    4   DONE_PARTIAL      completed for part of the work; the rest is reported
                          alongside and is not an error
    5   DONE_EMPTY        completed; the answer is legitimately nothing
    6   DONE_REFUSED      completed; the decision this was asked to make is no

`DONE_NO_CHANGE` and `DONE_ALREADY` are not the same. The first is "the write
ran and matched nothing"; the second is "the write did not need to run". An
`ON CONFLICT DO NOTHING` that conflicted is the second; an `UPDATE ... WHERE`
that matched no rows is the first.

`DONE_EMPTY` exists so a read that found nothing stops being indistinguishable
from a read that could not run — the defect recorded four times over in
`db2-boundary-blockers.md`, once for a fail-closed authorization gate that
would have answered "not granted" for every subject and looked healthy doing it.

### 4.2 Universal failures

    -1   FAILED            unspecified. Permitted only where nothing more is known
    -2   INVALID           the request was malformed, out of range, or incoherent
    -3   DENIED            authorization refused the caller
    -4   UNAUTHENTICATED   there was no verified principal to authorize
    -5   UNAVAILABLE       a dependency was absent: no connection, no provider
    -6   CONFLICT          lost a race, or a precondition moved underneath
    -7   NOT_FOUND         the named thing does not exist, and its absence is an
                           error rather than an answer
    -8   TOO_LARGE         the input or the answer exceeds a stated bound
    -9   TIMEOUT           the deadline passed before an outcome
    -10  INTEGRITY         stored state failed its own check
    -11  CANCELLED         the caller withdrew before an outcome

`-1` is kept, and kept meaning nothing in particular, so adoption can proceed one
function at a time without a flag day. A function still answering `-1` is
unfinished, not wrong.

`NOT_FOUND` is a failure only where absence is an error. Where absence is the
answer — "is this subject granted", "does this document exist" — the answer is
`DONE_EMPTY`, positive, and the distinction is the whole reason both exist.

### 4.3 Domain bands

Assigned from what the tree already separates, not invented:

    band   domain                            existing anchor
    100    tenancy and identity              db2_tenant.h, already -100..-105
    200    storage (postgres, pools, txn)    db_postgres.h, connection pooling
    300    organization (budget, egress,     org_budget.h, org_egress.h,
           rate, spend, telemetry, model)    org_rate.h, org_spend.h,
                                             org_telemetry.h, org_model_catalog.h
    400    custody and vault                 vault_server_key.h,
                                             vault_custody_tpm2.h, enrollments.h
    500    code index and projects           code_project_lifecycle.h
    600    knowledge base documents          kb_payload.h, ingest
    700    memory and facts                  typed_facts.h, memory_*
    800    css                               css_render_oracle.h
    900    transport (http client, sockets)  kb_http_client.h, control.h

The bands are named for what the code does, not for which store or which binary
it happens to live in. Band 200 is storage — one Postgres, whoever is hosting
it — and it does not move or change name when the schema owner does.

A domain code is used only where no universal code says it.
`DB2_SPEND_ERR_DENIED` is `DENIED`; it does not need a 300-band code.
`DB2_ERR_TENANT_BEGIN` is not any universal failure — the transaction opened and
the GUCs did not take — so it keeps a 100-band code.

## 5. Carrying a code

**In process** it is an `int`, and nothing further is needed.

**Across the module bus** the generic envelope had four field types — `utf8`,
`u32`, `u64`, `f64` — and none of them signed, so a negative code could not
cross at all. Every operation published so far that needed one carried an
unsigned `outcome` or `acknowledged` field with a private mapping at each end,
which the caller had to know about and which no schema described.

An `i32` field type is added, bounded like the others, four bytes little-endian
two's complement, carrying the code as itself. Its negative fixtures mutate past
*both* bounds — the below-minimum vector is the one an unsigned field never
needed and the one a decoder that forgot the cast would accept.

**It is not the transport status.** `AIMEE_MODULE_STATUS_*` and
`bus.ModuleStatus` answer whether the call happened, on an unsigned envelope
field, and stay separate. An operation that runs and fails answers
`AIMEE_MODULE_STATUS_OK` with a negative result in its body, and that is correct:
the call succeeded, the work did not.

## 6. Order

1. **Define.** This document, `src/headers/aimee_result.h`, and
   `server-go/result` — the last two mirroring each other value for value, with
   a test that reads the header and fails if they drift.
2. **Carry.** The `i32` field type in the envelope and its generator. Additive:
   until a schema uses it, the generated output is unchanged.
3. **Adopt, by subsystem.** Each subsystem's enum is converted with its callers
   in one change. The six zero-as-success cases in §3 are the load-bearing ones,
   because every caller of those reads zero as "fine" today.
4. **Convert the writes that cannot report at all.** Thirty functions discard
   `aimee_pg_step` and then have nothing left to answer with; twenty of them
   return `void` and need a signature before they can answer anything.
5. **Leave `-1` alone until something better is known.** A sweep renumbering
   every `-1` to a specific code without knowing which failure it was would be
   inventing information.

Adoption is deliberately not a flag day. The convention is designed so a
converted function and an unconverted one can call each other: `-1` still means
failure, a positive still means success, and the only value whose meaning
actually moves is zero — which is why §3 lists every place zero currently
carries weight, and why those go first.

## 7. What this does not do

It does not unify exception handling, logging levels, or the mapping from a
result to an HTTP response. It is about the integer a function in this project
returns to another function in this project, and about making that integer say
which of the things that can happen actually happened.
