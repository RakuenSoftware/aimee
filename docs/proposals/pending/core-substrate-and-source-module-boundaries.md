# Proposal suite: Aimee core, modular source ownership, and product boundaries

- **State:** PENDING — roundtable-approved 2026-07-20; **amended 2026-07-23 (post-approval)**.
  The 2026-07-23 amendment introduces the C-core / Go-module language boundary and the core-owned
  in-memory event bus as the single inter-module messaging construct. It reopens the taxonomy and
  shared invariants and must be re-reviewed before acceptance; it does not inherit the 2026-07-20
  approval.
- **Author:** Aimee project
- **Date:** 2026-07-20 (amended 2026-07-23)

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
11. **The communication core is written in C; every feature module is written in Go.** Language and
    selection are independent axes: a module may be required (always selected) yet still be a Go
    module. The C core carries authenticated, audited, typed messages between participants and hosts
    the event bus; it performs no feature work.
12. **Modules do not link or call each other directly.** A module reaches another module — and core
    reaches a module — only by publishing or requesting a typed event on the core-owned in-memory
    event bus, gated by the descriptor dependency graph and execution policy. There is no shared
    header, symbol, link edge, or side channel between modules. The C↔Go boundary makes this
    structural: the C core cannot link a Go module, and one Go module cannot link another across the
    descriptor boundary.
13. **The event bus is the single loggable, governable, recordable messaging construct.** Every
    inter-module message is a bus event, so one tap observes, records, authorizes (for action-class
    events), and can **replay** the entire cross-module message stream. No module-to-module
    communication may bypass it. Deterministic record-and-replay of the event stream is a
    first-class capability — the primary debugging and reproduction surface for the whole system.
14. **`memory` is a hub, not a peer.** Nearly every module depends on `memory`; `memory` depends on
    no other feature module. It is a sink in the dependency graph, keeping the graph acyclic under
    broad fan-in, and its public event contract stays narrow despite that fan-in.
15. **The bus stays within a performance budget.** In-memory, in-process dispatch with no network or
    serialization on the intra-service hot path; a benchmark gate bounds per-event dispatch overhead
    and holds the `memory` round trip at parity with the former in-process call. Governance capture
    and record must not push the hot path outside budget (record is asynchronous; only action-class
    verdicts are synchronous and cheap).
16. **Installation is dependency-complete.** Each of the Runtime and the Control Plane owns its own
    event bus; core is the bus owner in each. A module registers by publishing its capabilities to
    core over that bus, and a module may not be installed unless every module it declares a
    dependency on is already installed. Installation and selection are transactional and fail closed
    on a missing dependency; a module cannot be removed while an installed module still depends on
    it.
17. **Bus admission is restricted, authenticated, and core-controlled.** The bus is not a promiscuous
    endpoint any process can dial. Core is the sole admission authority; a participant attaches only
    with an attested identity and only when installed, registered, and authorized. There is no
    anonymous or ambient attach — an arbitrary local process cannot connect, enumerate, publish, or
    subscribe. Admission is least-privilege (an admitted participant is still confined to its declared,
    authorized event kinds) and fail-closed (a refused attach is denied and audited).

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
- The **communication core** is the C substrate that carries authenticated, audited, typed messages
  between participants (client, Runtime, Control Plane, and modules), hosts the event bus, and holds
  capability state. It contains no feature capability.
- The **event bus** is the core-owned, in-memory, in-process message path over which participants
  exchange typed events. It supports one-way notifications and correlated request/reply, resolves a
  target by capability, authorizes and records the hop, and returns typed `capability_absent` when a
  target is unselected or not `ready`. It is per-service (one in each of Runtime and Control Plane);
  cross-service events travel the existing network transport, not the in-memory bus.
- An **event contract** is the language-neutral schema for the event kinds a module publishes,
  subscribes to, and may request. It is the only surface another participant may invoke, and it
  replaces the C public header as the enforced dependency edge for Go modules.
- A **hub module** is a module (canonically `memory`) that is an allowed dependency of many other
  modules and itself depends on no feature module — a sink in the dependency graph.
- The **object closure** term extends per axis: the **C object closure** is the selected `.o` set
  for the communication core; the **module closure** is the selected set of Go module build units
  and their registered event contracts. Omission removes a module from the module closure and the
  capability closure alike.

