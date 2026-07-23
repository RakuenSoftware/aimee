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
`degraded`, `unavailable`, `stopping`, `failed`); its dependencies, carried as two **separate** lists
per suite invariant 16 — `hard_depends_on` and `soft_depends_on` — and the `generation` at which its
state last changed. A soft dependency additionally carries the `fallback` identifier its module
declared, so a consumer can be told which reduced behavior is in force rather than inferring it.
The two lists are never merged into one `depends_on`: they gate differently (see *Dependency law*),
and collapsing them would suppress a capability the parent contract requires to keep working.

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

### Each kind's permitted behavior is closed, not open-ended

"Declarative only" is not self-enforcing: a surface can require client-local behavior implicitly, by
naming something the client must resolve, render, or shape locally. So each kind's descriptor schema
is **closed** — a fixed field set, each field with a fixed type and a fixed meaning — and the
permitted client behavior for that kind is fixed with it:

| kind | the client is permitted to | and nothing else |
|---|---|---|
| `cli` | parse args against the declared schema, validate, forward the invocation to its authority | no local file/TTY/editor/process access, no local resolution of an argument's meaning |
| `tool` | present name/description/schema over MCP, forward the call on invoke | no local execution, no client-side schema rewriting |
| `route` | display or proxy the declared method+path to its authority | no client-authored request shaping, no alternate host |
| `web` | render the surface identifier from a **closed enumeration** the product boundary owns | no free-form identifier, no URL, no markup |

A descriptor field whose value the client would have to *interpret* to act — a path, a URL, a
hostname, a command line, a MIME type, a renderer name, a free-form `web` identifier — is not in any
kind's schema, and the descriptor validator rejects a descriptor carrying one. This is what makes
"the validator rejects a surface requiring client-local execution" a decidable property rather than
an aspiration: the check is schema conformance against a closed field set, not intent detection.

### Surface keys are globally unique and core-reserved names are refused

Capability ids are unique, but two *different* capabilities can still claim the same invocable name —
the same CLI verb or alias, the same MCP tool name, the same method+path. That is ambiguous dispatch
at best and surface impersonation at worst: a module claiming `remote` or `login` would shadow a
core verb the user trusts. So each kind has a **canonical key** that is unique across the entire
merged closure, not merely within a capability:

- `cli` — the fully-qualified verb path, and independently every alias, in one flat namespace
- `tool` — the tool name
- `route` — the (method, normalized path) pair
- `web` — the enumerated surface identifier

A canonical key claimed by two capabilities is a **registration error**: the authority refuses the
later registration, reports the conflict naming both claimants, and advertises neither surface. It
does not pick a winner, order-resolve, or silently rename. Separately, core **reserves** the names of
the verbs it owns (the attach/remote, identity/enrollment, health, help, and version verbs) plus a
reserved prefix; a module descriptor claiming a reserved name is rejected at validation, before it
can ever be registered. Aliases are checked identically to verbs — an alias is a claim on the
namespace, and alias-shadowing is the cheapest impersonation path.

### Descriptor content is untrusted input

Forbidding executable content does not make module-authored *text* safe: this content is rendered
into a terminal and parsed by a client. Every string field is normalized and bounded — a required
Unicode normalization form, no C0/C1 control characters or ANSI escape sequences, no bidirectional
or zero-width overrides, no confusable-script mixing within a canonical key, and a declared maximum
length. Every schema field is bounded in size, nesting depth, and total node count, and a schema
whose validation cost is not linear in input size is rejected. The client renders help and
descriptions control-safe regardless, on the assumption that an authority may itself be compromised.
Limits are enforced at the descriptor validator *and* re-enforced at the client, because the two
trust different parties.

**Document envelope**: the service `role` (`runtime` | `control`), `version`, an `epoch` identifying
this process instance, a monotonic **per-scope** `generation`, and the projection `schema_version`.

