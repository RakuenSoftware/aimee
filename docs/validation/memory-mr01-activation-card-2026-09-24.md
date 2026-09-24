# MR-01: activation respects generated-card inputs

Activation recall applied lifecycle and time checks but omitted the generated
card dependency predicate. A [restricted-role regression](memory-mr01-activation-card-2026-09-24/before-race.txt)
reproduces a stale card returned after its copied input changed. The same card
was already excluded by ordinary exact-ID retrieval and card listing.

Activation selection now checks the shared generated-card dependency predicate
before the limit. This also protects its post-fusion activation filter. The
[targeted race replay](memory-mr01-activation-card-2026-09-24/after-race.txt)
passes in 1.649 seconds, covering valid-card admission and eight invalidation
cases: shared/private input edits, revocation, expiry, hidden input, unit edit,
missing observation and changed card parent. Fixture setup uses the owner;
serving checks use the existing non-owner role.

The [full race suite and export](memory-mr01-activation-card-2026-09-24/full-race-export.txt)
pass in 219.943 and 4.750 seconds. The [actual HTTP reproduction](memory-mr01-activation-card-2026-09-24/http-before.json)
on `763aa8c7d` passes current-card admission, then fails the revoked-input
exclusion check. The [corrected candidate HTTP run](memory-mr01-activation-card-2026-09-24/http-after/checks.json)
passes all 42 checks on `6b79ec2f2`, including valid-card admission followed by
revoked-input exclusion. The runner exits zero and removes its disposable stack. This correction
adds no schema or C policy changes and does not close MR-01's provider-release race.
