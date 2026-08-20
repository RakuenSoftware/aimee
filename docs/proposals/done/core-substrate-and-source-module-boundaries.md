# Proposal suite: Aimee core, modular source ownership, and product boundaries

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`core-substrate-and-source-module-boundaries-residual.md`](../pending/core-substrate-and-source-module-boundaries-residual.md).

- **State:** DONE — delivered scope archived 2026-07-26.
  The 2026-07-23 amendment makes core a C communication substrate and every module a separate program
  in any conforming language, communicating over a core-owned shared-memory event bus (no
  cross-language linking, no cgo). It reopens the taxonomy and shared invariants and must be
  re-reviewed before acceptance; it does not inherit the 2026-07-20 approval.
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
11. **The communication core is written in C; a module may be written in any language that conforms
    to the bus contract.** The boundary between core and a module — and between two modules — is a
    language-neutral message contract, not a header or a link edge, so language is a free choice.
    First-party modules standardize on Go as the reference implementation language; that is a
    convention, not a requirement. Language and selection are independent axes: a module may be
    required (always selected) and written in any conforming language. The C core carries
    authenticated, audited, typed messages between participants and owns the event bus; it performs no
    feature work.
12. **Modules do not link or call each other; they only exchange messages over the bus.** A module
    reaches another module — and core reaches a module — only by publishing or requesting a typed
    event on the core-owned shared-memory event bus, gated by the descriptor dependency graph and
    execution policy. There is no shared header, symbol, link edge, cgo boundary, or side channel
    between core and a module or between two modules. Because the boundary is a message contract and
    the participants are separate programs, no cross-language linking exists and cgo is never required.
13. **The event bus is the single loggable, governable, recordable messaging construct.** Every
    inter-module message is a bus event, so one tap observes, records, authorizes (for action-class
    events), and can **replay** the entire cross-module message stream. No module-to-module
    communication may bypass it. Deterministic record-and-replay of the event stream is a
    first-class capability — the primary debugging and reproduction surface for the whole system.
14. **`memory` is a hub, not a peer.** Nearly every module depends on `memory`; `memory` depends on
    no other feature module. It is a sink in the dependency graph, keeping the graph acyclic under
    broad fan-in, and its public event contract stays narrow despite that fan-in.
15. **The bus stays within a performance budget.** The bus is an in-memory shared-memory transport:
    lock-free ring buffers in a shared segment, zero-copy payloads, and no per-message syscall on the
    fast path — near-in-process latency without a shared address space. A benchmark gate bounds
    per-event dispatch overhead and keeps the `memory` round trip small; splitting `memory` into its
    own program costs more than the former monolithic in-process call, so batching and streaming keep
    that cost bounded rather than pretending it is zero. Governance capture and record must not push
    the hot path outside budget (record is asynchronous; only action-class verdicts are synchronous
    and cheap).
16. **Installation is dependency-complete for hard dependencies.** Each of the Runtime and the
    Control Plane owns its own shared-memory event bus; core is the bus owner in each. A module
    registers by publishing its capabilities to core over that bus, and a module may not be installed
    unless every module it declares a **hard** dependency on is already installed. Installation and
    selection are transactional and fail closed on a missing hard dependency; a module cannot be
    removed while an installed module still hard-depends on it. A **soft dependency** is declared
    separately: the module uses the target capability when present but must function without it via a
    declared fallback, so a soft dependency never blocks install or removal. `module-loader`'s use of
    optional `governance` artifact trust (falling back to a hash-pin baseline) is the canonical soft
    dependency; an optional module may soft-depend on another optional module, but never hard-depend on
    one it could be selected without.
17. **Bus admission is core-controlled; the shared segment is not open.** The shared-memory bus is
    not a segment any process may map. Core is the sole admission authority: a module is granted the
    bus handle and its own queue mappings only when it is installed and registered, its
    identity/artifact is attested, and `execution-policy` authorizes it. There is no anonymous or
    ambient access — an unadmitted process holds no handle and cannot map the segment, enumerate, or
    inject. Admission is least-privilege (an admitted module still reaches only its declared,
    authorized event kinds) and fail-closed (a refused module is not started and the refusal is
    audited).