Runtime-disabled and omitted are not synonyms. A selected module may remain in the build while its
declared runtime lifecycle is disabled; an omitted module must be absent from every build/link/load
and runtime surface named by the suite's absence manifest.

## Language boundary and the inter-module event bus (2026-07-23 amendment)

The suite's original decision placed feature capabilities *inside* core. This amendment narrows core
to exactly what the first message of this work asked for: **the pieces needed to communicate between
the client, the Runtime, and the Control Plane.** That substrate is written in C. Every capability
that does work in response to a message — including `memory` — is a **Go module** that communicates
over a **core-owned in-memory event bus**, not a C-linked part of core.

### Two independent axes

Selection and language are orthogonal. Each module is classified on both:

- **Selection:** *required* (present in every profile, no enable switch) or *optional* (selectable,
  absent from the module and capability closure when omitted). Unchanged from the approved suite.
- **Runtime/language:** *communication core* (C, in the trusted message path) or *module* (Go,
  behind an event contract on the bus). New with this amendment.

"Required" no longer implies "compiled into C core." A required Go module is always selected and
always registered, but it is still a Go build unit that speaks only over the bus. The eighteen IDs
the approved taxonomy called "required core modules" split across the new language axis; the final
assignment is delegated to [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md),
which must resolve it before its round-trip proof is built. This suite records the intended carving:

- **Communication core (C):** `module-runtime` (now also the event bus and capability-state
  authority), `ir`, `translation`, `protocols`, `gateway`, and `config`. These move a typed message
  and nothing else.
- **Required Go modules:** `memory`, `learning`, `routing`, `delegates`, `tools`, `workspace`,
  `git`, `skills`, and `response-composition`. Always selected; still Go; still on the bus.
- **Trust kernel (pivotal):** `vault`, `execution-policy`, and `audit` gate and record every bus
  event. This suite's recommendation is to keep them in the C communication core, because the bus
  cannot authorize or record an inter-module event without them and the safety boundary must not
  depend on a module the bus is trying to reach. The capability-contract child owns the final
  placement and must state it explicitly; wherever it lands, the safety contracts and their
  reference implementations remain required in every profile.

All eight optional modules remain Go modules.

### The in-memory event bus

`module-runtime` owns an **in-memory, in-process event bus** per service. Every message between
participants — a remote client, the Runtime, the Control Plane, or another module — is a typed IR
event on this bus. A participant publishes a one-way notification or a correlated request and
receives the reply; the bus resolves the target by capability, authorizes the event through the
trust kernel, offers it to the governance/audit tap, and returns the typed result or
`capability_absent`. The same IR envelopes, auth, and capability advertisement drive both the C↔Go
boundary and the client↔server↔kb boundary, so a module invoking `memory` and a client invoking the
Runtime traverse one construct, not two. Cross-service events (Runtime↔Control Plane) leave the
in-memory bus and travel the existing network transport; the in-memory guarantee is intra-service.

Choosing a bus over point-to-point calls is deliberate: one construct carries every inter-module
message, so it is the single place to **log, govern, and reason about** cross-module behavior
(shared invariant 13). It also structurally enforces invariants 1 and 12 — the C core has no build
or link edge to any Go module; it holds only the descriptor graph and the bus. "Core never depends
on an optional module" becomes unbreakable rather than merely checked: core *cannot* link a module
at all. An unselected module simply has no registered event contract, and every attempt to reach it
fails closed.