The `generation` is **not** authority-wide. An authority-wide counter bumped on *any* change would
leak the existence and activity rate of capabilities a caller is not authorized to see: watching the
number advance without any visible change tells the caller that something it cannot see just moved,
which is exactly the disclosure the authorization scoping exists to prevent. Instead each authorized
projection carries a generation derived from **that projection's own content**, advancing only when
the bytes that scope would observe change. Two scopes therefore have unrelated generation sequences,
and neither can infer the other's activity from its own. Change notifications are likewise emitted
per scope and only for scopes whose projection actually changed; a caller receives no event, and no
observable timing difference, for a change confined to capabilities outside its projection. This
noninterference property is stated as invariant 9 and tested, not assumed.

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

1. **Only advisory fields may be ignored; every other unknown fails closed.** Ignoring an unknown
   field is safe exactly when the field cannot change what the client *does* — it is presentation
   metadata. It is unsafe when the field constrains validation, routing, authorization, or
   invocation, because ignoring it means acting without a constraint the author required. So the
   envelope marks each descriptor field `advisory` or `critical`, and the rule splits: an unknown
   **advisory** field is ignored and the capability is rendered without it; an unknown **critical**
   field makes that capability **unavailable** to that client, with a typed reason naming the field.
   A client is never asked to guess which kind it met. `critical` is the default for a field whose
   marking is itself missing, so an authority cannot downgrade a constraint by omission.
2. **Unknown surface kinds are dropped, not fatal.** A capability offering only surface kinds the
   client cannot render is omitted from that client's effective set; a capability offering a mix is
   presented through the kinds it can render.
3. **Unknown state values fail closed.** A lifecycle state the client does not recognize is treated as
   not-`ready`, so a newer state name can never be mistaken for availability.
4. **Additive-advisory is free; anything behavioral is negotiated.** Adding a capability, a surface,
   or an **advisory** field does not bump `schema_version`. Adding a **critical** field, removing any
   field, narrowing a meaning, or changing a state's semantics **does** — a behavior- or
   security-affecting addition is a version change even though it is additive, because an older
   client cannot honor it. The Runtime serves the highest `schema_version` at or below what the client
   declared, or refuses the registration with a typed error naming the required client version. The
   N→N+k guarantee this proposal claims is therefore bounded precisely: an N client keeps working
   across any number of *advisory* additions and new modules, and is told — never silently degraded —
   when a capability needs a newer client.
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
- **On change — every network edge has a defined mechanism, not an optional one.** Each network
  registration edge (client→Runtime, Runtime→Control Plane) operates in exactly one of two modes,
  chosen at registration and reported in the projection so both ends know which is in force:
  - **Notified.** The authority holds an open change stream to the registrant and pushes the new
    per-scope `generation` on change; the registrant then refetches conditionally. This is the
    default whenever the transport supports it.
  - **Bounded revalidation.** Where no stream can be held, the registrant revalidates conditionally
    on a **bounded interval** the authority states in the projection (a cheap generation-only
    request that returns not-modified in the common case).

  A registration that can establish neither mode is refused with a typed error rather than admitted
  in a mode where change would never arrive. A change to the closure, to any advertised state, or to
  any surface descriptor advances the affected scopes' `generation`. An `epoch` change (process
  restart) forces a full refetch and drops the cached view.
- **Additions propagate, not only withdrawals.** Freshness expiry alone can only *withdraw* a
  capability the client already knows about; it can never reveal a capability that appeared after the
  last fetch. That is why revalidation is mandatory rather than a fallback: without it a client would
  keep serving a correct-but-shrinking view forever and never pick up a newly installed module —
  which is precisely the upgrade case this proposal exists to serve. Discovery of additions is
  therefore bounded by the same interval as detection of removals.
- **Propagation latency is bounded end-to-end and stated.** A module state change advances its host
  service's affected scopes; if that service is a Control Plane, it advances the registered Runtime's
  merged projection, which advances what attached clients see, which fires the consumer protocol's
  capability-change signal. Each hop is either notified or bounded, so the worst-case
  module→consumer latency is the **sum of the hops' bounds** — a stated, testable number, not an
  unquantified "without polling" claim. No hop polls another hot; a hop in bounded mode issues one
  conditional generation check per interval.