18. **Delivery is per-subscription observer routing, not broadcast.** Core routes each event only to
    the modules registered as authorized observers of that event kind (observer pattern); a module
    never receives, and cannot enumerate, traffic for event kinds it did not subscribe to and is not
    authorized for. A subscription is honored only when the descriptor declares it and execution
    policy authorizes it. The single full-stream observer is core's own governance/audit tap, which
    is trust-kernel infrastructure, not a feature module.
19. **Modules are separate programs; isolation is by process or sandbox, not by language.** Core and
    each module are distinct programs that never link; they meet only on the shared-memory bus. A
    trusted first-party module runs as its own process mapped to only its authorized bus queues. An
    untrusted external or user module runs under an enforced sandbox — an OS-sandboxed process
    (seccomp/namespaces/container) or a WebAssembly instance in a host — reachable only through its
    authorized bus queues and unable to read core, `vault`, or another module's memory. The isolation
    mechanism is a deployment choice; the bus contract, admission, and routing are identical across
    them, and separate programs give independent fault domains and scaling. Cross-language in-process
    linking (including cgo and native Go plugins) is not used.

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
- The **event bus** is the core-owned, in-memory **shared-memory** message path over which
  participants exchange typed events. It supports one-way notifications and correlated request/reply,
  resolves a target by capability, authorizes and records the hop, and returns typed
  `capability_absent` when a target is unselected or not `ready`. It is per-service (one in each of
  Runtime and Control Plane); cross-service events travel the existing network transport, not the
  shared-memory bus.
- An **event contract** is the language-neutral schema for the event kinds a module publishes,
  subscribes to, and may request, plus the bus wire format that carries them. It is the only surface
  another participant may invoke, and it replaces the C public header as the enforced dependency edge
  between separate-program participants.
- The **bus host** is the single in-source (C) implementation of the bus — the shared segment,
  admission/handle handshake, observer routing, and the governance/audit tap. It is contained only by
  the two bus-hosting services, the Runtime and the Control Plane; it is not a public, reimplementable
  spec, and there is exactly one of it.
- The **bus client** is the per-language library that lets a module attach the shared-memory bus,
  encode/decode events in the wire format, and publish/subscribe/request. The bus-client spec is
  language-neutral, with reference clients in **C** and **Go** and a cross-language conformance suite,
  which is what makes any language a module host.
- A **hub module** is a module (canonically `memory`) that is an allowed dependency of many other
  modules and itself depends on no feature module — a sink in the dependency graph.
- The **object closure** term extends per axis: the **C object closure** is the selected `.o` set
  for the communication core; the **module closure** is the selected set of module programs and their
  registered event contracts. Omission removes a module from the module closure and the capability
  closure alike.

Runtime-disabled and omitted are not synonyms. A selected module may remain in the build while its
declared runtime lifecycle is disabled; an omitted module must be absent from every build/link/load
and runtime surface named by the suite's absence manifest.

## Language boundary and the inter-module event bus (2026-07-23 amendment)

The suite's original decision placed feature capabilities *inside* core. This amendment narrows core
to exactly what the first message of this work asked for: **the pieces needed to communicate between
the client, the Runtime, and the Control Plane.** That substrate is written in C. Every capability
that does work in response to a message — including `memory` — is a **separate module program** that
communicates over a **core-owned shared-memory event bus**. Because the boundary is a language-neutral
message contract and the participants are separate programs, core and modules never link, cgo is
never required, and a module may be written in any language with a bus client.

### Two independent axes

Selection and language are orthogonal. Each module is classified on both:

- **Selection:** *required* (present in every profile, no enable switch) or *optional* (selectable,
  absent from the module and capability closure when omitted). Unchanged from the approved suite.
- **Runtime/language:** *communication core* (C, in the trusted message path) or *module* (a separate
  program in any conforming language, behind an event contract on the bus). New with this amendment.

"Required" no longer implies "compiled into C core." A required module is always selected and always
registered, but it is a separate program that speaks only over the bus. The eighteen IDs the approved
taxonomy called "required core modules" split across the new axis; the final assignment is delegated
to [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md), which must resolve it
before its round-trip proof is built. This suite records the intended carving:

