# Proposal suite: Aimee core, modular source ownership, and product boundaries

- **State:** PENDING — roundtable-approved 2026-07-20; awaiting project acceptance
- **Author:** Aimee project
- **Date:** 2026-07-20

## Decision

Aimee is the shared memory, learning, routing, IR messaging, translation, execution, and safety
substrate used by agents and applications. We will make that substrate explicit, move feature code
out of broad historical `src/` buckets into owned modules, make non-core capabilities genuinely
optional, and remove complexity that has no supported non-self consumer.

This work is intentionally split. The former single proposal combined too many independently
reviewable decisions and made approval, sequencing, rollback, and acceptance ambiguous. This file
is now the suite index and shared contract; it does not duplicate the child proposals.

## Shared invariants

1. Core never depends on an optional module.
2. A capability is core only when removing it breaks Aimee's fundamental round trip, prevents the
   module architecture from functioning, or violates a non-negotiable security/correctness
   invariant.
3. Required modules have no user-facing enable switch. Replaceable providers may exist behind
   their contracts, but every required contract has a working reference implementation.
4. Optional means selectable at build/profile time and absent from the link/load closure when not
   selected. Runtime disablement is a separate declared capability.
5. New implementation belongs in `src/modules/<owner>/`; application directories are composition
   roots, not feature owners.
6. One descriptor graph drives Make, CMake, runtime registration, effective configuration, module
   documentation, and profile tests.
7. Every module has an authoritative individual document. Documentation and implementation change
   together.
8. Public compatibility is preserved unless an approved compatibility record says otherwise.
   Internal APIs have no compatibility entitlement and should be simplified aggressively.
9. A feature is not live merely because it registers, schedules, stores data, exposes config, or
   tests itself. Retention requires a supported journey or a production consumer outside its own
   feature cluster.
10. Less is more: remove duplicate implementations, registries, fallbacks, wrappers, stale config,
    and self-contained feature islands instead of relocating them.

## Shared terms

- A **required module** is present in every product profile and has no user-facing enable switch.
- An **optional module** is selectable and leaves no object, symbol, registration, route, asset,
  config, or background-work residue when omitted.
- A **provider** is a replaceable implementation behind a module-owned contract; replaceability of
  an implementation does not make the contract optional.
- A **profile** is a generated selection of modules and providers for a build/product shape.
- A **descriptor graph** is the validated set of module descriptors and their declared dependency,
  capability, source, config, surface, data, test, and documentation edges.
- An **object closure** is the complete selected `.o` set for a profile; a **capability closure** is
  the complete set of capabilities advertised by those selected modules and providers.
- **Capability state** is a typed lifecycle value: absent, selected, disabled, starting, ready,
  degraded, unavailable, stopping, or failed.
- A **supported journey** is a named, tested path from a production entrypoint to a user-visible or
  operational effect outside the feature's own cluster.
- **Truthful configuration** means every advertised setting is owned, active in the current module
  state, and read by production code; accepted legacy input need not be advertised.
- **Fail-closed** means an action is denied when authorization cannot complete. **Audit integrity**
  means append-only events have verifiable ordering and tamper evidence.
- A **compatibility record** is an approved, time-bounded exception that names affected surfaces,
  migration and recovery commands, retained artifacts, expiry, and owner.
- A **compatibility alias** is a descriptor-declared old name whose authority and expiry come from
  a compatibility record; the alias is a mechanism, never an independent promise.
- Acceptance tiers are `mechanical` (static/deterministic), `integration` (running components), and
  `hardware` (selected external model/accelerator providers).

Runtime-disabled and omitted are not synonyms. A selected module may remain in the build while its
declared runtime lifecycle is disabled; an omitted module must be absent from every build/link/load
and runtime surface named by the suite's absence manifest.

## Canonical module taxonomy

The required set contains exactly eighteen module IDs:

1. `module-runtime`
2. `config`
3. `ir`
4. `translation`
5. `protocols`
6. `gateway`
7. `memory`
8. `learning`
9. `routing`
10. `delegates`
11. `tools`
12. `workspace`
13. `git`
14. `skills`
15. `response-composition`
16. `vault`
17. `execution-policy`
18. `audit`