- **Freshness is bounded and fail-closed.** A projection older than its freshness window, or one
  whose authority is unreachable, is **stale**: capabilities that cannot be confirmed `ready` at the
  current generation are re-advertised as **unavailable**, never served from a stale cache as if live.
  The freshness window is required to exceed the revalidation interval, so ordinary revalidation
  keeps a healthy client live and only genuine loss of contact degrades it. Loss of the Control Plane
  degrades the Runtime's merged closure, which degrades what clients are offered; it does not
  silently freeze either.

## Merge and re-advertisement

The **Runtime** composes its own modules' closure with the Control Plane's projection into a single
effective set. Merging at the Runtime rather than at each client means the cross-service dependency
law is evaluated once, consistently, by the party that holds both views — and it is what lets the
client remain ignorant of the Control Plane entirely.

- **Dependency law across services — hard suppresses, soft degrades.** The parent suite's invariant
  16 governs dependencies, and it draws a distinction this seam must preserve: a **hard** dependency
  is required, while a **soft** dependency is used-if-present and its dependent "must function
  without it via a declared fallback". Readiness gating therefore splits:
  - a capability is offered only when it and every capability in its `hard_depends_on` list are
    `ready` — including dependencies satisfied on the *other* service; a hard dependency in any
    non-`ready` state suppresses its dependent;
  - a `soft_depends_on` entry that is not `ready` **never suppresses** its dependent. The capability
    is offered in `degraded` state, carrying the `fallback` its module declared, so the consumer is
    told which reduced behavior is in force. Suppressing it here would break the parent's guarantee
    that a soft dependency never blocks its dependent.

  **Scope note.** Invariant 16 is an *install-time* rule (what may be installed and removed). The
  parent suite states no *runtime readiness closure* over that graph. This proposal therefore
  **defines** the runtime gating above as a consuming refinement of invariant 16 — it does not merely
  restate a parent rule, and it must be reviewed as new normative content. It introduces no new
  dependency edge: it evaluates readiness over exactly the hard/soft edges the descriptor graph
  already declares. (An earlier revision cited "invariants 1–4" as the dependency law; those
  invariants govern the core/optional taxonomy and say nothing about readiness. The citation was
  wrong and is corrected here.)
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
4. No registrant is offered a capability whose **hard** dependencies are not `ready` across the merged
   services; a capability whose **soft** dependency is not `ready` is offered `degraded` with its
   declared fallback, never suppressed.
5. Every projection is generation-stamped; a registrant that cannot confirm the current generation
   treats affected capabilities as unavailable.
6. A registrant contacts only its own authority. The thin client issues no Control Plane request and
   holds no Control Plane address or credential; its view is exactly what its Runtime knows exists.
7. Surface descriptors are declarative; a projection carries no executable content, template language,
   or code reference, and no client-side handler binding.
8. The client binary contains no module-specific verb, tool name, route, help text, or argument
   schema; adding a module changes no client source.
9. **Noninterference across scopes.** A caller's `generation`, the change events it receives, and the
   observable timing of its requests are functions of its own authorized projection alone. A change
   confined to capabilities outside a caller's scope produces no advance, no event, and no timing
   signal for that caller.
10. **Canonical-key uniqueness.** No two capabilities in the merged closure claim the same canonical
    key for a kind, and no module claims a core-reserved name or prefix; a violation refuses the
    registration and advertises neither claimant rather than resolving to one.
11. **No silent behavioral downgrade.** A capability carrying a `critical` field the client does not
    understand is unavailable with a typed reason, never rendered as if the field were absent.

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

The scripts named below are **implementation deliverables of this proposal**, not claims that those
files exist today — the same convention every sibling in this suite follows. What a proposal owes at
review time is not the script but the **observable pass condition** each check asserts, so the check
cannot later be satisfied by a script that merely exits zero. Each check below therefore states its
fixture and its decision procedure; a flag name alone is not an acceptance criterion.

