# Proposal: immutable Go capability scope for delegate disclosure and execution

- **State:** PENDING. Restored 2026-08-16 after rejection #2686 discarded a live invariant
  together with its obsolete C placement.
- **Date:** 2026-07-21; Go ownership rewrite 2026-08-16.
- **Charter roles:** Enforce / Constrain-Verify / Gate-Promote.
- **Decision review:** Aimee roundtable `roundtable-63c3bdcf141f50ede72fa2a7` approved the
  corrective intent and the unified-set, fail-closed unenforced-permission, and frozen provider
  mapping decisions with no findings.

## Correction and lifecycle

Old PR #2686 correctly observed that its prescribed `_Thread_local` state in
`src/server/agent_tools.c` and binding in `src/server/server_compute.c` belonged to a superseded
delegate execution path. It incorrectly rejected the safety objective along with that placement.
The obsolete C snapshot is dropped; the still-live resolve-once disclosure/dispatch parity
invariant is restored under the current Go owners.

This correction returns the proposal to pending. It does not claim that all delegate providers and
dispatchers already consume a Go-owned capability set. Existing C resolution and filtering remain
compatibility behavior and negative-parity fixtures until the implementation slices below replace
their policy decisions.

## Problem and bounded objective

A delegate's advertised tools and executable tools can be derived at different times from mutable
role, toolset, registry, or adapter state. Re-resolution permits drift: a tool absent from the
provider request can become executable later, a role/template change can widen a live run, an empty
resolution can fall through to defaults, or a provider alias can resolve to a different canonical
tool at dispatch.

The objective is narrow and load-bearing:

> For one delegate invocation, disclosure and dispatch consume the same immutable effective
> capability set. Every later authority may narrow it; nothing may widen it.

This proposal does not add a permission vocabulary, policy language, tool registry, persistence
model, approval system, audit vocabulary, CLI family, or provider. It composes the Go seams that now
own the old invariant.

## Current Go baseline and ownership

At the reviewed baseline, `server-go/modules/delegates` already owns the pieces from which the
effective set is built:

- `permissions.go` resolves role permissions once, including exact scopes and enforcement points;
- `permissions_stage.go` returns those grants, unenforced names, and `DeniedTools` in one bounded
  reply;
- `toolpermissions.go` owns the permission-to-tool clamp;
- `AutoToolsForInvocation` consumes the caller's already-resolved `tools` permission rather than
  re-reading the role; and
- `RegistryExecutor` owns Go delegate selection and subprocess lifetime.

Go `delegates` owns capability-set construction and its invocation lifetime. The required Go
`execution-policy` boundary remains the final per-action authorization owner. Workspace/mount, API,
vault, egress, sandbox, governance, and approval enforcement retain their own narrowing authority.
Provider adapters translate names and transport the frozen set; they do not decide capabilities.

## One immutable `EffectiveCapabilitySet`

Go `delegates` resolves exactly one bounded set before provider request construction or execution.
The set is the single resolved object for the invocation and contains:

- schema and resolver revision;
- invocation, authenticated principal, parent delegation, workspace, and correlation identities;
- canonical role, role-definition digest, explicit toolset selection, tool-catalog revision, and a
  digest of every resolution input;
- the ordered, duplicate-free canonical tool names;
- resolved permission grants, exact scopes, and enforcement points;
- denied-tool and unenforced-permission lists;
- provider kind plus the frozen provider-adapter revision and name mapping;
- issuance/lifetime bounds and a digest over the complete set.

The set is immutable and non-transferable. A consumer rejects a different principal, invocation,
workspace, catalog revision, provider revision, correlation, input digest, or lifetime. The set is
run-local evidence, not a reusable bearer token and not durable configuration.

Putting tool names and permission metadata in one object is deliberate. Mount, API, disclosure,
and dispatch receive projections of the same resolution instead of asking the role question again.
This does not merge their enforcement points: a mount still enforces repository write scope, an API
still enforces knowledge mutation, the tool surface still enforces tool/shell membership, and
`execution-policy` still authorizes the proposed effect.

## Resolution rules

Resolution is one deterministic intersection, never a union of independent answers:

1. Bind authenticated invocation, principal, workspace, canonical role, and the exact optional role
   definition bytes.
