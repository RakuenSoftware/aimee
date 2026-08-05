# Proposal: the registration chain and the static thin client

- **State:** DRAFT — 2026-07-23; awaiting roundtable review. Not part of the 2026-07-20 suite
  roundtable approval; this is a later-drafted consuming child.
- **Parent:** [`core process separation residual`](core-process-separation-residual.md)
- **Owns:** the registration chain — how a module registers with its host service, how a Runtime
  registers with a Control Plane, and how a thin client registers with a Runtime; the
  generation-stamped capability-and-surface projection each registration edge returns and refreshes;
  and the rule that makes the thin client **static**: a client binary carries no module knowledge and
  requires no release when a module ships.
- **Consumes (does not redefine):** `module-runtime` capability state, closure, and descriptors;
  `config`/[`product-governance-web-and-config.md`](../done/product-governance-web-and-config.md) effective
  catalog and activation filtering; `gateway` admission/sessions/streaming; `protocols` MCP/ACP
  mappings; the thin-client↔Runtime and Runtime↔Control-Plane transports and their principal classes
  ([`tiered-llm-p8-thinclient-mtls.md`](../done/tiered-llm-p8-thinclient-mtls.md)).
- **Implementation dependencies:** module descriptors and capability-state lifecycle
  ([`module-runtime-source-ownership-and-build.md`](../done/module-runtime-source-ownership-and-build-residual.md),
  [`aimee-core-capability-contract.md`](../done/aimee-core-capability-contract.md)); the effective config
  catalog and product boundary ([`product-governance-web-and-config.md`](../done/product-governance-web-and-config.md)).
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
(`src/server/server_http.c:642`) and the Control Plane equivalent (`src/kb/http/kb_http.c:173`)
answer with a hardcoded string list independent of what is compiled in, selected, enabled, or ready,
and nothing consumes either. The consequence is the cost this proposal exists to remove: **shipping a
module means shipping a client**. Every release drags the thin client along, and a deployment cannot
upgrade its Runtime or Control Plane and pick up the new behavior without also upgrading every client
attached to it.