Cross-boundary readiness is answered by the capability advertisement defined in
[`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md): before a
module requests `memory` (or any dependency), it observes that dependency's `ready` state through
the same generation-stamped projection the thin client uses. Discovery, for a module and for a
client, is one mechanism.

### Bus admission and isolation

The bus carries every module's traffic, including the near-universal `memory` path and every
governed action-class event, so **who may attach to it is a security boundary in its own right** —
distinct from, and prior to, the per-event authorization above. A process that could connect freely
could observe every module's messages or inject its own; the bus must therefore not be a
promiscuous endpoint any process can dial (shared invariant 17).

- **Core is the sole admission authority.** As bus owner in each service, core admits or refuses
  every participant. There is no side channel to join and no anonymous or ambient attach.
- **Every participant is identity-attested.** Admission reuses the existing principal and transport
  classes — the vault principal model and the `cert:CN` / bearer attestation the thin-client↔server
  link already uses ([`tiered-llm-p8-thinclient-mtls.md`](tiered-llm-p8-thinclient-mtls.md)) — rather
  than inventing a second identity scheme. In-process built-in modules are admitted under the
  service's own principal at load; an out-of-process or externally authored module (via
  `plugin-loader`) attaches only through an access-restricted local endpoint and a per-module
  authenticated handshake. An arbitrary local process holds no such identity and is refused.
- **Admission is gated by installation and authorization.** A participant is admitted only when it
  is installed, registered, and authorized by `execution-policy`; a module that is not installed, or
  whose identity or dependencies do not check out, cannot attach. This is the connection-layer gate;
  the per-event contract check (declared, authorized event kinds only) still applies afterward, so
  admission never implies full access.
- **Isolation and least privilege.** An admitted participant sees only the event kinds it is
  authorized to subscribe to — the bus is not a broadcast every attached participant can read in
  full. A compromised or malicious local process cannot attach, enumerate participants, snoop
  traffic, or publish.
- **Fail-closed and audited.** A refused or failed attach is denied and recorded through the same
  tap as any other governed event, so an unexpected connection attempt is visible, not silent.

This contract sets the admission invariant and its reuse of existing identity machinery; the
mechanics of the local endpoint, the handshake, and out-of-process module attachment are owned by
`module-runtime` and `plugin-loader` in their documents.

### Record, replay, and debugging

Because every inter-module message is one typed event on one construct, the bus is a complete,
ordered record of the system's cross-module behavior — and that record is **replayable**. The bus
supports capturing the event stream (per service, within the performance budget) and re-driving it
against modules to reproduce a run deterministically. This is a first-class capability, not a
side effect (shared invariant 13):

- **Debugging.** One ordered stream shows exactly what every module published, requested, and
  received, with the trust-kernel verdict on each hop. A failure is inspected on the recorded stream
  instead of reconstructed from scattered per-module logs.
- **Reproduction and test.** A recorded stream replays a production incident or seeds a regression
  test without the original environment; module behavior is exercised against real captured events.
- **Forensics.** Governance replays a window of the stream to see the exact sequence that led to a
  verdict (complementing, not replacing, the durable audit chain in
  [`governance-attestable-enforcement.md`](governance-attestable-enforcement.md)).

Record and replay are deterministic with respect to the captured stream: replay presents the same
events in the same order; module non-determinism (clocks, randomness, external I/O) is captured as
events or stubbed at replay so a replayed run does not diverge silently. Capture obeys the same
redaction and principal-scoping as the audit tap; a recorded stream is a governed artifact.

### Performance budget (the speed constraint)

The bus is superior only while its cost stays within acceptable limits, and the hottest path —
module→`memory`, run on every request on both Runtime and Control Plane — is the one that must not
regress (shared invariant 15). The in-memory, in-process design is what makes this viable: intra-
service events cross no network and require no wire serialization, so the boundary crossing that used
to be an in-process call stays close to one. A benchmark gate bounds per-event dispatch overhead and
holds the `memory` round trip at parity with the former in-process call; the round-trip proof
exercises the `memory` stages *across* the C↔Go boundary, not as an in-proc shortcut.

Governance capture must live within this budget. The tap observes every event, but **recording** to
the durable audit chain is asynchronous and batched (consistent with the WORM hot-path cost noted in
[`governance-attestable-enforcement.md`](governance-attestable-enforcement.md)); only **action-class**
events require a synchronous pre-delivery verdict, and that verdict is a cheap in-memory policy
check. High-frequency data events such as `memory` recall are observed and recorded, not gated
synchronously, so completeness does not tax the hot path.

### `memory` as the hub

Because nearly every module depends on `memory`, the module→`memory` event is the busiest edge in the
graph and the one most at risk of becoming a bottleneck or a god-object. This amendment fixes three
rules (shared invariant 14):

1. **`memory` depends on no feature module.** It is a sink; the graph stays acyclic under broad
   fan-in. Code intelligence, embedding, and reranking stay owned by `memory` (unchanged), but
   `memory` requests nothing from another module over the bus.
2. **The `memory` event contract stays narrow.** Heavy fan-in does not license a wide surface: typed
   ingest, recall, index, embed, and rerank events only. Callers adapt to `memory`; `memory` does not
   grow an event kind per caller.
3. **The bus gives `memory` a first-class local fast-path.** Recall and ingest support batching and
   streaming so the in-memory crossing does not regress a hot path, per the performance budget above.

### Capability publication and dependency-complete installation

Each of the Runtime and the Control Plane owns its own in-memory event bus, and core is the bus
owner in each (shared invariant 16). A module does not have its capabilities read out of it by a
core poller; it **publishes its capabilities to core over the bus** when it registers, and publishes
state transitions as they happen. Core aggregates those publications into the capability closure and
the generation-stamped advertisement that
[`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md) projects to the
thin client and to other modules. Publication and advertisement are the same bus mechanism observed
from two ends.