2. Resolve the role permissions once. An absent definition may use the built-in role; a present
   empty definition is a deliberately powerless role. Unknown roles, unreadable or malformed
   definitions, duplicated grants, or unsupported scopes fail closed.
3. Refuse launch if any held permission has no enforcement point. Merely reporting an unenforced
   control would allow a role definition to promise a boundary that no choke point applies.
4. Apply the existing tools-on/off and turn conditions to the resolved `tools` permission.
5. Resolve the selected toolset once against an exact tool-catalog revision. An explicit override
   selects a toolset but cannot exceed the permission ceiling.
6. Remove every tool denied by the resolved permissions. Preserve the canonical tool ordering
   chosen by the resolver and freeze the result.
7. Freeze the provider mapping and complete set digest before constructing any provider request.

Any resolution error produces `bound_empty` plus a typed denial; it never falls back to a role,
default toolset, environment channel, or prior pooled-worker state.

The invocation states are explicit:

- `unbound`: named non-delegate compatibility callers only. A delegate invocation may not reach
  provider request construction or dispatch in this state.
- `bound_empty`: a valid or failed resolution with no executable tools. Disclosure emits none and
  dispatch denies every tool name.
- `bound_list`: the frozen ordered canonical list and its complete bound metadata.

Binding overwrites the complete prior state before resolution begins. Cleanup clears the complete
state. A shorter second invocation cannot retain a prior tail, and concurrent invocations cannot
observe one another's set.

## Disclosure and provider mapping

Disclosure projects the ordered canonical names through the provider mapping frozen in the set.
The mapping is versioned and injective for the exposed surface:

- a canonical tool unsupported by the provider may be omitted, which narrows the surface;
- two canonical tools may not map to the same provider name;
- an unknown, ambiguous, or dynamically introduced alias denies;
- reverse mapping at dispatch yields exactly one canonical member of the same set; and
- a changed adapter revision requires a new invocation resolution rather than mutating the live
  set.

Filtering preserves the relative order of surviving provider definitions. It does not reorder the
provider array to match an independently resolved list. Provider-specific fields and wire spelling
remain adapter concerns, but the adapter cannot invent or widen a capability.

## Dispatch and final authorization

Dispatch accepts only an exact canonical member recovered through the frozen provider mapping and
the same `EffectiveCapabilitySet` used for disclosure. It does not re-read a role template, choose a
toolset, query a live catalog, consult an alias registry, or fall through from `bound_empty`.

Membership is a ceiling, not an action allow verdict. After exact membership succeeds, dispatch
sends the typed action, arguments, principal, workspace, invocation, capability-set digest, and
relevant permission scope to Go `execution-policy`. Policy may deny or require another existing
authorization step based on the action and its target. A policy denial remains a denial through
tools, workspace, API, vault, provider, and legacy adapters.

The separation prevents both failure modes: `delegates` cannot authorize a dangerous argument just
because its tool name was disclosed, and `execution-policy` cannot make an undisclosed tool appear
in the invocation's executable surface.

## Compatibility migration

Current C delegate paths already consume Go-resolved permission data but still resolve toolsets
independently during disclosure and execution. During migration they consume a serialized projection
of the Go-produced set and become mechanical adapters:

- C may filter a provider array and perform an exact membership check against supplied canonical
  names;
- C may apply mount/API/tool denials already present in the set;
- C may reject malformed, missing, stale, or mismatched set data; and
- C may not re-resolve roles, definitions, permissions, toolsets, catalog membership, or aliases for
  a delegate invocation.

Provider subprocess flags such as an allowed-tools list are defense in depth, not the sole dispatch
boundary. The authoritative Go membership check and final `execution-policy` verdict remain
mandatory even when a provider offers its own restriction flag.

Non-delegate compatibility callers that still require `unbound` behavior are explicitly inventoried
and excluded from completion claims. Each must either migrate to a named capability-set producer or
remain a separately justified non-delegate path; `unbound` is never a delegate fallback.

## Implementation slices

1. **Contract and resolver.** Define the bounded Go `EffectiveCapabilitySet`, canonical encoding,
   complete input/set digests, explicit states, overwrite/clear lifetime, and typed denial reasons.
