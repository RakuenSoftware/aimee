# Shared memory reliability foundations

All post-stack implementation is on the single continuing
`agent/memory-reliability-proposals` branch. The existing #2988–#2990 PRs remain
separate pending their authorized merge; both available GitHub credentials refused
merge/readiness operations. No additional slice PR was opened.

## Fresh shared/private deployment

Implementation and harness `f5fd572aade74c96722e69ea6a5a254194d9eac7` were built in
owned CT 9498 on `.253`, using `Dockerfile.server` with `WITH_VSCODE=0`.
Image: `sha256:ef15724a4361fff3870d5607c708f93e784021d57c27d1e352cca2bca6d8c3eb`.
The fresh T2 topology uses separate Server/KB identities and PostgreSQL stores,
`aimee-postgres:pr2983`, and pinned Bekko-a25m embedder
`sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209`.

The sanitized [verdict receipt](memory-shared-reliability-2026-09-20/fresh-t2-f5fd572aad.json)
records 251 passing verdicts: 61 private, 167 shared, six identity and 17 topology.
This includes actual HTTP replacement with verified user authorship, failed
scope-copy rollback, preserved secondary tags, durable shared journal history
across KB restart, scope isolation and supervised owner outage/recovery.

The first shared run exposed an HTTP authority bug: the host always requested
model authority for supersede, so a user's correction of their own fact was
refused. The host now forwards its authenticated authority and Go independently
verifies it. Review-required refusals map to HTTP 409. An earlier test also used
an unsupported HTTP update route; it now exercises shipped supersede. Restart
checks preserve every pre-restart journal event while allowing genuine background
indexing writes to advance the revision.

This image predates the expected-version correction contract and the subsequent
generated-column trigger fix. Those changes require their own tested-revision
receipt; this run does not validate them.

## Expected-version correction validation

Implementation `1b4ffee6975a61e5924bf2dc65baee137a018a32` adds opt-in shared
get/supersede preconditions, bound to owner UUID, exact record ID and governed
revision. Restricted-role replay covers changed owners, changed scope tags,
hidden IDs, counter-only updates, authority refusal, admitted correction and
replayed stale correction. A committed two-connection test observes the losing
writer blocked on the actual row lock; after the winner commits, the loser gets
an expected-version conflict and creates no second replacement.

This test exposed a generated-column trigger defect: generated search outputs
could trigger and differ in BEFORE rows even for counter-only updates. Schema 23
excludes generated outputs from trigger targets and comparison, retaining their
canonical inputs. The shared journal fixture now includes generated columns and
another BEFORE trigger. It verifies unchanged content and counters emit no event.
The separate concurrency regression holds one record's counter update open while
another record in the same collection commits; collection generation is unchanged.
Governed same-collection writes still serialize in commit order.

The full memory race suite with PostgreSQL fixtures and restricted-role replay
passes (55.049 seconds). The native HTTP adapter test, schema synchronization and
memory ownership checks pass; the C bus remains C. Follow-up `f0938f7106` preserves
the token in JSON console inspection and explicitly refuses projections that
cannot return it. Its targeted contract test passes. `156fcb7f0b` keeps benchmark
unknown-coverage outcomes separate; 32 schema tests and the existing 12 temporal
fixture checks pass. These fixture checks do not certify complete MR-05/MR-18.

The fresh `1b4ffee697` image correctly refused stale corrections, but its HTTP
checks caught another transport defect: the Go fault classifier emitted 409 while
the native status decoder's whitelist rejected 409, causing a generic 502. The
runtime-web wire contract now admits 409; the native alternative classifier and
process smoke tests also cover conflict/review-required and unsupported-mode
parity. This changes HTTP transport classification, not memory policy or C bus
implementation. A fresh image must validate this correction before HTTP status
parity is claimed.

## Local correctness and performance scope

Restricted-role replay exercises the shipping schema, RLS, transactional scope
copies, rollback and authority checks. Shared journal tests cover commit ordering,
independent scope progress, outbox failure, cursor binding and protected grants.
Minimal typed projections avoid duplicated diagnostic/procedure rendering: the
fixed two-item fixture decreased from 1,044 to 280 bytes with the same evidence.
Versioned ingress limits account for exact serialized memory-envelope bytes and
retained IDs; provider-request token limits remain explicitly unsupported.

No new whole-request P95, quality noninferiority, provider token cap, durable
consumer application, or complete MR-01–18 acceptance claim is made here.
