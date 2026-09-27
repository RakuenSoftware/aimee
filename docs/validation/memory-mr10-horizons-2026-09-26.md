# MR-10: deterministic utility horizons

Functional implementation and acceptance complete on `f6115607c`. Optional
utility-horizon policy remains disabled; representative domain tuning and
promotion remain subject to MR-18.

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

## Validation completed before deployed provider replay

Code commits `23f695283`, `052d7520f` and `f6115607c` implement the
integration, activation/pending-release guards and microsecond clock precision.
The full PostgreSQL memory race suite passed in 359.301 seconds on `052d7520f`;
the exported owner passed in 5.646 seconds. The final precision-only change
passed the focused horizon PostgreSQL/race suite in 1.834 seconds. Private
migration registry checks passed. All 77 lint checks ran; the two initial
registration failures (schema ownership hash and module descriptor) were fixed,
and both affected checks passed when rerun. No interrupted run counts as a pass.

The first deployed replay on `23f695283` passed 40 API checks. Later native
provider fixtures exposed inadequate positive controls: constraint records did
not occupy the identity channel, and the ordinary recall budget retained only
the newest identity probe. These failed attempts do not establish provider
exclusion. The final fixture orders the transient probe first in recall and uses an
explicit 8,192-token recall allocation, restored afterward. At the default
allocation, decision metadata caused budget omission, which is not evidence of
horizon enforcement. Shadow must deliver the transient probe; enforcement must
exclude it and deliver the durable probe at the same controlled allocation.
A retry also confirmed that retired KB fixture keys cannot be silently reused;
subsequent runs use distinct keys. Every attempt restores the model roster,
disables optional policies and removes its synthetic records.

Domain/kind duration tuning and promotion remain unqualified pending the MR-18
representative task-outcome gate. Synthetic functional checks do not establish
answer quality, population false-exclusion rates or billing cost. No fitted or
catch-all production age policy is enabled.

## Final deployed result

The final replay passed **109/109 checks**, with process exit zero. Shadow
provider bytes contained both private and shared transient and durable probes;
enforcement excluded both transient probes while retaining both durable probes.
Exact payload hashes matched committed preparation receipts, collection sources
bound the effective policy digest, and each dispatch had a durable started
receipt. The fixed-provider response completed through the native turn path.
Both recall allocation settings and the model roster were restored.

Disabling the policy restored all eight fixture records at their unchanged
canonical versions before cleanup. An independent final audit confirmed both
canary owners healthy on `aimee-pr2990:f6115607c`, with native CLI version
`v0.4.5-pr2990.f6115607c` and horizon, selector and health flags all unset.
CT100 remained healthy on `aimee-native-core:0.4.5-bridge.2`, with healthy 0.4.5
embedder/PostgreSQL containers; the local paired thinclient status returned zero.
This validates configuration rollback, not an older binary reading the new
private schema version 39.

[Checks, failed attempts, frozen harnesses and hashes](memory-mr10-evidence-2026-09-26/README.md)
make the functional gate reviewable. Production population outcomes are not
inferred from these synthetic probes.