The non-obvious ones, made concrete:

- **Closure equality** (check 1) compares the served projection against the closure the descriptor
  graph and activation filtering produce for the same profile, over a fixture set of profiles
  including `core`, `runtime`, `control`, `full`, and full-minus-one. Pass is set equality of
  (id, kind, state, hard deps, soft deps, surfaces) — not a subset, and not a spot check.
- **"No second authorization model"** (check 2) is decided structurally, not by intent: the
  projection filter must reach its allow/deny decision solely through the existing gateway
  identity/capability gate, proven by a call-graph assertion that the projection path contains no
  authorization decision site not reachable from that gate, plus a differential fixture where a
  capability's visibility changes if and only if the gateway gate's answer changes.
- **"Client source unchanged when a module is added"** (check 4) builds the client from an unmodified
  source tree, adds a fixture module to the service, and asserts the module's `cli`/`tool` surfaces
  are usable through that same client binary — the artifact is bit-identical, so "unchanged" is a
  hash comparison, not a review judgement.
- **Propagation latency** (check 5) is measured, not asserted: a fixture module transitions state,
  and the elapsed time until the consumer observes the change must be at or below the sum of the
  configured per-hop bounds, in both notified and bounded-revalidation modes. The test also asserts
  the *addition* case — a module installed after the client attached becomes visible within that same
  bound — since expiry alone cannot discover additions.
- **No-client-to-Control-Plane** (check 5) is decided by network observation: the client is run with
  the Control Plane reachable only through an observed path, and the test fails if any packet from
  the client reaches it, and separately asserts no Control Plane address or credential exists in the
  client's configuration or memory.
- **Noninterference** (check 10) is a differential test: two principals with different authorized
  scopes attach; a change confined to capabilities outside principal B's scope must produce no
  generation advance, no event, and no response-timing difference for B beyond a stated tolerance.