The one surface that already works correctly shows the shape of the fix. MCP `tools/list` holds no
local catalog; it forwards `mcp.tools_list` to the Runtime and returns what the Runtime reports
(`src/cli_mcp_serve.c:315`), and `handle_initialize` advertises `listChanged` precisely because the
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
4. **The thin client is static and stateless.** A client binary ships no module knowledge and
   dispatches every module surface generically from the projection. It holds the current projection
   only in process memory while attached: it writes no projection, generation, capability state,
   integration catalog, or module fact to disk, and a restarted client refetches before presenting a
   surface. A client at version *N* correctly serves modules shipped in a Runtime or Control Plane at
   version *N+k* within the compatibility window; only a change to the core transport, registration,
   or handshake contract requires a client release.
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
([`core process separation residual`](core-process-separation-residual.md),
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
consumes it, through that consumer's own protocol: the MCP integration and `initialize` capability
object, `listChanged` notification, the ACP capability handshake (`src/acp_registry/agent.json`), and
the CLI/web surfaces. This is playback of the received closure, not a locally-authored catalog. If a
consumer's standard integration API persists configuration, that persisted artifact is
**consumer-owned output** reproduced from the current server projection; it is not an Aimee client
cache, and the client neither consults it as authority nor retains a second copy. The next invocation
still refetches from the Runtime. Consumer output therefore cannot make a thin client a state owner.

### Why registration is upward and not the reverse

The registrant is the party whose availability is contingent; the authority is the party that
persists and arbitrates. Pushing downward would require each authority to discover, address, and
maintain liveness for an unbounded, churning set of dependents — a Control Plane tracking every
Runtime, a Runtime tracking every client — and to hold credentials for connecting *to* them. Upward
registration inverts all three: the registrant knows exactly one address, initiates the only
connection, and authenticates itself under the transport class it already holds. This also matches
the transport that exists — the thin client's remote path probes and attaches to its Runtime and has
no Control Plane connection at all (`remote_health_ok`, `src/cli_remote.c:307`) — and it keeps the
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

A canonical key claimed by two capabilities is a **registration error**, and the resolution is
**incumbent-preserving**: the already-admitted claimant keeps the key and stays advertised, and the
*later* registration is rejected atomically — the whole registration, so a module cannot partially
land — with a typed conflict naming both claimants and an audit record.

Incumbent-preserving is a security requirement, not a tie-break preference. Withdrawing both
claimants on conflict would hand any admitted module a **denial-of-service primitive**: claim the
verb, tool name, or route of an incumbent it wants silenced, and the incumbent's surface disappears
even though the attacker's own registration failed. Refusal must never cost the incumbent anything.
Replacing an incumbent's key is possible only through a separately authorized **transactional
replacement** — withdraw-then-claim as one atomic operation, never a side effect of an ordinary
registration. Its authorization is specified rather than implied: the operation is permitted only to
a principal holding the incumbent capability's own authorization under a `cert:CN` transport class
(never a bare bearer), is available at registration time only — not as a live runtime mutation, so a
verb cannot change owner under a session in flight — and emits an audit record naming both
capabilities, the principal, and the transport class. A Runtime-local module that believes it should
own a key an incumbent holds does not get it by racing or by priority; an operator performs the
replacement deliberately.

Because "incumbent" is decided by order, the order is specified rather than left to whoever wins a
race. Uniqueness is enforced by **the authority that holds the merged closure** — the Runtime — since
it is the only party that sees both services' keys. Admission order is: Control-Plane capabilities
first, then Runtime-local capabilities, then within each group the descriptor graph's profile order,
which is already deterministic for a given profile. So when a Runtime module and a Control-Plane
module claim the same key, the Control-Plane claimant is the incumbent and the Runtime one is
refused — the same way on every start, on every node, with the same profile.

**That order only holds if the startup sequence produces it,** so the sequence is part of the
contract rather than an implementation detail. A Runtime, on start: (1) attempts registration with
its Control Plane and waits for the returned projection, bounded by a startup deadline; (2) admits
the Control-Plane capabilities into the merged closure; (3) only then admits its own modules' keys;
(4) begins serving client registrations. Serving clients before step 3 would expose a closure that is
about to change; admitting local keys before step 2 would make a Runtime module the incumbent purely
because it started first.

The unhappy path is stated because it is the one that actually varies: if the Control Plane is
unreachable within the startup deadline, the Runtime proceeds to step 3 and serves its local
capabilities — the outage rule already says an outage must not withhold independent Runtime-local
capabilities. A Control-Plane capability arriving later that claims a key a local module now holds is
**refused**, because incumbent-preservation outranks admission order: rank decides who wins a
*simultaneous* claim, never who may evict a live one. That refusal is a configuration error reported
to the operator with both claimants named, not a silent reordering, and it is resolved by the
authorized transactional replacement or by changing a descriptor — never automatically.

Separately, core **reserves** the names of the verbs it owns (the attach/remote, identity/enrollment,
health, help, and version verbs) plus a reserved prefix; a module descriptor claiming a reserved name
is rejected at validation, before it can ever be registered. Aliases are checked identically to verbs
— an alias is a claim on the namespace, and alias-shadowing is the cheapest impersonation path.

### The normative wire schema

"Closed schema" is only mechanically checkable if the schema itself is written down. The following is
the **normative semantic content** of the projection wire form — the same device
`module-runtime-source-ownership-and-build.md` uses for its ownership baseline. Key order is
irrelevant; no entry may be omitted, aliased, or weakened, and unknown keys are rejected.

```yaml
schema_version: 1
envelope:
  role: {enum: [runtime, control], required: true}
  version: {type: string, required: true}
  epoch: {type: string, required: true}          # opaque, compared for equality only
  generation: {type: uint64, required: true}     # per-scope, monotonic
  schema_version: {type: uint32, required: true}
  mode: {enum: [notified, bounded-revalidation], required: true}
  revalidate_after_ms: {type: uint32, required: true}
  stale_after_ms: {type: uint32, required: true}
  heartbeat_ms: {type: uint32, required: when_notified}
  half_open_after_ms: {type: uint32, required: when_notified}
  reconnect_deadline_ms: {type: uint32, required: when_notified}
  # `when_notified` means: MUST be present and non-zero when mode == notified, and MUST be
  # absent when mode == bounded-revalidation. Present-but-ignored is not permitted, so a
  # registrant never has to decide whether a stale deadline applies. The relations below
  # that mention these fields are evaluated only in notified mode.
  round_trip_budget_ms: {type: uint32, required: true}  # p99 allowance for one conditional exchange
  hop_bound_ms: {type: uint32, required: true}          # THIS hop's worst-case contribution
  propagation_bound_ms: {type: uint32, required: true}  # end-to-end: this hop + all upstream hops
capability:
  id: {type: capability_id, required: true}
  kind: {enum: [required, optional], required: true}
  state: {enum: [absent, selected, disabled, starting, ready, degraded,
                 unavailable, stopping, failed], required: true}
  unavailable_reason: {enum: [dependency, stale, schema_too_old, policy], required: false}
  # `policy` is permitted ONLY for a capability the caller is authorized to SEE but currently
  # forbidden to USE. A capability the caller is not authorized to see is absent from the
  # projection entirely (see "Absent means absent") and is NEVER projected as
  # unavailable/policy -- doing so would disclose its existence and defeat the
  # authorization scoping. Withholding and forbidding are different outcomes.
  # Legal only when state is one of {unavailable, degraded, failed}; the validator rejects
  # a reason on any other state (e.g. `ready` + `dependency`) as a contradictory record.
  # `degraded` carries `dependency` only when a soft dependency is unready.
  # `provenance` is deliberately NOT a field of the client-visible projection.
  # It exists only in an authority's internal merged closure and on the
  # Runtime↔Control-Plane edge. See "Topology is not projected downward".
  hard_depends_on: {type: list<capability_id>, required: true}   # may be empty
  soft_depends_on: {type: list<soft_dep>, required: true}        # may be empty
  generation: {type: uint64, required: true}
  surfaces: {type: list<surface>, required: true}
  advisory_ext: {type: ext_map, required: false}
soft_dep:
  id: {type: capability_id, required: true}
  fallback: {type: bounded_string, required: true}
surface:
  kind: {enum: [cli, tool, route, web], required: true}
  advisory_ext: {type: ext_map, required: false}
  # exactly one of the four bodies below, matching `kind`
ext_map:
  # The ONE place unknown keys are permitted. Everything placed here is, by
  # construction, presentation metadata an older client may ignore with no
  # behavioral consequence. Values are scalars or bounded strings only —
  # never nested objects, never anything a client must act on.
  keys: {type: name_token}
  values: {type: scalar_or_bounded_string}
  max_entries: 32
cli:
  verb_path: {type: list<name_token>, required: true, max_len: 4}
  aliases: {type: list<name_token>, required: true}
  args: {type: list<arg>, required: true}
  help: {type: bounded_string, required: true}
  long_help: {type: bounded_string, required: false}
  tier: {enum: [core, advanced, admin], required: true}
  hidden: {type: bool, required: true}
arg:
  name: {type: name_token, required: true}
  form: {enum: [positional, flag], required: true}
  type: {enum: [string, int, uint, bool, enum, duration_ms, capability_id], required: true}
  # Per-type bounds, so the linearity argument and the content-safety rules are one
  # validator pass rather than two: string -> bounded_string (charset/NFC/length);
  # int/uint -> 64-bit, rejected outside [INT64_MIN+1, INT64_MAX-1]; duration_ms ->
  # uint64 <= INT64_MAX-1; enum -> enum_values required, each a name_token, <= 64 values;
  # capability_id -> capability_id rules. `default` must satisfy its declared type's bounds.
  enum_values: {type: list<name_token>, required: false}   # required iff type == enum
  repeated: {type: bool, required: true}
  required: {type: bool, required: true}
  default: {type: scalar_of_declared_type, required: false}
  help: {type: bounded_string, required: true}
tool:
  name: {type: name_token, required: true}
  description: {type: bounded_string, required: true}
  input: {type: list<arg>, required: true}       # NOT arbitrary JSON Schema
route:
  method: {enum: [GET, POST, PUT, PATCH, DELETE], required: true}
  path: {type: normalized_path, required: true}
web:
  surface: {type: web_surface_enum, required: true}   # closed enum owned by the product boundary
hop_bound_computation:
  # COMPUTED by the authority, not configured independently; the validator recomputes it.
  # Every term includes the exchange that actually delivers the change -- an interval
  # elapsing is not a delivery -- AND the case where that exchange never completes.
  # request_timeout_ms bounds a conditional request that stalls (packets dropped
  # mid-request): without it, one dropped GET makes the hop unbounded no matter what the
  # interval says. The registrant detects the stall by its own request timeout, then
  # retries on the next interval.
  #   request_timeout_ms := round_trip_budget_ms * 2   (derived, not configured)
  #   one_revalidation   := revalidate_after_ms + request_timeout_ms + round_trip_budget_ms
  #   bounded-revalidation hop: one_revalidation
  #   notified hop (healthy):   heartbeat_ms + round_trip_budget_ms
  #   notified hop (failed):    half_open_after_ms + reconnect_deadline_ms + one_revalidation
  #   hop_bound_ms := max(healthy, failed)   -- worst case, since a stream may be dead
envelope_validity_relations:
  # Enforced by the validator and re-checked by the registrant. A projection violating any
  # of these is malformed and fails closed.
  #   half_open_after_ms   >  heartbeat_ms        (detect only after a heartbeat is genuinely late)
  #   stale_after_ms       >  revalidate_after_ms (ordinary revalidation keeps a healthy peer live)
  #   stale_after_ms       >= propagation_bound_ms (never withdraw faster than change can arrive)
  #   hop_bound_ms         == hop_bound_computation above (recomputed, never trusted)
  #   propagation_bound_ms >= hop_bound_ms
propagation_rule:
  # The sum ALONG THE ACTUAL CHAIN, not a single constant:
  #   propagation_bound_ms := hop_bound_ms + upstream.propagation_bound_ms
  # An authority with no upstream (a Control Plane) reports propagation_bound_ms == hop_bound_ms.
  # A Runtime adds its own hop to the value its Control Plane advertised, so the client's
  # figure is the true module→consumer worst case over the topology in force.
limits:
  name_token: {charset: "[a-z0-9][a-z0-9-]*", max_bytes: 64, nfc: true}
  bounded_string: {max_bytes: 4096, nfc: true, control_chars: forbidden}
  capability_id: {charset: "[a-z0-9][a-z0-9-]*", max_bytes: 128, nfc: true}
  normalized_path: {form: "percent-decoded, dot-segments resolved, no trailing slash,
                          lowercase scheme-relative, max 8 segments", max_bytes: 512}
  max_args_per_surface: 32
  max_surfaces_per_capability: 16
  max_capabilities_per_projection: 4096
  max_projection_bytes: 4194304
  # advisory_ext is module-controlled, so it is bounded per capability AND in aggregate.
  # The per-entry and per-map caps alone bound one record; without an authority-wide cap a
  # module could register many capabilities each at the maximum and amplify server memory.
  # ENFORCEMENT SITE IS PART OF THE CONTRACT, not an implementation choice: the authority
  # maintains a running total of admitted advisory bytes and checks it AT REGISTRATION,
  # rejecting the whole registration atomically when admitting it would exceed the cap.
  # Deferring to projection time would let a module register unbounded and fail only on
  # read (leaving the memory already spent), and a periodic sweep would race admission.
  # The running total is authority-lifetime state keyed by capability id, decremented on
  # withdrawal; overflow returns the same typed registration error as any other refusal.
  max_advisory_bytes_per_capability: 4096
  max_advisory_bytes_per_authority: 16777216
```

Two consequences worth stating, because they are the reason the checks are decidable:

- **The argument model is a closed list of scalar types, not a JSON Schema dialect.** `tool.input`
  reuses the same `arg` list as `cli`. This is a deliberate restriction: admitting general JSON
  Schema would make "reject a schema whose validation cost is not linear" undecidable in practice
  (`$ref` cycles, nested `allOf`/`anyOf`, backtracking patterns). With a flat list of typed scalars
  and a bounded `max_args_per_surface`, validation is O(args) by construction, so the linearity
  requirement is satisfied structurally rather than analyzed per descriptor. A module needing richer
  input shapes takes them as a declared scalar it parses itself, server-side.
- **Every string type is length-bounded and NFC-normalized at the schema level**, so the content
  rules below are enforced by the same validator pass rather than a separate best-effort scan.

Where a richer projection is needed later, `schema_version` increments and the negotiation rules in
*The static client* apply; this block is version 1.

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
[`module-runtime-source-ownership-and-build.md`](../done/module-runtime-source-ownership-and-build-residual.md) —
whose descriptor contract already declares "routes/commands/protocols". This proposal fixes their
projected wire form and requires the projection be **derived from** those declarations, never authored
separately. The projection introduces no capability, no state, no surface, and no dependency edge that
the descriptor graph and config activation filtering do not already declare.

An `optional` module **omitted** from the build closure is **absent from the projection entirely**
(not listed as `disabled`) — consistent with the suite rule that omission leaves no residue;
`disabled` is reserved for a selected module whose runtime lifecycle is off.

The projection is **authorization-scoped**: it is what *this* principal and transport class may see
and use, not a full inventory. A stronger transport class (`cert:CN`, per
[`tiered-llm-p8-thinclient-mtls.md`](../done/tiered-llm-p8-thinclient-mtls.md)) may be shown capabilities a
bare bearer is not. Scoping reuses the existing gateway identity/capability gate; it defines no second
authorization model. Surface descriptors are scoped with their capability — an unauthorized
capability's surfaces are absent, not merely non-invocable.

## The static client

The client's contribution to the chain is a **client capability declaration**: its protocol version,
its projection `schema_version`, and the surface kinds it can render. The Runtime filters the
projection to that declaration. An old client that does not render `web` surfaces is simply not sent
them; it is not broken by their existence.

**Compatibility rules that make version independence real:**

0. **The view is ephemeral.** The declaration and returned projection live only for the attached
   process. The client may retain them in memory to validate and forward calls during that attachment,
   but must not persist them through a file, database, keychain, environment rewrite, or consumer
   integration artifact. On restart it has no effective set until registration succeeds again.
   Durable identity credentials and the operator-selected Runtime endpoint belong to the transport
   bootstrap contract, not the capability/integration client, and must not be used to smuggle module
   or surface state into the thin client.
1. **Unknown keys are rejected everywhere except one designated place.** Ignoring an unknown field is
   safe exactly when the field cannot change what the client *does* — it is presentation metadata. It
   is unsafe when the field constrains validation, routing, authorization, or invocation, because
   ignoring it means acting without a constraint the author required. A per-field `advisory` /
   `critical` *marking* cannot carry this, because an old client meeting an unknown key has no way to
   read a marking attached to a key it does not know. So the distinction is **positional**, not
   annotational:
   - Every defined field in the schema is closed and **critical by construction**. An unknown key at
     any defined position is a malformed document: the capability is projected `unavailable` with
     `schema_too_old`, never silently parsed.
   - The single `advisory_ext` map is the **only** position where unknown keys are legal. Anything an
     authority places there is advisory by definition — an old client ignores unrecognized entries
     with no behavioral consequence, and a new client may use them. Its values are scalars or bounded
     strings only, so nothing there can ever encode a constraint.

   An authority therefore cannot introduce a behavior-affecting field that an old client would
   silently ignore: to be behavioral it must be a defined field, and a new defined field bumps
   `schema_version` and is negotiated per rule 4.

   **Defined fields are additive-only; a rename is not a rename.** Renaming a defined field —
   `requires_acknowledge` becoming `requires_confirmation` — must never be treated as one edit. It is
   a removal plus an addition, and rule 4 already bumps `schema_version` for each, so the negotiation
   path applies and an old client gets `schema_too_old` rather than a silently dropped constraint.
   Stating it explicitly because the failure mode is quiet and the mistake is natural: if a rename
   *were* handled as a single in-place edit at an unchanged version, both sides would parse
   successfully under their own schemas and the constraint would evaporate — exactly the class of
   silent downgrade this section exists to prevent. The validator therefore rejects any schema
   revision in which a defined field name disappears without a `schema_version` increment. The old client's parse is decidable in both
   directions — it knows every key it must understand, and it knows exactly one region it may skip.
2. **Unknown surface kinds are dropped, not fatal.** A capability offering only surface kinds the
   client cannot render is omitted from that client's effective set; a capability offering a mix is
   presented through the kinds it can render.
3. **Unknown state values fail closed.** A lifecycle state the client does not recognize is treated as
   not-`ready`, so a newer state name can never be mistaken for availability.
4. **Additive-advisory is free; anything behavioral is negotiated — per capability, not per
   connection.** Adding a capability, a surface, or an `advisory_ext` entry does not bump
   `schema_version`. Adding a **defined field**, removing any field, narrowing a meaning, or changing
   a state's semantics **does** — a behavior- or security-affecting addition is a version change even
   though it is additive, because an older client cannot honor it.

   Down-negotiation and critical fields must not be allowed to contradict each other. Serving an N
   client the older representation of a capability that has since gained a critical constraint would
   *erase* that constraint and leave the capability invocable without it; sending the newer
   representation would break negotiation; refusing the whole registration would take down every
   other capability too. The resolution is that **negotiation is per capability**, using this exact
   algorithm:

   1. The connection is admitted at `min(client_declared, authority_max)` — call it *V*. Registration
      is refused outright only if *V* is below the authority's minimum supported version, which is a
      genuine core-transport incompatibility.
   2. A capability whose complete representation fits within *V* is projected at *V*, normally.
   3. A capability that requires any **critical** field not expressible at *V* is projected at *V*
      as `state: unavailable` with `unavailable_reason: schema_too_old`. Its surfaces are omitted.
      This is expressible in every version — `unavailable` is in the base vocabulary — so the old
      client understands it exactly, without receiving a field it cannot parse.
   4. Authorities **retain** the older representation of a capability only for versions inside the
      supported window; outside it, rule 3 applies. Older representations are *derived* from the
      current descriptor by projection, never hand-maintained in parallel, so they cannot drift.
      Derivation is **cached per (capability, generation, schema_version)** and invalidated when the
      generation advances, so a projection costs one derivation per distinct version actually in use
      rather than one per request, and the supported-window width adds memory rather than per-request
      latency. The window is bounded by policy; an authority that cannot afford it narrows the window,
      which moves capabilities to `schema_too_old` visibly instead of degrading latency silently.
   5. The authority **rejects the invocation** of a capability it projected as `schema_too_old`,
      independently of what the client attempts. The old client's inability to see the constraint is
      never the only thing enforcing it.

   The N→N+k guarantee is therefore bounded precisely: an N client keeps working across any number of
   *advisory* additions and any number of new modules, loses only the specific capabilities that
   gained a constraint it cannot express, is told exactly which and why, and cannot invoke those
   capabilities even if it tries.
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

**A capability record is rendered even when it carries no surfaces.** The effective set is enumerated
from **capability records**, not from surfaces. This distinction is load-bearing: a capability that is
`unavailable` — because a hard dependency is not ready, because its projection is stale, or because it
requires a schema the client cannot express — has its surfaces omitted, so a dispatcher that built its
view by walking surfaces would render *nothing* and the capability would vanish silently. That would
contradict the guarantee that the user is told which capabilities were lost and why. So whenever a
record is present in the projection, the consumer is shown its `id`, `state`, and
`unavailable_reason`, with its surfaces absent and un-invocable. "Not offered" and "not mentioned" are
different outcomes: a capability outside the projection entirely is absent (see *Absent means
absent*), while a capability inside it that cannot currently be used is **visibly** unusable.

**Local execution is out of scope.** Some surfaces genuinely need the client's own machine — the
filesystem, the TTY, an editor integration. This proposal does **not** define a client-side native
handler mechanism for them; those surfaces remain core-owned client verbs. Introducing module-supplied
local execution would put untrusted module intent on the client host and is a separate proposal with
its own trust analysis. A module descriptor may not declare a surface requiring client-local
execution; the descriptor validator rejects it.

## Delivery: on registration and on change

The Runtime and the Control Plane each serve the projection over their existing `/v1` surface,
mirroring the fail-closed **readiness provider** pattern already in place
(`server_http_set_ready_provider`, `src/server/server_http.c:669`; `/v1/ready`) rather than the static
string list. A registered provider samples the live closure **off** the request path; with no provider
registered the surface fails closed exactly as `/v1/ready` does today, advertising nothing rather than
advertising a stale list.

- **On registration.** The thin client already probes `GET /v1/health` and pins the Runtime cert when
  it attaches (`src/cli_remote.c:321`, `remote_pin_cert`/`remote_set`). Registration extends that
  attach: the client presents its capability declaration and holds the returned projection with its
  `epoch`/`generation` only in process memory for the life of that attachment. The Runtime registers
  with the Control Plane the same way at its own startup and on reconnect.
- **On change — every network edge has a defined mechanism, not an optional one.** Each network
  registration edge (client→Runtime, Runtime→Control Plane) operates in exactly one of two modes,
  chosen at registration and reported in the projection so both ends know which is in force:
  - **Notified.** The authority holds an open change stream to the registrant and pushes the new
    per-scope `generation` on change; the registrant then refetches conditionally. This is the
    default whenever the transport supports it. A stream is only a bound if its own liveness is
    bounded, so notified mode carries three deadlines — **transmitted in the envelope**, not merely
    assumed: `heartbeat_ms`, which the authority emits on regardless of change; `half_open_after_ms`,
    after which a registrant that has seen neither change nor heartbeat declares the stream dead; and
    `reconnect_deadline_ms`. Beyond it, **fallback is mandatory** — a registrant that cannot
    re-establish the stream within the reconnect deadline reverts to bounded revalidation rather than
    waiting on a stream that may never speak again.

  **Only semantic changes advance a generation.** Advancing on *any* descriptor edit would make a
  reworded help string wake every attached client and cascade a refetch down the whole chain — a
  notification storm for a change that alters nothing a client can act on. So the fields split:
  changes to `verb_path`, `aliases`, arg `name`/`form`/`type`/`enum_values`/`repeated`/`required`,
  `tier`, `hidden`, `tool.name`, `route.method`/`path`, `web.surface`, any capability `state`, or any
  dependency list are **semantic** and advance the generation. Changes confined to `help`,
  `long_help`, `description`, or `advisory_ext` are **cosmetic**: they are carried in the next
  projection a registrant fetches for any other reason, and never trigger one on their own.

    A notified hop contributes `heartbeat_ms + round_trip_budget_ms` on the healthy path and
    `half_open_after_ms + reconnect_deadline_ms + one_revalidation` on the failure path — where
    `one_revalidation` is itself `revalidate_after_ms + request_timeout_ms + round_trip_budget_ms`,
    exactly as `hop_bound_computation` defines it. Its advertised `hop_bound_ms` is the **worse** of
    the two, since a registrant cannot know in advance whether its stream is alive. Two terms here are easy to drop and both are
    load-bearing: the post-fallback revalidation, because detecting a dead stream and abandoning
    reconnection does not by itself deliver the change; and the round trip, because an interval
    elapsing is not a delivery either — the conditional exchange still has to complete. Omitting
    either would let a conformance test certify a bound the specified behavior cannot meet on any
    real network. A silently broken stream therefore degrades to the revalidation bound; it never
    degrades to never.
  - **Bounded revalidation.** Where no stream can be held, the registrant revalidates conditionally
    on a **bounded interval** the authority states in the projection (a cheap generation-only
    request that returns not-modified in the common case).

  A registration that can establish neither mode is refused with a typed error rather than admitted
  in a mode where change would never arrive. A change to the closure, to any advertised state, or to
  any surface descriptor advances the affected scopes' `generation`. An `epoch` change (process
  restart) forces a full refetch and drops the in-memory view.
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
  The freshness window is required to exceed the revalidation interval, so ordinary revalidation keeps
  a healthy client live and only genuine loss of contact degrades it.
