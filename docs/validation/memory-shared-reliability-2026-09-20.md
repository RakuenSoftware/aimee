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
implementation. The real C-caller/Go-process smoke test verifies 400, 403 and 409
across that wire boundary.

The final fresh T2 run built implementation and harness
`5541025ba388d09b04539570e0e2d6e143a8b957` as
`sha256:c30858587be63a83959bc071ee9adec38387bdfbfcc25bfecdbef830f21f5ac8`,
with the same pinned PostgreSQL and embedding images above. Its separate
[sanitized receipt](memory-shared-reliability-2026-09-20/fresh-t2-5541025ba3.json)
records 257 passing verdicts: 61 private, 173 shared, six identity and 17 topology.
Both stale-version and repeated-correction requests return HTTP 409 with
`expected_version_conflict`; fresh corrections succeed and preserve authored
history and tags. Failed tag copies roll back all state. Shared owner/history
survive restart, and the existing private/KB isolation and outage gates pass.
The `.253` raw fixture evidence is retained under
`/opt/aimee-memory-proposals-evidence/t2-5541025ba3` inside owned CT 9498;
application containers were stopped after validation. No production instance was
used for these mutations.

## Durable correction retry validation

Implementation `93d235a353a2010e116830e153a8263c444657f8` adds
optional authenticated idempotency keys to shared corrections with an expected
version. Schema 24 stores immutable, actor-isolated, content-free retry references
to the existing canonical audit commit. The same transaction includes version
replacement, scope copies, extraction provenance/job, WORM sealing and invalidation.

The full memory race suite with required PostgreSQL fixtures and restricted-role
replay passes on the published implementation (66.777 seconds). Real concurrent connections verify that a second
request waits for the first transaction's outcome: after commit it replays the
same result, and after disconnection before commit it admits one replacement.
Another connection can replay the durable result without in-process state. A
forced failure at receipt insertion rolls back all prior work; an ordinary
admission refusal leaves neither an open audit commit nor a reserved key.

Restricted-role cases cover different payloads under one key, actor isolation,
connection/view changes, counter-only reads, moved/changed/erased results and
receipt permissions. Replays do not add canonical commits, invalidations or
extraction generations. Mutation telemetry excludes successful replays from new
mutation counts; refusal telemetry remains available without flushing rolled-back
success actions. Targeted audit/contract race tests pass (1.021 seconds).

The native HTTP forwarding regression and schema/Go-ownership gates pass. Only
the external host's JSON field forwarding changes; the C bus remains C. The
staged change passes the scoped secret scan. No new P95 improvement is asserted:
ordinary requests retain their existing path, while keyed writes pay for their
durable lock/receipt and use one grouped canonical audit commit.

The fresh application image is
`sha256:16baa48c5e82299d372edb64bc0b52662e465670d62c047cf743dde9a2ccca9e`,
built from `93d235a353`. The final harness is `43a1a18e7dfb683e16dbaf3aa234d6cea336ab56`;
its only change is the history-count assertion described below. Another fresh T2
stack on owned `.253` CT 9498 uses the same pinned PostgreSQL/embedder images as
above. The [sanitized receipt](memory-shared-reliability-2026-09-20/fresh-t2-43a1a18e7d.json)
records **264 passing verdicts**: 61 private, 180 shared, six identity and 17
topology. HTTP replay returns the same canonical commit/result while eligible;
changed payloads return 409, committed receipts survive KB restart, and retired
results return 409 without cached content or another correction. The existing
scope-isolation, outage/recovery, history, authorship and rollback gates pass.

The first fresh run passed 179 of 180 shared verdicts. Its duplicate-result
assertion incorrectly counted only the unsuffixed key: canonical supersession
retains the old row under `#v…`. The corrected assertion counts the complete key
family and still requires exactly two versions. The original failed evidence is
retained in `/opt/aimee-memory-proposals-evidence/t2-93d235a353`; the independently
created successful environment is recorded in
`/opt/aimee-memory-proposals-evidence/t2-43a1a18e7d`. Both runs' containers were
stopped; their evidence and volumes remain. Application code did not change
between the two runs.

This closes the tested shared-supersede retry cases, not all MR-02 acceptance.
Other verbs/personal placement, review proposals, durable consumer application,
restore-owner policy and final-release freshness remain outstanding.

## Versioned update and MCP parity

Implementation `e1f1101130e4b68c0247ab107618d5ee8006a069` extends shared `update` with the same expected-version
and durable retry contract as `supersede`. Update now obtains its inherited
confidence from the canonical locked row read, removing the separate confidence
query/lock. The new path preserves confidence caps and authority checks; no
whole-request P95 claim is made from this query reduction.

The full memory race suite with required PostgreSQL fixtures and restricted-role
replay passes (53.198 seconds). Both verbs have actual concurrent-connection tests
for commit/response loss and uncommitted disconnection. Update tests cover
inherited confidence, hidden IDs, authority refusal, stale versions, all public
views, changed payloads, cross-verb key reuse and obsolete results. A frozen
schema-one supersede digest verifies compatibility with previously committed
receipts. No schema migration is needed for the additional verb.

The shipping MCP adapter had been dropping expected versions and retry keys and
reducing successful owner responses to plain text. It now forwards those fields
and preserves keyed receipts. Tool discovery advertises the fields, versioned
get and decimal-string IDs. Go omits the native host's audit request on a replay,
so a successful retry is not counted as another mutation. Legacy unkeyed output
retains its existing text format. Native forwarding tests preserve exact version
strings beyond 2^53, long text, error responses and the complete keyed receipt;
tool-schema tests, generated-document checks and memory/C-bus ownership gates pass.

A fresh T2 deployment on owned `.253` CT 9498 built that exact implementation
and harness as
`sha256:91233bef315cdb3228359c3ea1ce354828957f039aff7a760c40f577cb23049c`,
using the same pinned PostgreSQL and embedding images listed above. Its
[sanitized receipt](memory-shared-reliability-2026-09-20/fresh-t2-e1f1101130.json)
records **281 passing verdicts**: 61 private, 197 shared, six identity and 17
topology. The new real MCP workflow stores model-authored content, reads its
version using a decimal-string ID, applies a keyed update and retries it. It
verifies unchanged confidence/model authorship, the same canonical receipt,
no repeated host audit request, changed-payload and stale-version conflicts,
retired-result refusal and exactly two retained versions. Personal placement
explicitly refuses the shared preconditions. Existing HTTP supersede, restart,
scope-isolation and deliberate outage/recovery checks also pass.

Raw evidence is retained under
`/opt/aimee-memory-proposals-evidence/t2-e1f1101130` inside CT 9498. The disposable
application/embedding/database containers were stopped after validation; their
volumes and evidence remain. All further work remains on the single continuing
branch. Store/delete idempotency, review proposals, personal content versioning,
consumer progress and the rest of the MR-01–18 acceptance requirements remain open.

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