- **N→N+k compatibility** (check 8) uses two real builds, not a mocked version string: client at N,
  service at N+1, with the N+1 service adding an advisory field, a critical field, a new surface
  kind, and a new module. Pass requires the advisory addition invisible, the critical addition
  producing typed unavailability, the new kind dropped, and the new module usable.

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_capability_advertisement.sh --projection-only --equals-module-runtime-closure --forbid-static-capability-list src/server/server_http.c,src/kb/http/kb_http.c --require-generation-epoch-schema-version --require-typed-state --omitted-optional-absent --disabled-only-for-selected"}
- {id: 2, tier: mechanical, check: "scripts/check_capability_advertisement.sh --authorization-scoped --require-transport-class-scoping bearer,cert-cn --scope-surfaces-with-capability --forbid-unauthorized-capability-leak --no-second-authorization-model"}
- {id: 3, tier: mechanical, check: "scripts/check_surface_descriptors.sh --derived-from-descriptors-only --kinds cli,tool,route,web --closed-schema-per-kind --reject-unknown-field --reject-interpretable-value path,url,host,commandline,mimetype,renderer --web-identifier-from-closed-enum --declarative-only --forbid-executable-content --forbid-template-language --forbid-client-handler-binding"}
- {id: 4, tier: mechanical, check: "scripts/check_static_thin_client.sh --command-table src/cmd_table.c --allow-core-verbs-only --forbid-module-verb --forbid-module-tool-name --forbid-module-route --forbid-module-help-text --forbid-module-arg-schema --client-binary-hash-identical-when-module-added"}
- {id: 5, tier: integration, check: "scripts/test_registration_chain.sh --module-to-host --runtime-to-control --client-to-runtime --registrant-contacts-only-its-authority --assert-no-client-to-control-packet-observed --assert-no-control-address-or-credential-in-client --measure-propagation-latency-vs-sum-of-hop-bounds --assert-addition-discovered-within-bound --both-modes notified,bounded-revalidation"}
- {id: 6, tier: integration, check: "scripts/test_capability_advertisement.sh --on-registration-snapshot --generation-advances-on-closure-state-and-surface-change --epoch-change-forces-refetch --stale-window-exceeds-revalidation-interval --stale-window-fails-closed --unreachable-control-degrades-runtime-merge-not-freezes --registration-refused-when-neither-mode-available"}
- {id: 7, tier: integration, check: "scripts/test_capability_advertisement.sh --merge-at-runtime --hard-dependency-not-ready-suppresses-dependent --soft-dependency-not-ready-degrades-with-declared-fallback --soft-dependency-never-suppresses --cross-service-both-directions --shared-id-takes-weaker-state --conflicting-shared-descriptor-fails-closed --effective-set-equals-ready-closure-under-hard-deps"}
- {id: 8, tier: integration, check: "scripts/test_static_thin_client.sh --real-builds --client-version N --service-version N+1 --new-module-usable-without-client-release --advisory-addition-invisible --critical-addition-yields-typed-unavailable --unknown-surface-kind-dropped --unknown-state-fails-closed --unmarked-field-defaults-critical --removed-or-critical-change-bumps-schema-version --incompatible-schema-refused-with-typed-error"}
- {id: 9, tier: integration, check: "scripts/test_capability_advertisement.sh --consumer mcp --consumer acp --consumer cli --re-advertise-effective-set --generic-dispatch-from-descriptor --invalid-args-typed-error-not-forwarded --emit-change-on-effective-set-change --absent-capability-returns-typed-capability-absent --disabled-and-unknown-externally-identical"}
- {id: 10, tier: integration, check: "scripts/test_advertisement_noninterference.sh --two-principals-differing-scope --change-outside-scope-b --assert-no-generation-advance-for-b --assert-no-event-for-b --assert-no-timing-signal-beyond-tolerance --per-scope-generation-derived-from-projection-content"}
- {id: 11, tier: mechanical, check: "scripts/check_surface_keys.sh --canonical-key-per-kind cli,tool,route,web --unique-across-merged-closure --aliases-share-verb-namespace --core-reserved-names-and-prefix-refused --duplicate-claim-refuses-registration-and-advertises-neither"}
- {id: 12, tier: mechanical, check: "scripts/check_descriptor_content_safety.sh --require-unicode-normalization --forbid-c0-c1-and-ansi-escapes --forbid-bidi-and-zero-width --forbid-confusable-script-mixing-in-canonical-keys --max-string-length --max-schema-size-depth-nodes --reject-superlinear-schema --enforced-at-validator-and-client"}
```

## Review status

Freshly drafted 2026-07-23. This proposal has **not** been through the suite roundtable and does not
inherit the 2026-07-20 approvals; it must complete its own technical-writing, architecture,
adversarial, and verification review before acceptance.

It does not modify the suite taxonomy or any shared invariant. It does, however, add normative
content beyond a pure projection, and review must treat these as new rather than inherited:

1. **Runtime readiness gating** over the hard/soft dependency graph. Invariant 16 is an install-time
   rule; the parent states no runtime readiness closure, so the gating in *Merge and re-advertisement*
   is defined here as a consuming refinement, over exactly the edges the descriptor graph declares.
2. **Canonical surface keys, the core-reserved namespace, and closed per-kind schemas**, which
   constrain what a module descriptor may declare. If review finds these amend the `module-runtime`
   descriptor contract rather than consume it, that content moves to `module-runtime` rather than
   being amended here.
3. **Per-scope generations and the noninterference property** (invariant 9), which constrain how an
   authority computes and emits change signals.

Revision history: the first draft cited "invariants 1–4" as the suite dependency law, carried an
undifferentiated `depends_on` that would have suppressed capabilities whose *soft* dependency was
unready (contradicting invariant 16), left change notification optional on network edges, used an
authority-wide generation that leaked cross-scope activity, and treated every additive descriptor
field as compatible. Roundtable review found each; this revision corrects them.