- **Communication core (C):** `module-runtime` (now also the event bus and capability-state
  authority), `ir`, `translation`, `protocols`, `gateway`, and `config`. These move a typed message
  and nothing else.
- **Required modules (Go reference):** `memory`, `learning`, `routing`, `delegates`, `tools`,
  `workspace`, `git`, `skills`, and `response-composition`. Always selected; separate programs on the
  bus; authored in Go as the first-party reference language, though any conforming language is
  admissible.
- **Trust kernel (pivotal):** `vault`, `execution-policy`, and `audit` gate and record every bus
  event. This suite's recommendation is to keep them in the C communication core, because the bus
  cannot authorize or record an inter-module event without them and the safety boundary must not
  depend on a module the bus is trying to reach. The capability-contract child owns the final
  placement and must state it explicitly; wherever it lands, the safety contracts and their
  reference implementations remain required in every profile.

All eight optional modules are separate programs on the bus, Go by first-party convention.

### The shared-memory event bus

`module-runtime` owns a **shared-memory event bus** per service — lock-free ring buffers in a shared
segment that admitted modules map, giving in-memory speed and zero-copy payloads without a shared
address space, so no cross-language linking or cgo is involved. Every message between participants —
a remote client, the Runtime, the Control Plane, or another module — is a typed IR event on this bus.
A participant publishes a one-way notification or a correlated request and receives the reply; the
bus resolves the target by capability, authorizes the event through the trust kernel, offers it to
the governance/audit tap, and returns the typed result or `capability_absent`. The same IR envelopes,
auth, and capability advertisement drive both the core↔module boundary and the client↔server↔kb
boundary, so a module requesting `memory` and a client invoking the Runtime traverse one construct,
not two. Cross-service events (Runtime↔Control Plane) leave the shared-memory bus and travel the
existing network transport; the shared-memory guarantee is intra-service.

Choosing a bus over direct calls is deliberate: one construct carries every inter-module message, so
it is the single place to **log, govern, and reason about** cross-module behavior (shared invariant
13). It also structurally enforces invariants 1 and 12 — the C core has no build or link edge to any
module; it holds only the descriptor graph and the bus. "Core never depends on an optional module"
becomes unbreakable rather than merely checked: core is a separate program that cannot link a module
at all. An unselected module simply has no registered event contract, and every attempt to reach it
fails closed.