- **An outage degrades by provenance and dependency, not wholesale.** "Loss of the Control Plane
  degrades the merged closure" is too coarse to implement safely: read literally, an implementation
  could withdraw everything and turn a Control Plane outage into a total Runtime feature outage. The
  blast radius is bounded exactly: when the Control Plane is unreachable, the capabilities that become
  `unavailable` are (a) those whose **provenance** is the Control Plane, and (b) Runtime-local
  capabilities that **hard-depend**, transitively, on one of those. Every other Runtime-local
  capability **remains available and is not withdrawn**. A Runtime-local capability that only
  *soft*-depends on a Control-Plane capability goes `degraded` with its declared fallback, never
  unavailable. The same rule applies symmetrically to any authority losing its upstream. The Runtime
  keeps serving what it alone can answer for; the client sees a smaller set, not an empty one.

### Topology is not projected downward

Provenance is what makes the rule above computable, and it is exactly the kind of fact a client must
not be told. A `provenance` field in the client-visible projection would disclose both that a Control
Plane exists and which capabilities come from it — contradicting the requirement that the client know
only what its Runtime knows. So provenance is **authority-internal**: it lives in the Runtime's merged
closure and travels on the Runtime↔Control-Plane edge, and it is absent from the bytes a client
receives. The Runtime computes the outage scope from it and projects only the *result* — capabilities
that are now `unavailable`.