2. **Permission composition.** Fold the existing one-time permissions reply and denied-tool clamp
   into the set; refuse unenforced grants; prove explicit definitions replace rather than augment
   built-ins.
3. **Catalog and provider mapping.** Bind an exact catalog and adapter revision, freeze the injective
   mapping, and prove omissions only narrow.
4. **Disclosure.** Make every Go-owned delegate provider construct its advertised tool surface from
   the set projection, preserving provider survivor order.
5. **Dispatch.** Require the same set digest and exact reverse-mapped canonical membership, then
   invoke Go `execution-policy` for the concrete action and target.
6. **Compatibility adapters.** Pass the set to retained C/provider paths, remove their delegate role
   and toolset re-resolution, and retain them as negative-parity fixtures until native Go paths
   replace them.
7. **Promotion.** Inventory all delegate and non-delegate callers, run the paired parity and mutation
   suite, remove delegate `unbound` edges, and only then move this proposal to done.

## Security invariants

1. The canonical tools disclosed and accepted for one invocation come from one immutable set.
2. `bound_empty` is distinguishable from `unbound` and has no fallback edge.
3. Permission, toolset, catalog, provider mapping, role, or environment changes after binding cannot
   widen a live invocation.
4. An explicit override cannot grant a permission or tool excluded by the resolved role definition.
5. Provider translation may omit but never invent, merge ambiguously, or widen canonical tools.
6. Final action authorization belongs to `execution-policy`; every other enforcement point may only
   narrow its verdict and the capability ceiling.
7. No canonical capability decision is owned by C.

## Acceptance evidence

### Resolver and lifetime

- A role definition is resolved once. Mutating its file, the selected toolset, environment,
  permission table, or tool catalog after binding does not change the invocation.
- Unknown role, unreadable/malformed definition, duplicate grant, invalid scope, missing catalog
  revision, resolution error, or unenforced permission refuses launch with a stable denial reason.
- `bound_empty` discloses zero and denies every dispatch without consulting a fallback. Binding a
  full set then a shorter or empty set leaves no prior name reachable; cleanup is idempotent.
- Cross-principal, cross-workspace, cross-invocation, cross-provider, stale-revision, expired, and
  digest-mismatched sets deny.

### Paired disclosure and dispatch

- For `bound_list`, interleaved disclosure and dispatch over one binding yield the identical
  canonical allowed set. Every filtered name denies.
- Reversed and duplicate provider input arrays preserve survivor order without creating duplicate
  executable names.
- Unsupported provider tools are omitted at both surfaces. Unknown, ambiguous, duplicate, or
  post-bind aliases deny; a changed adapter revision cannot affect the live invocation.
- An explicit toolset override that contains a permission-denied tool cannot disclose or execute it.

### Enforcement composition

- A capability-set member with disallowed arguments or target is denied by `execution-policy`, and
  that denial remains a denial through all adapters.
- Repository-write scopes are enforced at the exact mount/workspace; knowledge-write scopes at the
  exact API; shell/tool permissions at the tool surface. Holding a shell cannot bypass mount or API
  enforcement.
- Provider flags are removed in a negative fixture and the authoritative dispatch denial still
  holds, proving provider configuration is not the only boundary.

### Integration and promotion

- CLI/API delegation, workflow, roundtable, gateway, and background delegate journeys either bind a
  set before provider construction or deny. No delegate journey uses `unbound` fallback.
- Go and retained C adapters produce byte/semantic parity for the frozen fixtures and the same
  negative results under role/template/catalog/adapter mutation.
- Roundtable approves the frozen implementation and evidence diff. Only then may the state change
  from pending to done.

## Non-goals and open implementation details

This proposal does not define new permissions, side-effect classes, approval or governance policy,
audit events, persistent capability records, workflow/persona fields, execution limits, retry
semantics, or UI. It does not replace sandbox, workspace, mount, API, vault, transport, cost,
liveness, or output controls.

Exact event kind/stage allocation, maximum tool count/name size, canonical digest encoding,
capability lifetime representation, and provider mapping table layout are implementation details to
freeze against the live catalogs through roundtable review. They may not weaken the ownership,
immutability, fail-closed, or parity invariants above.
