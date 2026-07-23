# Proposal: the registration chain and the static thin client

- **State:** DRAFT — 2026-07-23; awaiting roundtable review. Not part of the 2026-07-20 suite
  roundtable approval; this is a later-drafted consuming child.
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** the registration chain — how a module registers with its host service, how a Runtime
  registers with a Control Plane, and how a thin client registers with a Runtime; the
  generation-stamped capability-and-surface projection each registration edge returns and refreshes;
  and the rule that makes the thin client **static**: a client binary carries no module knowledge and
  requires no release when a module ships.
- **Consumes (does not redefine):** `module-runtime` capability state, closure, and descriptors;
  `config`/[`product-governance-web-and-config.md`](product-governance-web-and-config.md) effective
  catalog and activation filtering; `gateway` admission/sessions/streaming; `protocols` MCP/ACP
  mappings; the thin-client↔Runtime and Runtime↔Control-Plane transports and their principal classes
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
Control Plane, and over time.

Today the client does not merely fail to *learn* the feature set — it **contains** it. `commands[]`
(`src/cmd_table.c:139`) is a compiled table of 84 entries, each binding a CLI verb, its help text,
and its tier to a `cmd_*` function pointer resident in the client binary. `route_capabilities`
(`src/server/server_http.c:653`) and the Control Plane equivalent (`src/kb/http/kb_http.c:173`)
answer with a hardcoded string list independent of what is compiled in, selected, enabled, or ready,
and nothing consumes either. The consequence is the cost this proposal exists to remove: **shipping a
module means shipping a client**. Every release drags the thin client along, and a deployment cannot
upgrade its Runtime or Control Plane and pick up the new behavior without also upgrading every client
attached to it.

The one surface that already works correctly shows the shape of the fix. MCP `tools/list` holds no
local catalog; it forwards `mcp.tools_list` to the Runtime and returns what the Runtime reports
(`src/cli_mcp_serve.c:308`), and `handle_initialize` advertises `listChanged` precisely because the
presented set is not the client's to know (`src/cli_mcp_serve.c:271`). A new server-side tool appears
in a stale client with no client change. This proposal generalizes that property to every surface a
module can offer, and makes it a contract rather than an accident.

This proposal owns the *registration and advertisement surface* only. It does not own the
capability-state model, the descriptor graph, the config catalog, transport authentication, or
governance policy distribution; it composes them.

## Decision

1. **Registration flows upward to the authority; projection flows back down the same edge.** There
   are exactly three registration edges: a module registers with its host service over that service's
   event bus; a Runtime registers with its Control Plane over the network transport; a thin client
   registers with its Runtime over the network transport. Each registrant announces itself to exactly
   one authority, and that authority answers with a generation-stamped projection of the capability
   and surface closure the registrant is authorized to see.
2. **A registrant talks only to its own authority.** The thin client never contacts the Control
   Plane; it does not know whether one exists. Its Runtime is its sole authority and holds the merged
   closure. The Control Plane's contribution reaches the client because the Runtime registered with
   it, not because the client did.
3. **The projection carries the invocable surface, not only capability state.** A capability is
   advertised together with the surface descriptors needed to present and invoke it — CLI verb,
   arguments, help, tier; MCP tool name and input schema; HTTP route; web surface — so a client that
   has never heard of a module can expose it.
4. **The thin client is static.** A client binary ships no module knowledge and dispatches every
   module surface generically from the projection. A client at version *N* correctly serves modules
   shipped in a Runtime or Control Plane at version *N+k* within the compatibility window; only a
   change to the core transport, registration, or handshake contract requires a client release.
5. **A capability the effective set does not offer is never presented**, and invoking it returns a
   typed `capability_absent` — never a partial or silent success.

## The registration chain

```
module ──register──▶ Runtime core ──register──▶ Control Plane core
  ◀──projection───     (server)     ◀──projection──      (kb)
                          ▲   │
                 register │   │ projection
                          │   ▼
                      thin client
                          ▲   │
                 register │   │ projection (re-advertisement)
                          │   ▼
                   consumer (MCP/ACP/CLI/web)
```

Each edge is the same mechanism at a different scale: *the registrant declares what it offers; the
authority admits it, folds it into a closure, and returns the closure the registrant may see.* The
edges differ only in transport and trust.

