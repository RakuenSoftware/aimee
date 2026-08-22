# Proposal: a module reply says what happened

- **State:** OPEN — needs ratifying before operations start carrying it, because
  every schema that adopts it stops being able to mean what it means today.
- **Date:** 2026-08-22.
- **Thesis:** A bus reply cannot currently say that the work failed. Each
  operation invents a way to imply it, and no schema describes any of them.

## 1. The gap

`bus.ModuleStatus` says whether the **call** happened: OK, cancelled, deadline
exceeded, invalid request, capability absent, internal. It does not say whether
the **work** succeeded, and it cannot — an operation that runs correctly and
finds nothing, and one that runs and is refused by policy, and one that cannot
reach its store, are all `ModuleStatusOK` with a body.

So each operation improvises in its body. Across the catalog today:

    acknowledged      u32, 0 or 1, in thirty-odd writes -- and in twenty of them
                      it is a constant, because the backend returns void and the
                      handler sets the field after calling it
    outcome           u32, with a private meaning per operation: 0/1/2 here
                      meaning applied/denied/failed, 0/1/2 there meaning
                      granted/not-granted/could-not-tell
    count == 0        for every list read, where "found nothing" and "could not
                      run" are the same answer

None of these is in a schema. The mapping lives in the handler that writes it
and the caller that reads it, and the two agree by inspection.

The failures that follow are recorded in `db2-boundary-blockers.md`: a
fail-closed authorization gate whose module-side answer would have been "not
granted" for every subject while looking healthy; a Prometheus export whose
loud TOOBIG becomes a quiet truncation; an admin denial that reads as an empty
allowlist.

## 2. The reply carries a result

One `i32` field, `result`, in the reply of any operation whose outcome is not
fully described by the values it returns.

    < 0   the work failed, and which failure
    = 0   the work has not finished
    > 0   the work succeeded, and which success

Zero is not an outcome. It means queued, running, in flight. A caller holding
zero has not been told the result because there is not one yet. It is reserved
so an operation that models unfinished work has a value for it, and so no
success ever collides with "still running".

### 2.1 Successes

    1   DONE            the effect was applied
    2   DONE_NO_CHANGE  it ran and nothing needed changing
    3   DONE_ALREADY    it was already so, so nothing needed to run
    4   DONE_PARTIAL    part finished; the remainder is reported alongside
    5   DONE_EMPTY      the answer is legitimately nothing
    6   DONE_REFUSED    the decision this was asked to make is no

`DONE_NO_CHANGE` and `DONE_ALREADY` are not the same. An `UPDATE ... WHERE`
that matched no rows is the first; an `ON CONFLICT DO NOTHING` that conflicted
is the second. One write matched nothing, the other never ran, and a caller
that needs to tell them apart currently issues a second query.

`DONE_EMPTY` is the one that closes the list-read gap: a read that found nothing
stops being indistinguishable from a read that could not run.

`DONE_REFUSED` is a success. An operation asked to decide something, which
decides *no*, has succeeded — a policy verdict, an ontology check, a gate that
ran and declined. Only a refusal that stopped the operation from running is a
failure, and that one is `DENIED`.

### 2.2 Failures

    -1   FAILED            unspecified; honest where nothing more is known
    -2   INVALID           malformed, out of range, or incoherent
    -3   DENIED            authorization refused the caller
    -4   UNAUTHENTICATED   no verified principal to authorize
    -5   UNAVAILABLE       a dependency was absent
    -6   CONFLICT          lost a race, or a precondition moved
    -7   NOT_FOUND         it does not exist, and its absence is an error
    -8   TOO_LARGE         input or answer exceeds a stated bound
    -9   TIMEOUT           the deadline passed before an outcome
    -10  INTEGRITY         stored state failed its own check
    -11  CANCELLED         the caller withdrew before an outcome

`NOT_FOUND` is a failure only where absence is an error. Where absence is the
answer — "is this subject granted", "does this document exist" — it is
`DONE_EMPTY` and positive. Both exist so the two stop being one.

That is the whole vocabulary. It is deliberately small and deliberately flat:
seventeen values, no per-subsystem ranges. An operation needing to say something
this cannot say should say it in a field of its own, where the schema describes
it, rather than in a number whose meaning has to be looked up.

## 3. Carrying it

The generic envelope had four field types — `utf8`, `u32`, `u64`, `f64` — and
none of them signed, so a negative could not cross at all. That is why the
improvisations in §1 are all unsigned.

`i32` is added: four bytes little-endian two's complement, bounded like the
others. Its negative fixtures mutate past **both** bounds; the below-minimum
vector is the one an unsigned field never needed and exactly the one a decoder
that forgot the cast would accept.

`AIMEE_MODULE_STATUS_*` and `bus.ModuleStatus` are unchanged. An operation that
runs and fails answers `ModuleStatusOK` with a negative result in its body: the
call succeeded, the work did not.

## 4. This is not a Go return convention

Go answers with `error` plus a value. Which failure is a sentinel; which success
is the value returned. Nothing here belongs in a Go signature, and the DB2
module is being ported to Go.

`server-go/result` is therefore two things and no more: the vocabulary above,
and a bridge that converts at the point a reply is encoded. Two rules fall out
of the conversion, both easy to get backwards:

**A nil error is `DONE`, never `PENDING`.** A Go function that has returned has
finished. `PENDING` reaches Go only from something modelling unfinished work
explicitly — a job row, a queue entry, a code read off a boundary.

**`PENDING` is not an error.** `result.Err` answers nil for it, as for a
success. Work that has not finished has not failed, and a caller treating "still
running" as an error abandons something that was going to succeed.

An error carrying no code converts to `FAILED`. Guessing a specific code from an
error string would be inventing information.

## 5. Order

1. This document and `server-go/result`.
2. `i32` in the envelope and its generator. Additive: until a schema uses it,
   the generated output is unchanged byte for byte.
3. New operations carry `result` from the start.
4. Existing operations swap their improvised field for it as their handlers are
   ported to Go — not before, because rewriting the C handler's mapping is work
   the port deletes.

Step 4 is the reason this is worth doing now rather than after the port: the Go
implementation should be written against a reply that can say what happened,
instead of reproducing thirty private mappings and then removing them.