Core infrastructure that is not a feature module is limited to application composition roots,
small base/value primitives, platform shims, and generated contracts.

The initial optional set contains exactly eight concrete module IDs:

1. `plugin-loader`
2. `governance`
3. `workflows`
4. `roundtable`
5. `kb-synthesis`
6. `runtime-web`
7. `control-web`
8. `benchmarks`

`workflows` owns the `triggers`, `cron`, and `event-activation` capabilities; those are not
additional module IDs.

The two enumerations above define the inventory's bootstrap contents. Implementation creates
`tests/baselines/modules/canonical-inventory.yaml` as the single normative build/runtime inventory
with schema version 1 as the first taxonomy implementation step, directly transcribing these
enumerations. Before the Git child is accepted, the enumerations and core responsibility table must
have set equality with that artifact. After child acceptance, descriptors and generated profiles
must also have set equality. List order is editorial; dependency and build order come only from
descriptors. Unknown keys, aliases, count drift, and projection mismatch fail acceptance.
Enforcement is owned by acceptance ids 8 and 9 in
`module-runtime-source-ownership-and-build.md`; failure blocks profile generation, every child
migration slice, and CI success.

Individual skill packages are optional content, not architectural modules. MCP and ACP adapters
are required protocol implementations. A channel, backend, or provider does not become core merely
because its core contract requires one reference implementation. Extension categories and
hypothetical future implementations are not modules. A provider or adapter may remain optional
behind a required module contract without becoming a module ID; entering the module taxonomy
requires a concrete ID and an amendment to the canonical inventory.

## Product boundary

- **Aimee Runtime** (`aimee-runtime`) replaces `aimee-server` as the per-user interaction and agent
  execution boundary.
- **Aimee Control Plane** (`aimee-control`) replaces `aimee-kb` as the multi-tenant management,
  governance, shared-memory, and fleet boundary.
- `runtime-web` and `control-web` are independent optional modules, enabled by default. Each GUI
  includes its dashboard; there is no separate dashboard switch. Either product can run headless.
- Old product/config names receive bounded compatibility aliases; new code may not introduce them.

The optional `governance` module owns federated OIDC/SSO, organizational identity and roles,
governance policy authoring/distribution, approvals and decision records, posture profiles,
attestation/evidence surfaces, agent/delegation identity chains, fleet governance, and executable-
artifact trust. It consumes core principal, vault, execution-policy, audit, gateway, protocol,
routing, delegate, tool, and config contracts. Core retains local principal/tenant handles,
fail-closed action enforcement, credential custody, transport authentication, and audit-ledger
integrity, so disabling governance removes the organizational governance plane without weakening
the core safety boundary.

The governance program includes
[`governance-attestable-enforcement.md`](governance-attestable-enforcement.md),
[`governance-policy-surface-and-posture.md`](governance-policy-surface-and-posture.md),
[`governance-agent-identity-and-artifact-trust.md`](governance-agent-identity-and-artifact-trust.md),
and [`tiered-llm-p5-oidc-control-plane.md`](tiered-llm-p5-oidc-control-plane.md), plus their plans and
follow-ups. Those proposals are implemented through `governance`; when they strengthen a required
safety invariant, the underlying enforcement or ledger change lands behind the owning core contract
rather than making that safety property optional.

## Proposal map and order

1. [`feature-liveness-and-background-curator-removal.md`](feature-liveness-and-background-curator-removal.md)
   defines evidence rules and removes the current background skill-curation job, whose schedules,
   state, metrics, and tests do not lead to a supported non-self consumer. It can land first.