The reason codes are constrained for the same purpose. A client-visible
`unavailable_reason: upstream_unreachable` would leak the existence of an upstream just as surely as
a provenance field. The client-visible enum is therefore closed to reasons that describe the client's
own relationship to its authority (`dependency`, `stale`, `schema_too_old`, `policy`); an upstream
outage reaches the client as `dependency`, which is true — the capability's dependency is not ready —
without describing where that dependency lived. Richer topological reasons may exist on the
Runtime↔Control-Plane edge and in operator-facing surfaces, which are not this projection.

Generalized: a registrant is told what it may *use*, never what its authority is *made of*. Any
future field naming another service, its address, its health, or a capability's origin belongs on the
upstream edge, not in the downward projection.

**A schema rule is not enough; the whole path is in scope.** Forbidding a `provenance` *field*
constrains only the document the codec emits. The projection path can leak the same fact through
channels the schema never sees — a debug log naming the upstream service, an HTTP header, a sidecar
audit record, an error string quoting an upstream address, a trace span with the peer's identity.
This is the precise shape of the mistake revision 3 made: a rule that looked complete because it
covered the structure being reviewed, while the disclosure lived elsewhere. So non-disclosure is a
property of the **path**, not the schema: no artifact the projection path emits on behalf of a client
request — response bytes, headers, logs, traces, metrics labels, or audit records visible at the
client's trust level — may contain another service's identity, address, health, or a capability's
origin. Enforcement is a deny-list of those tokens applied to everything the path can emit, with a
fixture that injects a provenance value and asserts it appears nowhere in any emitted artifact
(check 13). Operator-facing surfaces at the authority's own trust level are unaffected.

