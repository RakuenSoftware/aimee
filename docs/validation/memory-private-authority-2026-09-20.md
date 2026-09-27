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

## Fresh enrolled deployment

Implementation and harness `4294b04248` passed **364/364** checks on a new
owned T2 environment on `.253`: 103 private, 208 shared, 29 correction-review,
six identity and 18 topology checks. The
[sanitized receipt](memory-shared-reliability-2026-09-20/fresh-t2-4294b04248.json)
contains only verdict names and booleans.

Both application containers were verified against
`sha256:a0ff80b7484000d2c9e6b5ebdde45639670128b4d96deade38775c86c14957cb`;
both PostgreSQL containers used
`sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
The embedder remains pinned to
`sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209`.
Raw evidence remains at `/opt/aimee-memory-proposals-evidence/t2-4294b04248`
in owned CT 9498. The six verified containers are stopped; volumes and evidence
are retained.

The new checks exercise HTTP-authored records against MCP correction, upsert and
retirement attempts; untrusted provenance fields and certainty; successful
model-authored corrections; retained author identity; runtime erase restrictions;
and refusal without changed canonical revisions. Existing outage, rollback,
restart, shared-isolation and review tests also pass.

CI exposed three older fixture assumptions, corrected by harness `b4bc144cdd`:
the native integration test now requires refusal of MCP edits to a user's record
and separately tests successful MCP model corrections; the token-TTL rig uses
a separate key for the UDS user assertion; the semantic-expiry fixture supplies
explicit administrative authority when changing its controlled temporal boundary.
The application image is unchanged by these fixture corrections.

## Fresh standalone semantic and exploratory deployment

Harness `b4bc144cdd`, using the unchanged application/PostgreSQL/embedder images
above, passed **144/144** checks in a new T3 environment on `.253`: 103 private,
15 real-model semantic, 21 exploratory and five topology checks. Its
[sanitized receipt](memory-shared-reliability-2026-09-20/fresh-t3-b4bc144cdd.json)
contains no record payloads or credentials. The application and PostgreSQL image
IDs were verified independently on this deployment too.

The semantic gate verifies nonlexical retrieval, CLI recall, Server restart,
embedder outage/recovery and exclusion of expired or retired vectors. The
exploratory gate runs 24 concurrent Unicode writes, verifies HTTP/CLI/MCP exact
int64 IDs, suspends and terminates the Go memory owner, and checks bounded failure,
application liveness, supervised recovery, preserved committed content and retired
record exclusion. Raw evidence remains in
`/opt/aimee-memory-proposals-evidence/t3-b4bc144cdd` in owned CT 9498. Its three
containers are stopped; volumes and evidence are retained.

The corrected token-TTL CI job passed. Native integration exercised the intended
user refusal, authorized user correction, model creation/correction and owner
restart successfully; one assertion mistakenly supplied a regex to the helper's
literal matcher. The assertion now validates the returned positive decimal ID
explicitly. Full CI must finish on the resulting commit before a green claim.