Installation is **dependency-complete** and transactional. Each module declares its module
dependencies in its descriptor; a module may not be installed unless every module it depends on is
already installed, and it may not be removed while an installed module still depends on it. The
installer/profile generator computes the dependency closure and **refuses** an install or selection
that would leave a declared dependency unmet, naming the missing module — it never installs a module
into a state where a dependency it needs is absent. This is distinct from runtime readiness: a
dependency being *installed* is an install-time precondition; a dependency being *ready* is the
runtime condition the capability advertisement reports. Because `memory` is a dependency of nearly
every module, `memory` installs before its dependents and cannot be removed while any of them
remain.

### User-authored modules

Because the bus, the event contract, and capability publication are the *only* integration surface,
a module needs nothing from core but to speak that surface: subscribe to and publish its declared
event kinds, and publish its capabilities. This theoretically lets **end users author their own
modules** and plug them in without modifying, recompiling, or relinking core — the same boundary
that isolates the built-in Go modules admits a third-party one. The optional `plugin-loader` module
is where this is realized (loading and lifecycle of externally authored modules); this suite records
the property, and `plugin-loader`'s own document owns the packaging, loading, and sandbox mechanics.

The trust boundary does not soften for a user module — it is exactly why the bus boundary makes this
safe to contemplate:

- A user module is an **untrusted principal**. Every event it publishes or requests is authorized by
  `execution-policy` and recorded by `audit` through the same tap as any other event; it gets no
  ambient access and can reach another module (for example `memory`) only through that module's
  public event contract, only for event kinds it declared, and only if the dependency is installed.
- **Dependency-complete installation** applies unchanged: a user module that depends on `memory`
  installs only when `memory` is present and reaches it solely over the bus.
- Executable-artifact trust — signing and hash-pinning of externally authored modules — is owned by
  optional `governance`
  ([`governance-agent-identity-and-artifact-trust.md`](governance-agent-identity-and-artifact-trust.md));
  a deployment that requires signed modules enforces it there, over the same install and bus
  contracts.

Nothing about user-authored modules is a new core capability or a new privilege path; it is the
existing bus boundary observed from outside the project.

### What this changes downstream

- [`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md)
  owns the polyglot build (C core plus Go module builds from one descriptor graph), the event
  contract schema, bus ownership, and dependency enforcement re-expressed as authorized event
  publication/subscription rather than only a C link/symbol graph.
- [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md) owns the final C/Go
  carving of the eighteen IDs, the trust-kernel placement, and a round-trip proof whose stages flow
  as bus events across the boundary within the performance budget.
- [`governance-attestable-enforcement.md`](governance-attestable-enforcement.md) gains the bus as a
  single, uniform capture and enforcement seam, replacing the seven scattered enforcer sinks its A2
  inventory routes into the chain one by one.
- [`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md) is the
  discovery and readiness mechanism modules use to reach each other, not only the thin client.

## Canonical module taxonomy

The module IDs and their selection (required/optional) are unchanged by the 2026-07-23 amendment;
only their language/runtime placement and their communication mechanism (the event bus) are added
above. The required set contains exactly eighteen module IDs:

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
8. [`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md) owns the
   runtime capability-advertisement surface: how a Runtime and a Control Plane project their live
   capability closure and state to a connecting thin client, refresh it on change, and how the thin
   client merges both and re-advertises the effective set to its consumer. It consumes proposals
   2–5's capability-state, config, product, and protocol contracts and adds no taxonomy. It was
   drafted after the 2026-07-20 suite review and awaits its own roundtable review; it does not
   inherit the suite approvals.

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