**Single-Runtime scope.** This holds for the three-edge chain defined here, in which each registrant
has exactly one authority. It is *not* established for a topology where Runtimes exchange
projections with each other — in a mesh or federation, provenance would travel sideways and a peer
would learn another peer's composition. Peer-Runtime and multi-Runtime topologies are out of scope
and require their own proposal with their own disclosure analysis; nothing here should be read as
having cleared them.

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
  it returns a typed `capability_absent` indistinguishable from a request against a capability that
  never existed, so the wire cannot distinguish *disabled* from *never-existed* (mirroring the
  web-route rule in `product-governance-web-and-config.md`).

  "Indistinguishable" is only enforceable if the wire result is written down, so it is normative
  rather than descriptive:

  ```yaml
  capability_absent:
    status: 404
    body: {error: {code: "capability_absent"}}   # exact bytes, no id echo, no detail field
    headers: no capability-specific header, no varying Content-Length
    timing: drawn from the same distribution as an unknown capability; the handler
            resolves absent and never-existed on the same path, so no branch on
            "existed but withheld" occurs before the response is emitted
  ```

  The response must not echo the requested id, name a reason, or vary in length — each would
  reintroduce the distinction the rule exists to remove. Note this is deliberately *different* from a
  capability that is present-but-`unavailable`: that one is visible in the projection with its state
  and reason (see *A capability record is rendered even when it carries no surfaces*), because the
  client is authorized to know it exists. Absent means the client is not authorized to know that, so
  it learns nothing at all.

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
9. **Noninterference across scopes, over deterministic channels.** A caller's `generation`, the
   change events it receives, and the bytes of its projection are functions of its own authorized
   projection alone: a change confined to capabilities outside a caller's scope produces no
   generation advance, no event, and no byte difference for that caller. This invariant is scoped
   deliberately to **deterministically observable** channels. Wall-clock timing is *not* claimed as a
   covert-channel-free surface — a shared process has shared caches, allocators, and schedulers, and
   asserting timing noninterference would be a promise the implementation cannot keep and a test that
   would fail nondeterministically in CI. What is required and tested instead is that the projection
   path performs **no work proportional to out-of-scope content**: scope filtering happens before
   per-capability work, so cost is a function of the caller's own projection size. Residual
   microarchitectural timing is documented as out of scope, not silently claimed.
10. **Canonical-key uniqueness.** No two capabilities in the merged closure claim the same canonical
    key for a kind, and no module claims a core-reserved name or prefix. A violation rejects the
    **later** registration atomically while the **incumbent remains admitted and advertised**; a
    conflict can never withdraw an existing surface. Replacing an incumbent's key is possible only
    through an authorized transactional replacement.
11. **No silent behavioral downgrade.** A capability whose representation contains an unknown key at
    any defined position is projected `unavailable` with `schema_too_old`, never rendered as if the
    key were absent. Only `advisory_ext` entries may be skipped, and nothing placed there can encode
    a constraint.
12. **Topology is not projected downward.** A client-visible projection contains no field naming
    another service, its address, its health, or a capability's origin. Provenance and topological
    reason codes exist only in an authority's internal closure and on its upstream edge.
13. **The thin client owns no durable capability or integration state.** Exiting the client destroys
    its effective-set view; restarting with the same home/config presents no module CLI verb, MCP
    integration, tool, route, help, schema, generation, or availability state until the Runtime
    projection is fetched again. Consumer-owned configuration written through a consumer's standard
    integration API is treated only as rendered output and is never read back as Aimee authority.

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

**Each flag is a separately reported assertion.** The checks below are written as one invocation per
concern, but the flag lists bundle many independent assertions, and a single exit code would let one
passing assertion mask a failing one — or let an unimplemented assertion contribute silently. So the
contract on every script here is: it emits one machine-readable result **per flag** (`flag`,
`pass|fail|not-implemented`, and on failure the observed value), a flag with no implementation
reports `not-implemented` rather than passing by omission, and the run fails if *any* flag is not
`pass`. This is what makes a long flag list acceptable — it is a list of named assertions with
individual verdicts, not a checklist collapsed into one boolean. Reviewers read the per-flag report,
not the exit status.

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
  are usable through that same client binary. The "unchanged" half is a hash comparison rather than a
  review judgement — but a hash comparison is only meaningful against a **reproducible build**, and
  an ordinary C toolchain defeats it by embedding build timestamps, build IDs, and absolute paths
  that differ run to run even when no source changed. So the check requires a normalized build:
  `SOURCE_DATE_EPOCH` pinned, deterministic archive and link order, no `__DATE__`/`__TIME__`, build
  paths remapped, and build ID either omitted or derived only from input content. The check
  **self-tests that contract first** — it builds the unmodified tree twice and requires those two
  hashes to match before comparing anything else, so a toolchain that cannot reproduce fails as a
  harness error rather than as a false report about module leakage. Where reproducibility genuinely
  cannot be achieved on a platform, the fallback assertion is that no file under the client's source
  closure differs, which is weaker and must be reported as such rather than silently substituted.
