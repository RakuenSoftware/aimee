# Source revalidation at provider handoff

Automatic ingress previously preserved assertion/episode source versions but did
not use them at the final provider fence. The Go memory owner now retains the
versioned sources selected by outer packing and issues a random opaque handle.
The external host installs that handle only after integrity acceptance. Request
context copies preserve it for asynchronous native execution.

After final request-byte admission and before selecting provider bytes, the host
asks Go for a scoped revalidation request, forwards it through the authenticated
KB action transport, and returns the owner answer to Go. Each attempt has a fresh
challenge; a reply must match both the challenge and source digest. A changed,
hidden, erased or ineligible source refuses dispatch as `stale_context` (409).
Missing/expired handles, lost Go state, transport failure and mismatched replies
refuse as `unavailable`. Every retry checks the owner again.

The KB Go owner compares all selected assertion/episode revisions, owner IDs and
complete direct memory-parent sets in one SQL statement snapshot under the
restricted runtime role and request scope. It applies current parent eligibility.
Assertions preserve their original valid-time/belief-time/history policy; facts
retain their stricter all-evidence eligibility contract. No per-source query or
per-parent module call is added. Exact decimal IDs remain strings across C JSON.

The handle is bound to the originating request/principal/caller and active scope.
Repeated assembly accumulates prior source bindings in the same handle rather
than discarding sources from earlier native context. Finishing the HTTP request
or native run releases its Go-held state. Interrupted requests have a 15-minute
expiry, 1,024-entry limit and 16 MiB source-payload limit; capacity refusal never
silently evicts a live handle. These are process-local guards, not durable receipts.

## Validation

- Full Go/PostgreSQL memory and runtime-web suites pass. Runtime-role checks cover
  root revision, suppression and expiry; parent content, suppression, expiry and
  scope changes; missing added parents, including invalidated fact evidence;
  historical/fixed-time policy; mixed assertion/episode checks; and the routed
  public KB operation. Targeted runtime-role race checks pass.
- Go tests cover stale/outage/wrong-challenge/wrong-digest answers, changed
  principals, restart/expiry, replayed and superseded attempts, scope changes,
  capacity, accumulated versions, concurrent request isolation and cleanup.
- The actual C ingress adapter with the independently running Go fixture passes
  all three provider formats with both fence modes. It tests changed sources,
  unavailable KB/Go owners and mismatched replies: refused attempts select zero
  provider bytes. Async context copies retain their handle, repeated attempts
  revalidate, and completed handles cannot be revived. The fixture supplies KB
  answers; this native test alone does not prove live HTTP/SQL race behavior.
- Native application build, HTTP/request-context/fence unit tests, standalone
  export and 17 S1 checks pass. All 77 lint
  checks and documentation/link checks pass, including the final request cleanup.
- Five authenticated deployment checks cover unchanged sources, scope loss,
  unauthenticated transport, parent changes after selection and refreshed bindings.
  Fresh deployment of this change and latest-head CI are pending.

The preceding batched-fact image passes [1,570 fresh T2/T3 checks](memory-fact-query-batching-2026-09-21.md).
Its CI T1 test compared whole-projection stability while testing one fixture's
source binding. Other candidates and retrieval traces are not frozen by that
fixture. The check now compares the exact fixture source and independently verifies
the complete commitment; changed-parent text comparison concerns that assertion.
This does not treat a changed whole projection as having the same digest.

## Cost and limits

The [local benchmark](memory-source-revalidation-2026-09-21/benchmark.json) checks
36 assertions and 36 direct parents using one database query. Reusing fixed SQL
and sharing eligibility expressions improves median cost from **2.02 ms to
1.36 ms**, and median allocations from **170,832 to 56,938 bytes** (66.7% lower).
[Initial](memory-source-revalidation-2026-09-21/initial.txt) and
[optimized](memory-source-revalidation-2026-09-21/optimized.txt) samples are retained.
This measures the newly added check, not whole-request latency or released P95.

Coverage is retained versioned facts, typed assertions and episodes in automatic
ingress with an active host request context. Unversioned ordinary/private recall,
learning/procedure/code channels and callers without that context remain outside
this check. Source versions do not cover every transitive or collection dependency.
The statement snapshot does not lock writers through remote provider handoff.
Durable prepared/dispatch/acknowledgement receipts, final-body and renderer/policy
binding, restore-resistant incarnation and post-check race handling remain open.
No MR-01–MR-18 proposal is certified complete by this checkpoint.

All source policy, version checks, state and module-side transport remain Go.
The C host only forwards opaque requests/replies and enforces the Go outcome;
the existing C bus is unchanged.
