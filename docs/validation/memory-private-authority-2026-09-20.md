# Private mutation authority validation

The Go private-memory owner now uses the shared replacement admission rules for
store, supersede, conditional correction and retirement. Authenticated host
context supplies user/model authority and principal identity; body fields cannot
elevate them. Migration 29 labels existing authorship `unknown`, captures verified
new authorship, retains it in revision history, and prevents compatibility SQL
from bypassing protected-content admission. Ordinary runtime roles cannot erase
or truncate canonical private records; user-authorized retirement remains available.

Private writes lock the current row, compare an optional version and commit the
payload, authorship, history and invalidation together. Same-key stores serialize
missing identities with a transaction advisory lock. The collection owner is
read with the locked record, avoiding a separate version-check round trip.
Model confidence is capped at 0.8, or 0.5 for L5. Background maintenance only
changes eligible model-authored, unprotected records; user and unknown authorship
requires review rather than automatic promotion, demotion or retirement.

## Local checks

The complete uncached memory and Aimee-family race suites passed with both
required PostgreSQL fixtures enabled: 51.553 seconds and 1.400 seconds.
Shipping migration tests cover verified human and model callers, body/incomplete
context forgeries, authority refusals without journal changes, model corrections,
retained authorship, legacy unknown provenance, protected epistemic kinds,
authorized retirement, runtime SQL bypass refusal and maintenance exclusions.
Existing retention tests cover concurrent conditional corrections and rollback on
late history failure. Privacy and recall-composition fixtures now apply the
shipping private migrations, including the host-context command envelope.

Memory ownership, C-boundary and schema synchronization checks pass. The memory
owner remains Go and the C bus is unchanged. The previous PR head, `68eaab6d44`,
completed its full CI workflow successfully (run 35523906337); that result does
not certify these newer changes. Fresh Docker and CI results for this change
will be recorded separately after they finish.

## Remaining work

Private proposal/reviewer and durable idempotency parity remain open. This change
refuses unauthorized replacement; it does not yet create a private correction
draft. Broader MR-01–MR-18 acceptance and full-request performance certification
remain open in the delivery tracker. Whole-DB2 retirement remains deferred.
