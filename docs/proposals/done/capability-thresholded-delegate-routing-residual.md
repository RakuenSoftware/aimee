# Capability-thresholded delegate routing: residual work

- **State:** DONE. Implementation and validation gates delivered 2026-09-06.
- **Implemented:** 2026-09-06.

**Archived parent:** [`capability-thresholded-delegate-routing.md`](../done/capability-thresholded-delegate-routing.md)

## Delivered behavior

1. **Persisted task competence, independent of price and size.** The Go provider
   store accepts per-model `competence` assessments with integer scores (0–100)
   and required evidence sources. Fleet `role_contracts` set minimum scores (1–100).
   Missing assessments cannot satisfy a contract. Evidence is bound to its model ID
   and survives native compatibility saves; replacing the model invalidates it.

2. **Qualified learning and context-band cost selection.** The existing routing
   process now has a cost-selection stage (event 6402). It ranks the eligible pool
   with initial request input/output estimates, strict context-band boundaries,
   independent price overrides, explicit free prices and unknown-price handling.
   The `delegate_routing_v2` learner selects cost or competence preference within
   that same pool. Historical price-tier evidence stays separate. No additional
   model invocation classifies the task or predicts the route.

3. **Registration-scoped health.** Credential/subscription failures affect sibling
   targets of the stored registration. Model failures remain local, and a different
   account on the same provider/URL remains eligible. Legacy transport diagnostics
   also use registration keys, with synchronized access and no overflow aliasing.

4. **Roles as task contracts.** Existing canonical role names remain compatible;
   their grants now intersect competence requirements and the existing capability,
   context, scope, health and policy filters. Wildcards, tier/name/provider overrides,
   retries and escalation cannot bypass a competence threshold. A model-ID override
   cannot inherit another model's assessment. No qualified candidate produces a
   deterministic refusal rather than a below-threshold dispatch. Model diagnostics
   and delegate results expose the qualification facts and routing policy version.

5. **Validation gates.** Go tests cover price bands, free versus unknown prices,
   competence preference, malformed inputs, stable fallback and policy persistence.
   Native tests cover the actual provider-store-to-router path and registration
   health isolation. C/Go bus conformance exercises the new stage in the separately
   running routing process. These are included in the existing CI aggregate. An
   opt-in real-provider acceptance runner exercises `/v1/delegate/run`; its contract
   tests are included in CI.

## Rollout and limits

Contracts are opt-in per task role, so deployments can supply evidence and introduce
thresholds incrementally. Scores are operator or benchmark assessments on a declared
rubric, not fabricated calibration data. No production roster, credentials, benchmark
scores or live-learning switches are changed by this implementation.

Cost ordering uses an estimate of the initial dispatch, with uncached input pricing;
it is not a total-run budget guarantee. Unknown prices are never interpreted as free.
Existing explicit/default delegate preferences and local/health preferences remain
applicable. The retired automatic verifier-triggered escalation remains retired.

Isolated full-stack VM/CT acceptance on `.253` passed using a deterministic external
completion endpoint. This exercises real routing, enrollment, storage and sandbox
execution, but does not measure vendor model quality, calibration or billing.

Configuration and exact commands: [routing module](../../modules/routing.md).
Live-provider cases and invocation: [provider validation](../../../scripts/validation/providers/README.md).

## Acceptance evidence

- Go routing/provider tests: passed.
- Native agent and provider catalog tests: passed.
- Full Go gate, including C-to-Go bus conformance: passed.
- Module descriptors, process contracts and event durability checks: passed.
- Live-gate contract tests: passed.
- Full native unit suite: passed (654 executables, including sanitizer variants).
- Fresh Debian VM and CT on `.253`: twelve routing scenarios each passed, including
  real sandbox execution and routing-process failure/recovery; all owned guests and
  volumes removed. [Evidence and limits](../../validation/intelligent-routing-on-253.md).
- Native-wire and actual PostgreSQL durable job-completion regressions: passed in
  both guests. Full local Go gate passed after the live-discovered completion fix.
