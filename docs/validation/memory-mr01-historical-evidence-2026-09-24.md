# MR-01 historical memory-backed evidence — 2026-09-24

Requested-time assertion and relation reads now apply the historical parent
policy instead of requiring every retained parent to remain current today.
Superseded, archived and retired versions can support an authorized historical
result; requested-time validity remains half-open. Deleted, revoked, quarantined,
suppressed active and foreign-scope parents remain excluded. Ordinary current
reads retain their existing exclusions. The policy fingerprint advances to
`current-validity-v11` so cached evaluation identities do not mislabel the change.

The same assertion predicate is used by source revalidation, including the
selected read policy and exact parent revisions. An edit or revocation after
selection still refuses release. Generated relation inputs retain their copied
revision checks and apply the requested parent-time policy.

The [restricted-role reproduction](memory-mr01-historical-evidence-2026-09-24/before.txt)
returned no authorized historical assertion before the change. The
[targeted race replay](memory-mr01-historical-evidence-2026-09-24/targeted-race.txt)
passes selection and release, current exclusion, and historical erase/revoke,
quarantine, scope and exclusive-boundary cases. The
[full run](memory-mr01-historical-evidence-2026-09-24/full-before-expectation-update.txt)
found one older public-route assertion that expected historical reads to exclude
an authorized archived parent. It has been updated to assert that exact retained
parent while still requiring current routes to exclude it; the remaining full
suite had no failures. The updated public-route and historical/source targeted suite passed in 3.537 seconds. The new 188-check process fixture remains pending. This is not MR-01 certification.