- **Propagation latency** (check 5) is measured, not asserted: a fixture module transitions state,
  and the elapsed time until the consumer observes the change must be at or below the sum of the
  configured per-hop bounds, in both notified and bounded-revalidation modes. The test also asserts
  the *addition* case — a module installed after the client attached becomes visible within that same
  bound — since expiry alone cannot discover additions.
- **No-client-to-Control-Plane** (check 5) is decided by network observation with a defined capture
  point: the Control Plane is reachable from the client's namespace only via a capturing interface,
  capture runs from before client start to after clean exit, and the check fails on any packet from
  the client addressed to it. The configuration half is a defined scan, not "inspect memory": the
  client's on-disk configuration, environment, and command line are scanned for the Control Plane's
  address and credential in plaintext, percent-encoded, and base64 representations. A live-memory
  scan is deliberately *not* claimed — it has no reliable capture point or lifecycle and would pass
  vacuously; the enforceable property is that no Control Plane address or credential is ever
  *delivered* to the client, which the network and configuration observations together decide.
- **Noninterference** (check 10) is a differential test over deterministic channels only: two
  principals with different authorized scopes attach; a change confined to capabilities outside
  principal B's scope must produce, for B, no generation advance, no event, and a byte-identical
  projection. The work-proportionality property is checked structurally, and the structural claim is
  stated precisely enough to test: **the scope decision for a capability requires only its `id` and
  authorization tag** — not its surfaces, dependencies, state, or `advisory_ext` — and is taken
  *before* any expansion or allocation on that capability. "Filters first" is otherwise satisfiable
  by a path that expands a record and then discards it, which does work proportional to
  out-of-scope content. The fixture constructs an out-of-scope capability carrying a maximal
  `advisory_ext` blob and asserts the projection performs no allocation attributable to it. No
  wall-clock tolerance is asserted, because none can be honored.
