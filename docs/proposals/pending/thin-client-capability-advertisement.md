# Proposal: advertise the effective capability set to the thin client and its consumer

- **State:** DRAFT — 2026-07-23; awaiting roundtable review. Not part of the 2026-07-20 suite
  roundtable approval; this is a later-drafted consuming child.
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** the runtime capability-advertisement surface — how a Runtime and a Control Plane
  project their live capability closure and state to a connecting thin client, how that projection
  is refreshed when the closure or state changes, and how the thin client merges the two
  projections and re-advertises the effective set to its own downstream consumer.
- **Consumes (does not redefine):** `module-runtime` capability state and closure;
  `config`/[`product-governance-web-and-config.md`](product-governance-web-and-config.md) effective
  catalog and activation filtering; `gateway` admission/sessions/streaming; `protocols` MCP/ACP
  mappings; the thin-client↔server transport and its principal classes
  ([`tiered-llm-p8-thinclient-mtls.md`](tiered-llm-p8-thinclient-mtls.md)).
- **Implementation dependencies:** module descriptors and capability-state lifecycle
  ([`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md),
  [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md)); the effective config
  catalog and product boundary ([`product-governance-web-and-config.md`](product-governance-web-and-config.md)).
- **Date:** 2026-07-23

## Thesis

Under the suite, core becomes only the contracts needed to communicate between the client, the
Runtime (server), and the Control Plane (kb); every other behavior is an optional module with
declared dependencies and a typed capability state. Once that is true, a thin client can no longer
assume a fixed feature set: which modules are selected, enabled, and ready differs per Runtime, per
Control Plane, and over time. The client — and whatever consumes the client — must be *told* what is
actually available, kept current when it changes, and told truthfully enough to fail closed.

Today that contract is a lie by omission. `route_capabilities` (`src/server/server_http.c:642`) and
the kb equivalent (`src/kb/http/kb_http.c:173`) each return a **hardcoded string list** that is
independent of what is compiled in, selected, enabled, or ready. Nothing on the client consumes it,
and nothing pushes an update when the server's or Control Plane's capability set changes. This
proposal replaces that static list with a **truthful projection of the module-runtime capability
closure and state**, delivers it to the thin client on connect and on change, and defines how the
thin client re-advertises the merged effective set to its consumer.

This proposal owns the *advertisement surface* only. It does not own the capability-state model, the
config catalog, transport authentication, or governance policy distribution; it composes them.

**One discovery mechanism (2026-07-23).** The suite amendment
([`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md))
makes every module a Go participant on a core-owned in-memory event bus, where nearly every module
depends on `memory`. The same generation-stamped capability projection defined here is what a module
consults to learn whether `memory` (or any dependency) is `ready` before it publishes a request on
the bus. Discovery is one mechanism for a client and for a module; this proposal's projection serves
both, so a module never reaches an unready dependency and a client never advertises one.

## Decision

1. Each Runtime and each Control Plane exposes a **capability advertisement**: a typed, versioned
   projection of its selected capability closure and current capability state, scoped to the
   authenticated principal and transport class of the caller.
2. The advertisement is delivered to a connecting thin client **on connect** and **refreshed on
   every change** to the closure or to any advertised capability's state, identified by a monotonic
   generation so the client can detect staleness and fail closed.
3. The thin client **merges** the Runtime and Control Plane advertisements into one effective
   capability set by applying the suite dependency law across both services, and **re-advertises**
   that effective set to its downstream consumer through the consumer's own protocol
   (MCP/ACP capability negotiation, and the CLI/web surfaces).
4. A capability the effective set does not offer is never presented to the consumer, and invoking
   it returns a typed `capability_absent`, never a partial or silent success.

## Advertisement content

The advertisement is a projection, not a new source of truth. For each capability in the caller's
authorized view it carries: the capability `id`; its `kind` (`required` | `optional`); its typed
`state` (the module-runtime lifecycle value — `absent`, `selected`, `disabled`, `starting`,
`ready`, `degraded`, `unavailable`, `stopping`, `failed`); the declared `depends_on` capability ids
that gate it; and the `generation` at which its state last changed. The document carries a
service-level `epoch` (identifying this process instance) and a monotonic `generation` (bumped on
any change), plus the `service` role (`runtime` | `control`) and `version`.