Cross-boundary readiness is answered by the capability advertisement defined in
[`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md): before a
module requests `memory` (or any dependency), it observes that dependency's `ready` state through
the same generation-stamped projection the thin client uses. Discovery, for a module and for a
client, is one mechanism.

**Intra-service versus cross-service.** The shared-memory bus is intra-service: a module reaches
the `memory` (and every other capability) **of its own service** — the Runtime hosts per-user memory,
the Control Plane hosts shared memory. When a module must reach the *other* service's capability (a
Runtime module reading Control-Plane shared memory), the request does not travel the shared-memory
bus; it crosses on the **existing Runtime↔Control network transport**, carrying the *same* typed event
contract, auth, and capability advertisement, and is marked cross-service. That crossing is
explicitly **not** on the shared-memory fast path — it carries network latency and is subject to the
performance budget's separate, looser cross-service class, and cross-service readiness is gated by the
merged advertisement across both services. This proposal fixes only that a cross-service request
reuses the event contract over the network transport rather than inventing a second path; the exact
routing and which capabilities are cross-service-reachable are owned by the product boundary in
[`product-governance-web-and-config.md`](product-governance-web-and-config.md) and the advertisement
child.

### Bus admission and isolation

The bus carries every module's traffic, including the near-universal `memory` path and every
governed action-class event, so **who may map its shared segment is a security boundary in its own
right** — distinct from, and prior to, the per-event authorization above. A process that could map
the segment freely could observe every module's messages or inject its own; the shared segment must
therefore not be mappable by any process (shared invariant 17).

- **Core is the sole admission authority.** As bus owner in each service, core creates the shared
  segment and hands a module its handle and its own queue mappings only on admission. There is no
  side channel to join and no anonymous or ambient mapping; an unadmitted process holds no handle and
  the segment's OS permissions deny it.
- **Every module is identity-attested.** Admission reuses the existing principal and trust machinery
  — the vault principal model, the `cert:CN` / bearer classes the thin-client↔server link uses
  ([`tiered-llm-p8-thinclient-mtls.md`](tiered-llm-p8-thinclient-mtls.md)), and, for externally
  authored modules, `governance`'s artifact signing/trust
  ([`governance-agent-identity-and-artifact-trust.md`](governance-agent-identity-and-artifact-trust.md))
  — rather than inventing a second scheme. A first-party module is trusted at build time; an external
  module's artifact is verified before core starts it.
- **Admission is gated by installation and authorization.** A module participates only when it is
  installed, registered, and authorized by `execution-policy`; a module that is not installed, or
  whose identity or dependencies do not check out, is not started and is granted no handle. This is
  the start-time gate; the per-event contract check (declared, authorized event kinds only) still
  applies afterward, so admission never implies full access.
- **Isolation and least privilege.** An admitted module maps only its own queues, so it sees only the
  event kinds it is authorized to subscribe to — the bus is not a shared channel every module can
  read in full (invariant 18). An untrusted module additionally cannot read core, `vault`, or another
  module's memory: its process/sandbox boundary and its restricted queue mappings expose nothing but
  its authorized bus verbs (invariant 19).
- **Fail-closed and audited.** A refused module is not started and the refusal is recorded through
  the same tap as any other governed event, so an unexpected start attempt is visible, not silent.

This contract sets the admission invariant and its reuse of existing identity machinery; the
mechanics of segment creation, handle granting, and sandbox startup are owned by `module-runtime` and
`module-loader` in their documents.

### Execution model: separate programs, isolation by process or sandbox

Every module is a separate program that meets core only on the shared-memory bus (invariant 19).
Trust decides *how* a module is isolated, not whether it links into core — nothing links into core:

- **Trusted (first-party).** A first-party module runs as its own process, mapped to only its
  authorized bus queues. It is authored in Go by convention but may be any conforming language. It is
  compartmentalized from other modules by the process boundary and observer routing; separate
  processes also give it an independent fault domain and independent scaling.
- **Untrusted (external/user).** An external or user-authored module runs under an enforced sandbox,
  and the sandbox mechanism is a deployment choice over one identical contract: an **OS-sandboxed
  process** (seccomp/namespaces/container) for a native binary in any language, or a **WebAssembly
  instance** in a host for a WASM-targeting module. Either way it reaches nothing but its authorized
  bus queues and cannot read core, `vault`, or another module's memory; the sandbox capability set
  *is* the observer-routed authorized surface, so the runtime enforces isolation rather than trusting
  the module.

Because the participants are separate programs, there is no cross-language in-process linking anywhere
— cgo and native Go plugins are both excluded, the former unnecessary and the latter unshippable
(exact toolchain/dependency lockstep, no universal platform support, no isolation). Splitting a
module out of core costs more than a monolithic in-process call did; the shared-memory bus keeps that
cost small (zero-copy, no syscall on the fast path) and batching/streaming keep the `memory` path
within the performance budget. `module-loader` owns artifact verification, the sandbox host(s), and
lifecycle for the untrusted tier.

### Subscription and routing (observer pattern)

Admission decides *whether* a participant is on the bus; routing decides *what it receives*. These
are separate, and both are least-privilege. The bus is **not** a shared channel every admitted
module reads in full — that would make admission the only wall and let any module observe `memory`
traffic, delegate invocations, or another module's events. Instead the bus follows an **observer
pattern with authorization-scoped routing** (shared invariant 18):

- **Core is the subject and router; modules are observers.** A module registers as an observer of
  the specific event kinds it handles, and receives the replies to its own requests. Core maintains
  the observer set per event kind and delivers each event only to that set. Nothing reaches a module
  it did not subscribe to.
- **Subscription is authorized at subscribe time.** Core honors a subscription only when the
  module's descriptor declares that subscribe edge *and* `execution-policy` authorizes it for the
  module's principal. An undeclared or unauthorized subscription is refused, fail-closed and audited
  — a module cannot opt into arbitrary traffic, and there is no "all events" subscription available
  to any module.
- **Request/reply is point-to-point.** A correlated request is delivered to the one serving module,
  and its reply to the one requester; neither is fanned out to other observers.
- **The reference monitor is the one exception.** Core's governance/audit tap observes the full
  stream because it *is* the trust kernel's observation point, not a feature module. This is how the
  "single loggable, governable" seam (invariant 13) coexists with module compartmentalization: the
  reference monitor sees everything; every principal sees only its authorized slice.

The security payoff is least privilege by construction: a compromised or malicious module can
observe only the event kinds it was already authorized for, and can inject only its declared,
authorized kinds. Widening what a module can see or send is a descriptor-plus-policy change,
reviewable and audited, not an ambient consequence of being on the bus.

### Record, replay, and debugging

Because every inter-module message is one typed event on one construct, the bus is a complete,
ordered record of the system's cross-module behavior — and that record is **replayable**. The bus
supports capturing the event stream (per service, within the performance budget) and re-driving it
against modules. This is a first-class capability, not a side effect (shared invariant 13):

- **Debugging.** One ordered stream shows exactly what every module published, requested, and
  received, with the trust-kernel verdict on each hop. A failure is inspected on the recorded stream
  instead of reconstructed from scattered per-module logs.
- **Reproduction and test.** A recorded stream replays a production incident or seeds a regression
  test without the original environment; module behavior is exercised against real captured events.
- **Forensics.** Governance replays a window of the stream to see the exact sequence that led to a
  verdict (complementing, not replacing, the durable audit chain in
  [`governance-attestable-enforcement.md`](governance-attestable-enforcement.md)).

Determinism has honest limits, because modules are separate programs, not one lockstep runtime. The
suite does **not** promise bit-identical global re-execution of every process. It defines two replay
modes:

- **Observational replay (always faithful).** Re-present the recorded, ordered stream for inspection,
  forensics, and debugging. Nothing is re-executed, so this is exact by construction and is the
  default for governance and triage.
- **Module replay (bounded, divergence-detecting).** Re-drive one module — or a chosen subset —
  against its recorded inbound events and compare its produced outbound events to the recording.
  Replay is deterministic **only to the extent a module is a function of its bus inputs**: any
  clock, randomness, or external I/O a module uses must be sourced from the bus or injected from the
  recording, or the module is not bit-reproducible and the spec marks it so. Divergence between
  replayed and recorded output is **detected and reported**, never silently absorbed.

Capture obeys the same redaction and principal-scoping as the audit tap; a recorded stream is a
governed artifact.

### Performance budget (the speed constraint)

The bus is superior only while its cost stays within acceptable limits, and the hottest path —
module→`memory`, run on every request on both Runtime and Control Plane — is the one that must not
regress (shared invariant 15). A **shared-memory** bus is what makes this viable: lock-free ring
buffers in a shared segment give near-in-process latency and zero-copy payloads with no per-message
syscall on the fast path, so a separate-program `memory` is reached far faster than any socket and
without cgo. This is not free — splitting `memory` into its own program costs more than the former
monolithic in-process call — so a benchmark gate bounds per-event dispatch overhead and batching and
streaming keep the `memory` round trip small; the round-trip proof exercises the `memory` stages
*across* the bus, not as an in-proc shortcut.

The budget is not a slogan; it is a **committed baseline artifact with named metrics and thresholds**,
owned by `module-runtime` and enforced by the benchmark gate. It fixes, at minimum: a ceiling on
per-event bus dispatch overhead (host enqueue → client dequeue, excluding module work); the `memory`
round-trip latency at p50 and p99 measured against a recorded pre-migration baseline; and a maximum
allowed regression factor for that round trip. The concrete numbers live in a committed baseline file
(not in prose), are set from a measured baseline before the `memory` migration slice, and are a
merge gate: the slice may not land while the gate is red. Changing a threshold is a reviewed change to
that file, so "within budget" always names a real, checkable number.

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
3. **The bus gives `memory` a first-class fast-path.** Recall and ingest support batching and
   streaming so the shared-memory crossing does not regress a hot path, per the performance budget
   above.

### Capability publication and dependency-complete installation

Each of the Runtime and the Control Plane owns its own shared-memory event bus, and core is the bus
owner in each (shared invariant 16). A module does not have its capabilities read out of it by a
core poller; it **publishes its capabilities and its invocable surface descriptors to core over the
bus** when it registers, and publishes state transitions as they happen. Core aggregates those
publications into the capability closure and the generation-stamped projection that
[`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md) returns to the
thin client and to other modules. Publication and advertisement are the same bus mechanism observed
from two ends.