2. [`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md)
   establishes descriptors, dependency enforcement, generated builds, documentation gates, and
   the physical source boundary.
3. [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md) defines the eighteen
   required module contracts and executable core round trip.
4. [`memory-learning-and-inference-boundaries.md`](memory-learning-and-inference-boundaries.md)
   owns code intelligence, required inference, adaptive learning, skills, response composition,
   and optional KB synthesis.
5. [`product-governance-web-and-config.md`](product-governance-web-and-config.md) owns the Runtime /
   Control Plane rename, governance split, web lifecycles, and truthful configuration surfaces.
6. The governance program—[`governance-attestable-enforcement.md`](governance-attestable-enforcement.md),
   [`governance-policy-surface-and-posture.md`](governance-policy-surface-and-posture.md),
   [`governance-agent-identity-and-artifact-trust.md`](governance-agent-identity-and-artifact-trust.md),
   and [`tiered-llm-p5-oidc-control-plane.md`](tiered-llm-p5-oidc-control-plane.md)—owns the optional
   governance feature design and depends on proposals 2–5's core/module/product boundaries.
7. [`large-refactor-delivery-and-compatibility.md`](large-refactor-delivery-and-compatibility.md)
   sequences the moves and defines compatibility, cleanup, recovery, and completion gates.

`git-core-contract.md` is a required forthcoming child of proposal 3 and must be accepted before
the Git migration slice begins. It owns Git API, event-production, mutation, security,
compatibility, workspace/memory seams, and executable fixtures; this suite decision owns only
Git's required-core classification and memory's continued ownership of code intelligence.

Proposals 2–5 may be reviewed in parallel; proposal 6 follows their boundary decisions, and
implementation follows the dependency order recorded in proposal 7. Approval of one child does not
imply approval of another. No child proposal may
redefine this suite's taxonomy or shared invariants; changing them requires updating the suite
index and every affected child in one review.

## Suite-level completion

The program is complete only when every child proposal is accepted and its binding checks pass.
The core-contract proposal owns the `core` profile; the product proposal owns `runtime` and
`control`; the delivery proposal owns `full` and full-minus-one. The module/build proposal owns
Make/CMake object equality and individual module docs; the product proposal owns headless operation;
the liveness and delivery proposals own dispositions, cleanup ledgers, compatibility, and recovery.
At completion, omitted optional modules leave no residue and feature implementation no longer lives
in broad `src/`, `src/server/`, `src/kb/`, `src/db1/`, `src/db2/`, or global-header buckets.

## Review status

Earlier roundtable approvals applied to revisions of the former monolithic proposal. They are
useful review history, not approval of this split suite.

The split suite completed fresh review on 2026-07-20. The technical writer rejected initial
boundaries around stage/config/deletion ownership and then approved all three bounded document
groups on the final revision. Architecture rejected optional Git/workflow leakage, weak learning and
skills admission, Control/core ambiguity, web-alias behavior, and the KB-synthesis write boundary;
the revised suite resolved each and received **APPROVED**. Adversarial review then rejected vacuous
readiness/config/docs/cleanup gates, curator resurrection paths, alias and web leakage, unsigned
canonical changes, and incomplete tenancy/recovery proofs; the hardened revision received
**APPROVED**. Verification independently approved the core/memory, module/product, and
liveness/delivery groups, covering the complete suite without review-payload truncation.

All technical-writer, architecture, adversarial, and verification approvals apply to the same
revision. Roundtable approval means the suite is coherent and executable enough to seek project
acceptance; it does not bypass per-slice review or the binding gates in each child.

Post-approval terminology amendments renamed optional `evals` to `benchmarks` and added optional
`governance`. Focused technical-writing, architecture, adversarial, and verification reviews
approved both amendments after governance ownership, dependency direction, absent-module behavior,
and the core safety boundary were made normative. These amendments therefore retain the suite's
roundtable-approved state.

A further focused amendment promoted `git` to required core, fixed the inventory at eighteen
required and eight optional module IDs, and preserved code-intelligence ownership in `memory`.
Technical-writing review approved the final phased wording. Roundtable review rejected the earlier
mixed taxonomy/implementation gate, then approved the revision after pre-child taxonomy checks and
post-child Git implementation checks were separated; its final artifact reported no issues, with
zero surviving findings after replay verification.