The projection is derived only from module-runtime's capability closure and state and config's
activation filtering. It introduces no capability, no state, and no dependency edge that those
contracts do not already declare. Under the suite amendment
([`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)),
that closure is assembled by **modules publishing their capabilities and state transitions to core
over the event bus** — core aggregates those publications rather than polling modules — and this
proposal projects the aggregate. Publication (module→core) and advertisement (core→client/module)
are the same capability data observed from the two ends of the bus. An `optional` module that is **omitted** from the build closure is
**absent from the advertisement entirely** (not listed as `disabled`) — consistent with the suite
rule that omission leaves no residue; `disabled` is reserved for a selected module whose runtime
lifecycle is off. `absent` never appears for an id the caller could otherwise enumerate; unknown ids
are simply not present.

The advertisement is **authorization-scoped**: it is a projection of what *this* principal and
transport class may see and use, not a full server inventory. A stronger transport class
(`cert:CN`, per [`tiered-llm-p8-thinclient-mtls.md`](tiered-llm-p8-thinclient-mtls.md)) may be shown
capabilities a bare bearer is not. The scoping reuses the existing gateway identity/capability gate;
it does not define a second authorization model.

## Delivery: on connect and on change

The Runtime and Control Plane each serve the advertisement over their existing `/v1` surface,
mirroring the fail-closed **readiness provider** pattern already in place
(`server_http_set_ready_provider`, `src/server/server_http.c:667`; `/v1/ready`) rather than the
static string list. A registered provider samples the live capability state **off** the request
path; with no provider registered every capability reads `unknown` and the surface fails closed,
exactly as `/v1/ready` does today.

- **On connect.** The thin client already probes `GET /v1/health` and pins the server cert when it
  attaches (`src/cli_remote.c:304`, `remote_pin_cert`/`remote_set`). It additionally fetches the
  advertisement for each attached service and records its `epoch`/`generation`.
- **On change.** The client keeps the advertisement current without polling hot: it revalidates
  against the service `generation` (conditional fetch keyed on the generation as an entity tag), and
  where a live stream to the client already exists, a capability-change event carries the new
  `generation` and the client refetches. A change to the closure or to any advertised state bumps
  `generation`; an `epoch` change (process restart) forces a full refetch and drops any cached view.
- **Freshness is bounded and fail-closed.** A cached advertisement older than a bounded freshness
  window, or one whose service is unreachable, is treated as **stale**: capabilities that cannot be
  confirmed `ready` at the current generation are re-advertised to the consumer as **unavailable**,
  never served from a stale cache as if live. Loss of the Control Plane degrades the merged set; it
  does not silently freeze it.

## Merge and re-advertisement to the consumer

The thin client composes the Runtime and Control Plane advertisements into a single **effective
set** and re-advertises it to whatever consumes the client.

- **Dependency law across services.** A capability is offered to the consumer only when it and every
  `depends_on` capability it declares are `ready` — including dependencies satisfied on the *other*
  service (a Runtime capability that depends on a Control-Plane capability is offered only if the
  Control Plane advertises that dependency `ready`). A dependency in any non-`ready` state
  suppresses the dependent from the effective set. This is the suite dependency law
  (`core-substrate-and-source-module-boundaries.md` invariants 1–4) evaluated at the client seam.
- **Re-advertisement maps onto the consumer's protocol.** The effective set is projected into the
  consumer handshake the client already speaks: the MCP `initialize` capability object
  (`handle_initialize`, `src/cli_mcp_serve.c:271`) and its change notification, the ACP capability
  handshake (`src/acp_registry/agent.json`), and the CLI/web capability surfaces. When the effective
  set changes, the client emits the consumer protocol's capability-change signal; consumers without
  one observe the change on their next handshake.
- **Absent means absent.** A capability outside the effective set is not listed to the consumer, and
  a consumer request against it returns a typed `capability_absent`, with the same external status,
  body shape, and timing as an unknown capability so the wire cannot distinguish *disabled* from
  *never-existed* (mirroring the web-route rule in `product-governance-web-and-config.md`).

## Truthfulness invariants

1. The advertisement equals the module-runtime capability projection for the caller's scope; it
   never lists a capability the closure does not contain, nor a state the runtime does not hold.
2. A capability advertised `ready` is invocable for that principal at that generation; readiness and
   invocability cannot contradict, the same way `/v1/ready` forbids a 200 body that says
   `ready:false`.
3. Omitted optional modules are absent from the advertisement; only selected-but-off modules are
   `disabled`.
4. The consumer is never offered a capability whose dependencies are not `ready` across the merged
   services.
5. Every advertisement is generation-stamped; a consumer or client that cannot confirm the current
   generation treats affected capabilities as unavailable.

## Non-goals

- Defining or changing the capability-state lifecycle, the descriptor graph, or the module taxonomy
  (owned by `module-runtime`/core-capability-contract).
- Defining the rendered effective **config** catalog, activation filtering, or user-visible config
  surfaces (owned by `product-governance-web-and-config.md`).
- Defining transport authentication or the principal/cert classes (owned by the mTLS/p8 work);
  this proposal only *scopes the projection by* the class the transport already established.
- Distributing governance policy, roles, or posture; those are optional `governance` surfaces and
  are advertised, if selected, like any other optional capability.
- Introducing a generic service-locator or a second capability registry; the advertisement is a
  read-only projection of the one module-runtime closure.

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_capability_advertisement.sh --projection-only --equals-module-runtime-closure --forbid-static-capability-list src/server/server_http.c,src/kb/http/kb_http.c --require-generation-epoch --require-typed-state --omitted-optional-absent --disabled-only-for-selected"}
- {id: 2, tier: mechanical, check: "scripts/check_capability_advertisement.sh --authorization-scoped --require-transport-class-scoping bearer,cert-cn --forbid-unauthorized-capability-leak --no-second-authorization-model"}
- {id: 3, tier: integration, check: "scripts/test_capability_advertisement.sh --service runtime --service control --on-connect-snapshot --generation-bumps-on-closure-and-state-change --epoch-change-forces-refetch --stale-window-fails-closed --unreachable-service-degrades-not-freezes"}
- {id: 4, tier: integration, check: "scripts/test_capability_advertisement.sh --merge-two-services --cross-service-dependency-gating --dependent-suppressed-when-dependency-not-ready --require-effective-set-equals-ready-closure-under-deps"}
- {id: 5, tier: integration, check: "scripts/test_capability_advertisement.sh --consumer mcp --consumer acp --re-advertise-effective-set --emit-change-on-effective-set-change --absent-capability-returns-typed-capability-absent --disabled-and-unknown-externally-identical"}
```

## Review status

Freshly drafted 2026-07-23. This proposal has **not** been through the suite roundtable and does not
inherit the 2026-07-20 approvals; it must complete its own technical-writing, architecture,
adversarial, and verification review before acceptance. It adds a consuming runtime surface and does
not modify the suite taxonomy, shared invariants, or any parent contract; if review finds it does,
it must be re-scoped rather than amending a parent here.