**Module → host service (intra-service, event bus).** A module publishes its capabilities, its
surface descriptors, and its state transitions to core over the shared-memory event bus when it
registers, exactly as the suite amendment specifies
([`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md),
"Capability publication and dependency-complete installation"). Core aggregates the publications
rather than polling modules. This edge is where the closure originates. Both the Runtime and the
Control Plane own a bus and terminate this edge for their own modules.

**Runtime → Control Plane (inter-service, network).** The Runtime registers with the Control Plane,
declaring its identity, version, and the capability/surface closure its own modules published. The
Control Plane admits it under the existing transport principal class, folds it into its view, and
returns its own generation-stamped projection scoped to that Runtime. The Runtime holds the **merged
closure** — its modules plus the Control Plane's advertised capabilities — evaluated under the suite
dependency law across both services. Cross-service dependency evaluation happens here, at the
Runtime, once, rather than at every attached client.

**Thin client → Runtime (inter-service, network).** The client registers with the Runtime it is
attached to, declaring its identity, its protocol version, and the surface kinds it can render (see
*The static client*). The Runtime returns the merged closure projected and filtered to what that
client's principal, transport class, and surface-kind support permit. The client learns only what its
Runtime knows exists; it never learns whether a given capability originated in the Runtime's own
modules or in a Control Plane, and must not depend on the distinction.

**Client → consumer (re-advertisement).** The client projects the effective set into whatever
consumes it, through that consumer's own protocol: the MCP `initialize` capability object and
`listChanged` notification, the ACP capability handshake (`src/acp_registry/agent.json`), and the
CLI/web surfaces. This is a projection of the received closure, not a locally-authored one.

### Why registration is upward and not the reverse

The registrant is the party whose availability is contingent; the authority is the party that
persists and arbitrates. Pushing downward would require each authority to discover, address, and
maintain liveness for an unbounded, churning set of dependents — a Control Plane tracking every
Runtime, a Runtime tracking every client — and to hold credentials for connecting *to* them. Upward
registration inverts all three: the registrant knows exactly one address, initiates the only
connection, and authenticates itself under the transport class it already holds. This also matches
the transport that exists — the thin client's remote path probes and attaches to its Runtime and has
no Control Plane connection at all (`remote_health_ok`, `src/cli_remote.c:306`) — and it keeps the
Control Plane reachable from a network position clients cannot reach.

## What is registered

A registration declares, and a projection returns, the **capability record**: everything a consumer
needs to decide whether a capability is available and how to invoke it.

**Capability state**: the capability `id`; its `kind` (`required` | `optional`); its typed `state`
(the module-runtime lifecycle value — `absent`, `selected`, `disabled`, `starting`, `ready`,
`degraded`, `unavailable`, `stopping`, `failed`); the declared `depends_on` capability ids that gate
it; and the `generation` at which its state last changed.

**Surface descriptors**: for each surface the capability offers, a typed descriptor sufficient for a
client that has never heard of the module to present and dispatch it. Surface kinds:

- `cli` — verb (and subcommand path), argument and flag schema with types and defaults, one-line
  help, long help, tier (`core` | `advanced` | `admin`), hidden flag, and aliases. This is precisely
  the information `command_t` holds today (`src/cmd_table.c:139`) minus the function pointer, which
  the generic dispatcher replaces.
- `tool` — MCP tool name, description, and JSON input schema, in the shape `tools/list` already
  returns.
- `route` — HTTP method and `/v1` path, for clients that proxy or surface routes.
- `web` — the web surface identifier the product boundary defines.

A surface descriptor is **declarative only**. It names no client-side code, carries no executable
content, no template language, and no code URL; it says what a surface is called, what it takes, and
what it is for. The client's generic dispatcher decides how to render it and where to send the call.
This is the boundary that keeps a static client from becoming a code-delivery channel: registration
transports *descriptions*, never behavior.

**Document envelope**: the service `role` (`runtime` | `control`), `version`, an `epoch` identifying
this process instance, a monotonic `generation` bumped on any change, and the projection
`schema_version`.

Surface declarations live in the module descriptor and are owned by
[`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md) —
whose descriptor contract already declares "routes/commands/protocols". This proposal fixes their
projected wire form and requires the projection be **derived from** those declarations, never authored
separately. The projection introduces no capability, no state, no surface, and no dependency edge that
the descriptor graph and config activation filtering do not already declare.

An `optional` module **omitted** from the build closure is **absent from the projection entirely**
(not listed as `disabled`) — consistent with the suite rule that omission leaves no residue;
`disabled` is reserved for a selected module whose runtime lifecycle is off.

The projection is **authorization-scoped**: it is what *this* principal and transport class may see
and use, not a full inventory. A stronger transport class (`cert:CN`, per
[`tiered-llm-p8-thinclient-mtls.md`](tiered-llm-p8-thinclient-mtls.md)) may be shown capabilities a
bare bearer is not. Scoping reuses the existing gateway identity/capability gate; it defines no second
authorization model. Surface descriptors are scoped with their capability — an unauthorized
capability's surfaces are absent, not merely non-invocable.

## The static client

The client's contribution to the chain is a **client capability declaration**: its protocol version,
its projection `schema_version`, and the surface kinds it can render. The Runtime filters the
projection to that declaration. An old client that does not render `web` surfaces is simply not sent
them; it is not broken by their existence.

**Compatibility rules that make version independence real:**

1. **Unknown fields are ignored, not fatal.** A client encountering a descriptor field it does not
   know renders the capability without it.
2. **Unknown surface kinds are dropped, not fatal.** A capability offering only surface kinds the
   client cannot render is omitted from that client's effective set; a capability offering a mix is
   presented through the kinds it can render.
3. **Unknown state values fail closed.** A lifecycle state the client does not recognize is treated as
   not-`ready`, so a newer state name can never be mistaken for availability.
4. **Additive by default; removal and semantic change are versioned.** Adding a capability, a surface,
   a surface kind, or a descriptor field does not bump `schema_version`. Removing a field, narrowing
   its meaning, or changing a state's semantics does, and the Runtime serves the highest
   `schema_version` at or below what the client declared, or refuses the registration with a typed
   error naming the required client version.
5. **The client ships no module knowledge.** No module name, verb, tool name, route, help string, or
   argument schema is compiled into the client. `commands[]` reduces to the verbs core owns —
   attach/remote, identity and enrollment, health, help, version, and the generic dispatcher itself —
   and everything else arrives by registration. A mechanical check enforces this so the table cannot
   silently regrow.

**What still requires a client release:** a change to the transport, the registration handshake, the
projection envelope, or the generic dispatch contract — that is, a change to *core*, which is exactly
the small stable surface the suite reduces core to. Shipping a module is not on that list.

**Generic dispatch.** For a `cli` surface, the client parses the declared argument schema, validates
locally against it, and forwards the invocation to the Runtime over the existing transport. For a
`tool` surface it presents the declared schema through MCP and forwards on call, as `tools/list`
already does. Failure to satisfy the declared schema is a client-side typed error naming the offending
argument; the client does not guess, coerce, or forward an invocation it cannot validate.

**Local execution is out of scope.** Some surfaces genuinely need the client's own machine — the
filesystem, the TTY, an editor integration. This proposal does **not** define a client-side native
handler mechanism for them; those surfaces remain core-owned client verbs. Introducing module-supplied
local execution would put untrusted module intent on the client host and is a separate proposal with
its own trust analysis. A module descriptor may not declare a surface requiring client-local
execution; the descriptor validator rejects it.

## Delivery: on registration and on change

The Runtime and the Control Plane each serve the projection over their existing `/v1` surface,
mirroring the fail-closed **readiness provider** pattern already in place
(`server_http_set_ready_provider`, `src/server/server_http.c:667`; `/v1/ready`) rather than the static
string list. A registered provider samples the live closure **off** the request path; with no provider
registered the surface fails closed exactly as `/v1/ready` does today, advertising nothing rather than
advertising a stale list.

- **On registration.** The thin client already probes `GET /v1/health` and pins the Runtime cert when
  it attaches (`src/cli_remote.c:304`, `remote_pin_cert`/`remote_set`). Registration extends that
  attach: the client presents its capability declaration and records the returned projection with its
  `epoch`/`generation`. The Runtime registers with the Control Plane the same way at its own startup
  and on reconnect.
- **On change.** A registrant keeps its projection current without polling hot: it revalidates against
  the authority's `generation` (conditional fetch keyed on the generation as an entity tag), and where
  a live stream already exists, a change event carries the new `generation` and the registrant
  refetches. A change to the closure, to any advertised state, or to any surface descriptor bumps
  `generation`. An `epoch` change (process restart) forces a full refetch and drops the cached view.
- **Change propagates down the whole chain.** A module state change bumps its host service's
  `generation`; if that service is a Control Plane, the change bumps the registered Runtime's merged
  `generation`, which bumps what attached clients see, which fires the consumer protocol's
  capability-change signal. One module transition reaches the consumer through the chain without any
  hop polling another.
- **Freshness is bounded and fail-closed.** A projection older than a bounded freshness window, or one
  whose authority is unreachable, is **stale**: capabilities that cannot be confirmed `ready` at the
  current generation are re-advertised as **unavailable**, never served from a stale cache as if live.
  Loss of the Control Plane degrades the Runtime's merged closure, which degrades what clients are
  offered; it does not silently freeze either.

## Merge and re-advertisement

The **Runtime** composes its own modules' closure with the Control Plane's projection into a single
effective set. Merging at the Runtime rather than at each client means the cross-service dependency
law is evaluated once, consistently, by the party that holds both views — and it is what lets the
client remain ignorant of the Control Plane entirely.

- **Dependency law across services.** A capability is offered only when it and every `depends_on`
  capability it declares are `ready` — including dependencies satisfied on the *other* service. A
  Runtime capability depending on a Control-Plane capability is offered only if the Control Plane
  advertises that dependency `ready`; a dependency in any non-`ready` state suppresses its dependent.
  This is the suite dependency law
  (`core-substrate-and-source-module-boundaries.md` invariants 1–4) evaluated at the Runtime seam.
- **Identity collision is resolved at the merge, deterministically.** Capability ids are global under
  the suite taxonomy, so the same id advertised by both services is the same capability; its merged
  state is the **weaker** of the two (any non-`ready` contribution yields non-`ready`), and its surface
  descriptors must be identical. A conflicting descriptor for a shared id is a registration error the
  Runtime reports and fails closed on — it does not pick a winner.
- **Re-advertisement maps onto the consumer's protocol.** The effective set is projected into the
  handshake the client already speaks: the MCP `initialize` capability object (`handle_initialize`,
  `src/cli_mcp_serve.c:271`) and its change notification, the ACP capability handshake, and the
  CLI/web surfaces. When the effective set changes, the client emits the consumer protocol's
  capability-change signal; consumers without one observe the change on their next handshake.
- **Absent means absent.** A capability outside the effective set is not listed, and a request against
  it returns a typed `capability_absent` with the same external status, body shape, and timing as an
  unknown capability, so the wire cannot distinguish *disabled* from *never-existed* (mirroring the
  web-route rule in `product-governance-web-and-config.md`).

## One discovery mechanism

The same generation-stamped projection is what a **module** consults to learn whether `memory` (or any
dependency) is `ready` before it publishes a request on the bus. Discovery is one mechanism for a
client and for a module: publication (registrant→authority) and projection (authority→registrant) are
the same capability data observed from the two ends of one edge. A module never reaches an unready
dependency, and a client never advertises one, for the same reason and through the same records.

## Truthfulness invariants

1. The projection equals the module-runtime capability-and-surface closure for the registrant's scope;
   it never lists a capability the closure does not contain, a state the runtime does not hold, or a
   surface a descriptor does not declare.
2. A capability advertised `ready` is invocable for that principal at that generation through every
   surface it advertises; readiness and invocability cannot contradict, the same way `/v1/ready`
   forbids a 200 body that says `ready:false`.
3. Omitted optional modules are absent from the projection; only selected-but-off modules are
   `disabled`.
4. No registrant is offered a capability whose dependencies are not `ready` across the merged services.
5. Every projection is generation-stamped; a registrant that cannot confirm the current generation
   treats affected capabilities as unavailable.
6. A registrant contacts only its own authority. The thin client issues no Control Plane request and
   holds no Control Plane address or credential; its view is exactly what its Runtime knows exists.
7. Surface descriptors are declarative; a projection carries no executable content, template language,
   or code reference, and no client-side handler binding.
8. The client binary contains no module-specific verb, tool name, route, help text, or argument
   schema; adding a module changes no client source.

## Non-goals

- Defining or changing the capability-state lifecycle, the descriptor graph, or the module taxonomy
  (owned by `module-runtime`/core-capability-contract). This proposal fixes only the projected wire
  form of surface declarations the descriptor contract already owns.
- Defining the rendered effective **config** catalog, activation filtering, or user-visible config
  surfaces (owned by `product-governance-web-and-config.md`).
- Defining transport authentication or the principal/cert classes (owned by the mTLS/p8 work); this
  proposal only *scopes the projection by* the class the transport already established.
- Delivering module code, handlers, templates, or any executable content to a client; and defining
  client-local module execution, which is explicitly deferred.
- Distributing governance policy, roles, or posture; those are optional `governance` surfaces and are
  advertised, if selected, like any other optional capability.
- Introducing a generic service-locator or a second capability registry; the projection is a read-only
  view of the one module-runtime closure.

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_capability_advertisement.sh --projection-only --equals-module-runtime-closure --forbid-static-capability-list src/server/server_http.c,src/kb/http/kb_http.c --require-generation-epoch-schema-version --require-typed-state --omitted-optional-absent --disabled-only-for-selected"}
- {id: 2, tier: mechanical, check: "scripts/check_capability_advertisement.sh --authorization-scoped --require-transport-class-scoping bearer,cert-cn --scope-surfaces-with-capability --forbid-unauthorized-capability-leak --no-second-authorization-model"}
- {id: 3, tier: mechanical, check: "scripts/check_surface_descriptors.sh --derived-from-descriptors-only --kinds cli,tool,route,web --declarative-only --forbid-executable-content --forbid-template-language --forbid-client-handler-binding --reject-client-local-execution-surface"}
- {id: 4, tier: mechanical, check: "scripts/check_static_thin_client.sh --command-table src/cmd_table.c --allow-core-verbs-only --forbid-module-verb --forbid-module-tool-name --forbid-module-route --forbid-module-help-text --forbid-module-arg-schema --client-source-unchanged-when-module-added"}
- {id: 5, tier: integration, check: "scripts/test_registration_chain.sh --module-to-host --runtime-to-control --client-to-runtime --registrant-contacts-only-its-authority --assert-no-client-to-control-request --assert-client-holds-no-control-address-or-credential --module-change-propagates-to-consumer-without-polling"}
- {id: 6, tier: integration, check: "scripts/test_capability_advertisement.sh --on-registration-snapshot --generation-bumps-on-closure-state-and-surface-change --epoch-change-forces-refetch --stale-window-fails-closed --unreachable-control-degrades-runtime-merge-not-freezes"}
- {id: 7, tier: integration, check: "scripts/test_capability_advertisement.sh --merge-at-runtime --cross-service-dependency-gating --dependent-suppressed-when-dependency-not-ready --shared-id-takes-weaker-state --conflicting-shared-descriptor-fails-closed --require-effective-set-equals-ready-closure-under-deps"}
- {id: 8, tier: integration, check: "scripts/test_static_thin_client.sh --client-version N --service-version N+1 --new-module-usable-without-client-release --unknown-field-ignored --unknown-surface-kind-dropped --unknown-state-fails-closed --removed-field-bumps-schema-version --incompatible-schema-refused-with-typed-error"}
- {id: 9, tier: integration, check: "scripts/test_capability_advertisement.sh --consumer mcp --consumer acp --consumer cli --re-advertise-effective-set --generic-dispatch-from-descriptor --invalid-args-typed-error-not-forwarded --emit-change-on-effective-set-change --absent-capability-returns-typed-capability-absent --disabled-and-unknown-externally-identical"}
```

## Review status

Freshly drafted 2026-07-23. This proposal has **not** been through the suite roundtable and does not
inherit the 2026-07-20 approvals; it must complete its own technical-writing, architecture,
adversarial, and verification review before acceptance. It adds a consuming runtime surface and does
not modify the suite taxonomy, shared invariants, or any parent contract; if review finds that its
surface-descriptor requirements amend the `module-runtime` descriptor contract rather than consume it,
that content must move to `module-runtime` rather than be amended here.