- **N→N+k compatibility** (check 8) uses two real builds, not a mocked version string: client at N,
  service at N+1, with the N+1 service adding an advisory field, a critical field, a new surface
  kind, and a new module. Pass requires the advisory addition invisible, the critical addition
  producing typed unavailability, the new kind dropped, and the new module usable.

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_capability_advertisement.sh --projection-only --equals-module-runtime-closure --forbid-static-capability-list src/server/server_http.c,src/kb/http/kb_http.c --require-generation-epoch-schema-version --require-typed-state --omitted-optional-absent --disabled-only-for-selected"}
- {id: 2, tier: mechanical, check: "scripts/check_capability_advertisement.sh --authorization-scoped --require-transport-class-scoping bearer,cert-cn --scope-surfaces-with-capability --forbid-unauthorized-capability-leak --no-second-authorization-model"}
- {id: 3, tier: mechanical, check: "scripts/check_surface_descriptors.sh --conform-to-normative-wire-schema-v1 --derived-from-descriptors-only --kinds cli,tool,route,web --closed-schema-per-kind --reject-unknown-field --arg-types-from-closed-scalar-set --forbid-general-json-schema-dialect --assert-state-reason-validity-relation --reject-reason-on-ready-state --reject-defined-field-rename-without-schema-version-bump --reject-interpretable-value path,url,host,commandline,mimetype,renderer --web-identifier-from-closed-enum --declarative-only --forbid-executable-content --forbid-template-language --forbid-client-handler-binding"}
- {id: 4, tier: mechanical, check: "scripts/check_static_thin_client.sh --command-table src/cmd_table.c --allow-core-verbs-only --forbid-module-verb --forbid-module-tool-name --forbid-module-route --forbid-module-help-text --forbid-module-arg-schema --reproducible-build-self-test-builds-twice-and-requires-match --source-date-epoch-pinned --deterministic-archive-and-link-order --no-date-time-macros --build-paths-remapped --client-binary-hash-identical-when-module-added --fallback-no-source-closure-diff-reported-as-weaker"}
- {id: 5, tier: integration, check: "scripts/test_registration_chain.sh --module-to-host --runtime-to-control --client-to-runtime --registrant-contacts-only-its-authority --capture-from-before-start-to-after-exit --assert-no-client-to-control-packet-observed --scan-config-env-argv-for-control-address-and-credential plaintext,percent,base64 --measure-propagation-latency-vs-sum-of-hop-bounds --assert-addition-discovered-within-bound --both-modes notified,bounded-revalidation --healthy-notified-hop-bound-equals-heartbeat-plus-roundtrip --failed-notified-hop-bound-equals-halfopen-plus-reconnect-plus-revalidate-plus-roundtrip --hop-bound-is-worse-of-healthy-and-failed --assert-failure-path-includes-post-fallback-revalidation --assert-bound-includes-round-trip --measure-against-same-bound-validator-enforces --cosmetic-help-edit-does-not-advance-generation --semantic-edit-does-advance-generation --drop-stream-falls-back-to-revalidation --half-open-stream-detected-within-deadline"}
- {id: 6, tier: integration, check: "scripts/test_capability_advertisement.sh --on-registration-snapshot --generation-advances-on-closure-state-and-surface-change --epoch-change-forces-refetch --stale-window-exceeds-revalidation-interval --stale-window-fails-closed --control-outage-withdraws-only-control-provenance-and-hard-dependents --assert-independent-runtime-local-capabilities-remain-available --soft-dependent-degrades-with-fallback-not-unavailable --assert-outage-surfaces-to-client-as-dependency-reason --registration-refused-when-neither-mode-available"}
- {id: 7, tier: integration, check: "scripts/test_capability_advertisement.sh --merge-at-runtime --hard-dependency-not-ready-suppresses-dependent --soft-dependency-not-ready-degrades-with-declared-fallback --soft-dependency-never-suppresses --cross-service-both-directions --shared-id-takes-weaker-state --conflicting-shared-descriptor-fails-closed --effective-set-equals-ready-closure-under-hard-deps"}
- {id: 8, tier: integration, check: "scripts/test_static_thin_client.sh --real-builds --client-version N --service-version N+1 --new-module-usable-without-client-release --advisory-ext-addition-invisible-to-old-client --new-defined-field-yields-unavailable-reason schema_too_old --unknown-key-at-defined-position-yields-schema-too-old --advisory-ext-is-only-unknown-key-position --assert-unavailable-record-with-empty-surfaces-still-enumerated-to-consumer --assert-id-state-and-reason-shown-to-user --per-capability-not-per-connection-refusal --other-capabilities-unaffected --assert-authority-rejects-invocation-of-schema-too-old-capability --older-representation-derived-not-hand-maintained --derivation-cached-per-capability-generation-schemaversion --assert-projection-latency-flat-in-supported-window-width --unknown-surface-kind-dropped --unknown-state-fails-closed --defined-field-addition-bumps-schema-version --below-minimum-version-refuses-registration-with-typed-error"}
- {id: 9, tier: integration, check: "scripts/test_capability_advertisement.sh --consumer mcp --consumer acp --consumer cli --re-advertise-effective-set --generic-dispatch-from-descriptor --invalid-args-typed-error-not-forwarded --emit-change-on-effective-set-change --absent-capability-returns-typed-capability-absent --disabled-and-unknown-externally-identical"}
- {id: 10, tier: integration, check: "scripts/test_advertisement_noninterference.sh --two-principals-differing-scope --change-outside-scope-b --assert-no-generation-advance-for-b --assert-no-event-for-b --assert-byte-identical-projection-for-b --assert-scope-decision-uses-only-id-and-authorization-tag --assert-no-allocation-on-out-of-scope-advisory-blob --per-scope-generation-derived-from-projection-content --no-wallclock-timing-assertion"}
- {id: 11, tier: integration, check: "scripts/check_surface_keys.sh --canonical-key-per-kind cli,tool,route,web --unique-across-merged-closure --aliases-share-verb-namespace --core-reserved-names-and-prefix-refused --uniqueness-enforced-at-merged-closure-holder --admission-order control-plane-then-runtime-then-profile-order --startup-sequence register-upstream-then-admit-upstream-then-admit-local-then-serve-clients --no-client-served-before-local-admitted --incumbent-preservation-outranks-admission-order --late-upstream-claim-on-live-local-key-refused --colliding-keys-across-services-fixture --same-incumbent-on-every-start --duplicate-claim-rejects-later-registration-atomically --replacement-requires-incumbent-authorization-under-cert-cn --replacement-refused-for-bare-bearer --replacement-registration-time-only --replacement-emits-audit-record --assert-incumbent-surface-still-advertised-after-conflict --assert-conflict-cannot-withdraw-incumbent --deterministic-registration-order --replacement-only-via-authorized-transactional-op --audit-record-emitted"}
- {id: 12, tier: mechanical, check: "scripts/check_descriptor_content_safety.sh --require-nfc --forbid-c0-c1-and-ansi-escapes --forbid-bidi-and-zero-width --forbid-confusable-script-mixing-in-canonical-keys --enforce-per-scalar-type-bounds string,int,uint,bool,enum,duration_ms,capability_id --default-satisfies-declared-type-bounds --enforce-schema-v1-numeric-limits name_token,bounded_string,capability_id,normalized_path,max_args_per_surface,max_surfaces_per_capability,max_capabilities_per_projection,max_projection_bytes,max_advisory_bytes_per_capability,max_advisory_bytes_per_authority --assert-per-authority-advisory-aggregate-bound --enforcement-site registration-time --assert-overflow-rejects-registration-atomically --assert-running-total-decremented-on-withdrawal --assert-not-deferred-to-projection-time --linearity-by-closed-scalar-arg-model --enforced-at-validator-and-client"}
- {id: 13, tier: mechanical, check: "scripts/check_projection_topology_nondisclosure.sh --client-visible-schema-has-no-provenance-field --path-level-not-schema-only --deny-list-applied-to response,headers,logs,traces,metrics-labels,audit --inject-provenance-fixture-and-assert-absent-from-every-emitted-artifact --client-visible-reason-enum dependency,stale,schema_too_old,policy --forbid-upstream-reason-codes-downward --forbid-service-name-address-or-health-field --assert-upstream-outage-projects-as-dependency --diff-client-projection-vs-internal-closure"}
- {id: 14, tier: mechanical, check: "scripts/check_envelope_deadlines.sh --require-heartbeat-halfopen-reconnect-roundtrip-when-notified --assert-half-open-greater-than-heartbeat --assert-stale-greater-than-revalidate --recompute-hop-bound-and-reject-mismatch --assert-bound-includes-stalled-request-timeout --drop-connection-mid-request-fixture --assert-propagation-bound-equals-hop-plus-upstream-over-actual-topology --assert-stale-at-least-propagation-bound --control-plane-propagation-bound-equals-its-hop-bound --reject-malformed-envelope-fail-closed"}
- {id: 15, tier: integration, check: "scripts/test_capability_absent_indistinguishability.sh --absent-vs-never-existed --assert-identical-status-404 --assert-identical-body-bytes --assert-no-id-echo --assert-no-reason-field --assert-no-varying-content-length --assert-no-capability-specific-header --assert-single-resolution-path-no-withheld-branch --distinguish-from-present-but-unavailable-record"}
- {id: 16, tier: integration, check: "scripts/test_static_thin_client_state.sh --assert-no-projection-generation-capability-integration-or-module-state-persisted --scan-files-databases-keychains-env-and-consumer-output --restart-offline-presents-no-surface --restart-online-refetches-before-presenting --consumer-output-never-read-as-authority --transport-identity-and-endpoint-only-durable-exceptions"}
```

## Review status

**Review in progress; not yet converged.** Drafted 2026-07-23. This proposal has not been through the
suite roundtable and does not inherit the 2026-07-20 approvals; it must complete its own
technical-writing, architecture, adversarial, and verification review before acceptance.

It has been through **six roundtable verdicts** (revisions 1→7), and every genuine finding from each
is resolved — see the revision history below. Revision 8 adds a governing stateless-client
clarification after those verdicts and is not reviewed yet. Review had paused before a clean pass
while the `codex` seat was out of quota; that quota reset on 2026-07-29, so the next step is a
full-strength pass over revision 8. The two rounds before the pause produced malformed-verdict seat
failures and two demonstrably false findings (a "truncated paragraph" that is intact, and a cited
line number the panel gave as 675/702 that is 642/669 in both this branch and `origin/testing`). The
outstanding revision-7 items were non-blocking suggestions and nits; revision 8's new state-ownership
boundary and check 16 must be reviewed as new normative content.

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

Revision history.

*Revision 7 → 8* (governing clarification, not yet roundtable-reviewed): "static" still allowed an
implementation to persist the returned projection and treat the client as a second state owner. The
thin client is now explicitly stateless: it may hold the projection only in process memory while
attached, must refetch before presenting a surface after restart, and may not persist capability,
generation, integration, or module state. Server-projected MCP/CLI integration material written
through a consumer's standard API is consumer-owned rendered output, never a client cache or an
authority the client reads back. Truthfulness invariant 13 and binding check 16 make that boundary
observable across files, databases, keychains, environment rewrites, consumer output, offline
restart, and online refetch.

*Revision 1 → 2* (roundtable run `…_1784821917_31`, 6 blocking + 1 foundational): the draft cited
"invariants 1–4" as the suite dependency law, carried an undifferentiated `depends_on` that would
have suppressed capabilities whose *soft* dependency was unready (contradicting invariant 16), left
change notification optional on network edges, used an authority-wide generation that leaked
cross-scope activity, treated every additive descriptor field as compatible, and asserted a
client-local-execution ban with no decidable property behind it.

*Revision 6 → 7* (roundtable run `…_1784827190_54`, 7 real blocking + suggestions; two of the nine
reported blocking were false and rejected — one seat reported the *Capability state* paragraph
"ends mid-token", which it does not; the text is intact and that seat had truncated its own context.
Another claimed `route_capabilities` is at `server_http.c:675` and `server_http_set_ready_provider`
at `702`; both are wrong — 642 and 669 respectively in this branch *and* in `origin/testing`, the
same 675 a prior round asserted, so the citations stand.) Real fixes: the envelope marked the
notified-mode deadlines `required: true` while the prose said "required iff notified" — reconciled to
a `when_notified` requiredness (present-and-non-zero in notified mode, absent otherwise), so the
validity relations that mention them are unambiguous. The narrative hop arithmetic still summed
`revalidate_after_ms + round_trip_budget_ms` while the YAML had grown a `request_timeout_ms` term, so
prose and schema disagreed on the number a conformance test checks — the narrative now names
`one_revalidation` and matches. `unavailable_reason: policy` had no stated precondition, so it could
have been used for a capability withheld for authorization — disclosing its existence; it is now
permitted only for a capability the caller may *see* but not *use*, with withheld capabilities absent
entirely. The Runtime's startup sequence was never pinned, so the admission order that decides
"incumbent" was not actually guaranteed by anything; the sequence (register upstream → admit upstream
→ admit local → serve clients) is now stated, with the unreachable-upstream path and the rule that
incumbent-preservation outranks admission order. Check 4's bit-identical-binary assertion assumed a
reproducible build the proposal never required; it now specifies the normalization and self-tests
reproducibility before drawing any conclusion about module leakage. And the proposal's own "a flag
name alone is not an acceptance criterion" was contradicted by checks that bundle many assertions
under one exit code; every check script now reports per-flag verdicts, with unimplemented flags
failing rather than passing by omission.

*Revision 5 → 6* (roundtable run `…_1784826546_53`, 5 architect blocking + 5 suggestions; two further
"blocking" entries were seat failures returning malformed verdicts, not findings): `capability_absent`
was named as a typed result in two places but had no wire definition, so "indistinguishable from
never-existed" was unenforceable and an implementer could not tell whether it meant 404, 200+empty,
or something else — now a normative status/body/timing block with its own check 15, explicitly
contrasted with the *visible* present-but-`unavailable` record. The per-authority advisory cap named
no enforcement site; registration-time summation is the only site that closes the amplification
(projection-time fails after the memory is spent, a sweep races admission), so the site, the running
total's lifecycle, and the overflow contract are now part of the schema. The bounded-revalidation hop
bound still had no term for a request that *stalls* — one dropped GET made the hop unbounded
regardless of the interval — fixed with a derived `request_timeout_ms`. Topology non-disclosure was
enforced on the schema alone, which cannot bound what the code emits: a log line, header, trace span,
or audit record could carry provenance out of band, the same shape of mistake revision 3 made, so it
is now a path-level property with a deny-list over every emitted artifact. Defined-field renames are
now explicitly rejected without a version bump — rule 4 already covered this, since a rename is a
removal plus an addition, but the failure is silent and the mistake natural. Adopted suggestions:
per-scalar-type bounds folded into the one validator pass; the work-proportionality claim formalized
(the scope decision may use only `id` and authorization tag, before any expansion); transactional
replacement authorization specified (incumbent's own authorization, `cert:CN` only, registration-time
only, audited); derivation cost model stated (cached per capability/generation/schema_version, so
window width costs memory not latency); and the bounds block split into computation, validity
relations, and propagation rule so each is independently testable.

*Revision 4 → 5* (roundtable run `…_1784825942_51`, 3 blocking + 6 suggestions + 2 nits): an
`unavailable` capability has its surfaces omitted, and a dispatcher enumerating *surfaces* would have
rendered nothing at all — silently losing the capability the client was supposed to be told about;
the effective set is now enumerated from capability records, so an unusable capability is visibly
unusable rather than absent. The validity relations referenced a `propagation_bound_ms` that was
never a field, so the relation could not be enforced; it and `hop_bound_ms` are now stated envelope
fields the validator recomputes, summed along the actual chain rather than read from one constant.
The hop bounds counted only intervals, not the round trip that delivers the change, so a conformance
test could certify a bound no real network meets; `round_trip_budget_ms` is now a term in every hop
bound and the hop advertises the worse of its healthy and failed paths. Adopted with them: generation
advances only on *semantic* descriptor changes, so a reworded help string no longer storms every
client; canonical-key admission order is specified (Control-Plane, then Runtime, then profile order)
so "incumbent" is not decided by a race; `advisory_ext` gains a per-authority aggregate bound against
server-side amplification; `unavailable_reason` gains a state-validity relation; and multi-Runtime
topologies are explicitly out of scope, since provenance would travel sideways in a mesh. Cited line
numbers were re-verified against the branch and four were wrong (`server_http.c` 653→642, 667→669;
`cli_mcp_serve.c` 308→315; `cli_remote.c` 306→307, 304→321) — note the review's own proposed
replacement for the first was also wrong, so each was checked against the symbol rather than adopted.

*Revision 3 → 4* (roundtable run `…_1784823160_37`, 4 blocking): revision 3's own outage fix
introduced a `provenance` field into the **client-visible** projection, disclosing both that a
Control Plane exists and which capabilities came from it — a direct contradiction of invariant 6 and
of the governing request. Provenance is now authority-internal, the client-visible reason enum is
closed against topological codes, and *Topology is not projected downward* states the general rule
(invariant 12, check 13). The advisory/critical distinction was unrepresentable as specified —
criticality sat on a whole surface while unknown keys were rejected outright, so an old client could
not identify an unknown field as advisory at all; replaced with a *positional* rule where every
defined field is critical by construction and a single `advisory_ext` map is the one place unknown
keys are legal. Notified mode's deadlines were required but never transmitted, and the failure-path
bound omitted the post-fallback revalidation delay, so a conformance test could certify a bound the
behavior cannot meet; the deadlines are now envelope fields with stated validity relations
(check 14) and the failure bound includes that delay. Invariant 10 still said a collision advertises
neither claimant, contradicting the incumbent-preserving rule it was supposed to encode and
reinstating the denial-of-service primitive on a literal reading — corrected.

*Revision 2 → 3* (roundtable run `…_1784822498_34`, 6 blocking): the "closed schema" was named but
never written down, leaving checks 3 and 12 undecidable — now a normative wire-schema block with a
closed scalar argument model (chosen so validation linearity is structural rather than analyzed) and
concrete numeric limits. The collision rule advertised *neither* claimant, which handed any admitted
module a denial-of-service primitive against an incumbent's verb or tool name — now
incumbent-preserving, with replacement only through an authorized transactional operation. Critical
fields contradicted schema down-negotiation (serving the older representation erased the constraint)
— now a per-capability negotiation algorithm where such a capability is `unavailable` with
`schema_too_old` and the authority refuses its invocation independently of the client. Control Plane
loss was unbounded in blast radius — now scoped by `provenance` plus hard-dependency closure, with
independent Runtime-local capabilities explicitly retained. Notified mode had no liveness deadline,
so a silently broken stream had no bound — now heartbeat, half-open detection, reconnect, and
mandatory fallback to revalidation. The noninterference and no-Control-Plane-contact checks were not
reproducible — the memory-scan and wall-clock-timing assertions are withdrawn as unenforceable and
replaced with deterministic ones (network capture with a defined lifecycle, configuration/env/argv
scanning, byte-identical projections, and a structural work-proportionality check); residual
microarchitectural timing is now documented as out of scope rather than silently claimed.