Module→core publication is the first edge of a three-edge **registration chain** owned by that
child: module→host service (this bus edge), Runtime→Control Plane, and thin client→Runtime.
Registration always flows upward to the authority and the projection flows back down the same edge;
a registrant contacts only its own authority, so the thin client's whole view is what its Runtime
knows exists and it never addresses a Control Plane. Because the projection carries surface
descriptors and not only capability state, a client needs no release when a module ships.

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

Because the bus contract, the event contract, and capability publication are the *only* integration
surface, a module needs nothing from core but to speak that surface: attach the bus with a bus
client, subscribe to and publish its declared event kinds, and publish its capabilities. This lets
**end users author their own modules in any language** and plug them in without modifying,
recompiling, or relinking core — the language boundary that once meant "C or Go" is gone, because the
boundary is a message contract, not a linkage. A user module is authored in any language for which a
bus client exists (or can be written as a small shim), packaged as an OS-sandboxed process or a WASM
module, and admitted like any other untrusted participant. The optional `module-loader` module
realizes this (artifact verification, sandbox host(s), and lifecycle); this suite records the
property, and `module-loader`'s own document owns the packaging and host mechanics.

The trust boundary does not soften for a user module — the sandbox and the bus boundary are exactly
why this is safe:

- A user module is an **untrusted principal** running in an enforced sandbox (OS-sandboxed process or
  WASM instance). Every event it publishes or requests is authorized by `execution-policy` and
  recorded by `audit` through the same tap as any other event; it maps only its authorized bus queues,
  so it gets no ambient access and can reach another module (for example `memory`) only through that
  module's public event contract, only for event kinds it declared, and only if the dependency is
  installed.
