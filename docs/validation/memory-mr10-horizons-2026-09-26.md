# MR-10: deterministic utility horizons

Validation in progress. This report records implementation scope; it does not
claim a completed deployment or promotion gate until the evidence below is added.

The Go owner integrates the pure evaluator with protected MR-02 creation/update
journal anchors. Operator configuration is bounded, versioned and unavailable
as a request parameter. Exact-version overrides and explicitly admitted
confirmation events cannot be renewed by serving, access counters, stale
versions or fabricated events. Shared eligibility applies the horizon before
lexical/dense/graph/bundle limits; source revalidation independently checks it.
Collection observations also bind policy digest and the next horizon boundary.

Historical inspection remains subject to canonical scope, lifecycle and erasure
rules. Ordinary horizon exclusion is distinct from validity expiration and
physical deletion. No policy is enabled by default, and no fitted domain policy
is promoted. The operator configuration and rollback contract are documented in
[the memory guide](../modules/memory.md#utility-horizons-mr-10).

Focused PostgreSQL/race fixtures exercise both placements, exact boundary,
current versus retained historical reads, pre-limit lexical/dense/graph/bundle
exclusion, durable-kind survival, counter stability, override/safety precedence,
changed-version rejection, confirmation events, release refusal, collection
boundary and policy invalidation, and revocation after policy rollback. Pure
fixtures additionally cover missing/malformed/future anchors and policy identity.