- **Dependency-complete installation** applies unchanged: a user module that depends on `memory`
  installs only when `memory` is present and reaches it solely over the bus.
- Executable-artifact trust — signing and hash-pinning of externally authored modules — is owned by
  optional `governance`
  ([`governance-agent-identity-and-artifact-trust.md`](governance-agent-identity-and-artifact-trust.md));
  a deployment that requires signed modules enforces it there before core starts the module.

Nothing about user-authored modules is a new core capability or a new privilege path; it is the
existing bus boundary, enforced by the sandbox, offered to code from outside the project — in
whatever language its author chose.

### What this changes downstream

- [`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md)
  owns the polyglot build (C core plus separate module programs from one descriptor graph), the event
  contract schema, the single in-source (C) bus host and the bus-client spec with its C and Go
  reference clients and cross-language conformance suite, bus ownership, and dependency enforcement
  re-expressed as authorized event publication/subscription rather than only a C link/symbol graph.
- [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md) owns the final
  core-vs-module carving of the eighteen IDs, the trust-kernel placement, and a round-trip proof whose
  stages flow as bus events across the boundary within the performance budget.
- [`event-bus-governance-and-capture.md`](event-bus-governance-and-capture.md) uses the bus as a
  single, uniform capture and enforcement seam, replacing the seven scattered enforcer sinks that
  the in-flight [`governance-attestable-enforcement.md`](governance-attestable-enforcement.md) routes
  into the chain one by one — consuming that work's ledger and attestation surface without modifying
  it (it is mid-implementation).
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

1. `module-loader`
2. `governance`
3. `workflows`
4. `roundtable`
5. `kb-synthesis`
6. `runtime-web`
7. `control-web`
8. `benchmarks`

`workflows` owns the `triggers`, `cron`, and `event-activation` capabilities; those are not
additional module IDs.

**Rename (2026-07-23 amendment).** The optional module formerly listed as `plugin-loader` is renamed
`module-loader`: under the separate-program model it loads and hosts *modules* (native or sandboxed,
any language), not in-process C plugins, and it supersedes the legacy in-tree plugin loader. The
count stays eight; the canonical inventory transcribes `module-loader`. Because `plugin-loader` was a
proposed ID that never reached a build or runtime, the rename carries no runtime compatibility alias;
`module-loader`'s own document owns any migration of the legacy plugin code.

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
   registration chain and the static thin client: a module registers with its host service over the
   bus, a Runtime registers with its Control Plane, and a thin client registers with its Runtime;
   each edge returns a generation-stamped projection of the capability closure *and the invocable
   surface descriptors* (CLI verb/args/help, MCP tool schema, route, web surface) the registrant may
   see. A registrant contacts only its own authority — the thin client never contacts a Control Plane
   and does not know whether one exists; the Runtime holds the merged closure and evaluates the
   cross-service dependency law once. The client therefore ships no module knowledge and needs no
   release when a module ships. It consumes proposals 2–5's capability-state, config, product, and
   protocol contracts and adds no taxonomy. It was drafted after the 2026-07-20 suite review and
   awaits its own roundtable review; it does not inherit the suite approvals.
9. [`event-bus-governance-and-capture.md`](event-bus-governance-and-capture.md) owns governance and
   audit over the module event bus: the single tap that captures every inter-module event, authorizes
   action-class events, and feeds the durable ledger and attestation bundle. It consumes the in-flight
   [`governance-attestable-enforcement.md`](governance-attestable-enforcement.md) (mid-implementation)
   without modifying it, and lands after both the event bus and that work's A1–A5. Later-drafted
   consuming child; awaits its own review.
10. [`module-loader.md`](module-loader.md) owns the optional `module-loader` module (renamed from
    `plugin-loader`): the module package format, artifact verification, the OS-sandbox and WebAssembly
    host runtimes, and the loaded-module lifecycle for external and user-authored modules. It consumes
    core admission/bus/policy and optional `governance` artifact trust without owning them.
    Later-drafted consuming child; awaits its own review.

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
in broad `src/`, `src/server/`, `src/kb/`, `src/db1/`, `src/modules/db2/c/`, or global-header buckets.

### Tracked delegates header transition

Owner: required-core `delegates`. Completed 2026-07-22. All 19 delegate headers now live under
`src/modules/delegates/include/aimee/delegates/`; all consumers use canonical includes; the flat Make/CMake
include roots are removed; and `src/modules/delegates/module.yaml` declares the complete public-header set.
The header-layout contract, source-ownership mutation tests, and refactor public-header baseline prevent
flat shadows or a second supported public API from returning. The pre-existing roundtable/delegates header
cycle remains a separate dependency-design follow-up rather than being hidden inside this mechanical move.

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

**The 2026-07-23 amendment is not covered by any of the approvals above.** It makes core a C
communication substrate and every module a separate program in any conforming language over a
core-owned shared-memory event bus (bus host vs bus client, admission, observer routing,
record/replay, dependency-complete install, `module-loader` renamed from `plugin-loader`). This
changes shared invariants and the taxonomy's language axis, so it **reopens** the suite index and the
`module-runtime` and `aimee-core-capability-contract` children for fresh technical-writing,
architecture, adversarial, and verification review, and it requires reconciling the sibling children
(`memory-learning-and-inference-boundaries`, `product-governance-web-and-config`,
`feature-liveness-and-background-curator-removal`, `large-refactor-delivery-and-compatibility`), which
carry 2026-07-23 reconciliation notes pending that review. Until that review completes, the suite
holds its prior roundtable-approved state only for the parts the amendment did not touch; the amended
surface is DRAFT. The three new consuming children
(`thin-client-capability-advertisement`, `event-bus-governance-and-capture`, `module-loader`) are
freshly drafted and unreviewed.
