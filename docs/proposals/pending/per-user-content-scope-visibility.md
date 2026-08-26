# Per-user content scope: a project you cannot see returns nothing

- **State:** PENDING. Restored 2026-08-16 after rejection #2678 applied the wrong Go-only
  lifecycle rule; the already-landed SQL controls remain operator-gated compatibility enforcement,
  and the unresolved authorization owner is rewritten below as Go work.
- **Scope:** KB tenancy and read visibility across database content, code index, workspace, and
  memory.
- **Decision review:** Aimee roundtable `roundtable-261796393f7c902f80808996` approved the
  corrective intent, ownership boundary, memory default, and migration authority with no findings.

## Correction and lifecycle

Old PR #2678 rejected this proposal because its unresolved slices named PostgreSQL schema/RLS and
the C workspace implementation. That was the wrong lifecycle decision. A live security objective
does not become invalid when its stale implementation plan names C or SQL: canonical policy must be
rewritten under the appropriate Go module, while C, SQL, and PostgreSQL remain mechanical adapters,
migrations, containment checks, and fail-closed enforcement.

This correction restores the proposal to pending. It does not claim the cross-tenant read hole is
fully fixed. Slices already present in `src/modules/db2/c/schema.sql` remain useful compatibility
controls, but their activation is still operator-gated and they are not the future authorization
owner. The proposal moves to `done/` only after the Go implementation and all negative acceptance
evidence below land.

## Problem and boundary

A user who is not a member of a project can still reach content attributed to that project through
surfaces that do not consistently consume one authorization decision.

The tenancy model already has teams, projects, memberships, access modes, and exact project
referents. The delivered reader-context companion supplies authenticated caller context and live
two-user/two-team RLS coverage. The partial SQL content-scope work adds `projects.kb_project`,
`kb_project_visible()`, policies for document and vector families, and an operator activation seam.
Those are valuable controls, but SQL currently derives membership itself and therefore acts as a
second policy owner. Code-index rows, filesystem access, and memory also do not yet consume one
canonical content-visibility result.

This proposal covers reads: get, list, search, index lookup, workspace open/list, and recall. It
does not redesign write authorization, authentication, project access modes, or the meaning of an
existing membership.

## Authorization ownership

The required Go `execution-policy` module is the sole canonical owner of content-visibility
authorization. It evaluates a closed request using authenticated actor identity, exact tenancy and
code-project referents, workspace identity, action, resource class, and resource identity. It emits
either a typed denial or a versioned `ContentVisibilityDecision`.

The decision is immutable and request-bound, not a transferable bearer grant. Its bounded contract
contains at least:

- policy and schema versions;
- authenticated actor and issuing service principal;
- exact tenancy `kb_project.id`, exact code-index `projects.id`, and workspace identity;
- closed action and resource-class enums plus the exact resource referent when one exists;
- request and input digest, correlation identity, issuance time, expiry, and decision identifier;
- allow/deny result and a stable redacted reason suitable for audit.

The event-bus principal and correlation must match the decision. Reuse by another actor, request,
project, action, resource, or expired operation denies. A missing, malformed, unverifiable, or
ambiguous decision denies.

Only Go `execution-policy` decides whether the actor may see the content. Consumers and adapters
may narrow an allow, for example, path containment can still reject an otherwise authorized
workspace request, but no downstream component may turn a Go denial into an allow.

This typed decision is a new architecture choice needed to prevent the Go rewrite from creating
parallel policy owners. It was approved as part of the corrective intent; its concrete event kind,
stage ID, size bound, and codec remain implementation details to freeze through roundtable review
before the first executable slice.

## Resource binding and enforcement

### Go workspace

Go `workspace` constructs the canonical bounded resource context from authenticated request facts.
It validates runner identity, workspace containment, exact code-project identity, and the mapping to
the exact tenancy project. It does not infer authorization from a path or caller-supplied project
name. It sends that context to `execution-policy`, then applies the returned decision before list,
open, search, or runner I/O.

Legacy C workspace code remains a containment and I/O adapter during migration. It may reject
unsafe paths, but it may not independently grant visibility or substitute name-based membership.

### Go DB2 and PostgreSQL

Go `db2` consumes the same decision over its typed module contract and binds its exact actor,
project, action, resource class, decision ID, and request digest transaction-locally. Only the
admitted DB2 module holds the database credential and setter capable of establishing that scope;
generic application roles and direct callers cannot manufacture it. Scope is cleared before a
pooled connection is reused.

PostgreSQL roles and RLS mechanically enforce the exact transaction-local bounds. Missing scope,
unattributed rows, a project mismatch, a resource-class mismatch, or an invalid decision returns no
rows. RLS may narrow the Go decision but cannot broaden it.

The current `kb_project_visible()` SQL membership calculation is a temporary compatibility
constraint. During cutover it may only further restrict an allowed Go decision. After parity is
proved, SQL stops independently deriving authorization and checks the exact decision-bound project
and resource context instead.

Project names are not tenancy referents. `projects.name` is globally unique in the code index,
while `kb_project.name` is unique only under its parent team. Content therefore binds through exact
IDs: `projects.id` maps to exactly one `projects.kb_project`, and unattributed or ambiguous mappings
deny. The `files` code-index table gains RLS through that exact project relation. Document,
file-index, vector, PDF, structured-document, region, cell, and asset paths retain or gain the same
exact attribution rule.

## Memory scope decision

The old proposal left memory ownership open between project, team, and identity. This rewrite
resolves it: learned and user-derived memory is project-scoped by default. That matches the existing
current-project-first recall model and gives the same isolation rule as documents, indexes, and
workspace content.

Shared or global memory is a distinct, explicit class requiring separate Go authorization. Absence
of a memory scope is not evidence of sharing: legacy untagged memory is invisible to ordinary
scoped recall until it is attributed. The compatibility `include_all` path is available only to a
separately authorized migration or audit operation and never to an ordinary read or recall.

This is a new product decision that closes the old proposal's unresolved question. The roundtable
approved the default in `roundtable-261796393f7c902f80808996`; any later change to team- or
identity-default semantics requires a separately reviewed proposal rather than an implementation
shortcut.

## Migration authority

Use backfill-then-enable. Existing rows are attributed to exact projects, preflight counts prove no
unexpected unattributed or ambiguous content remains, and only then are the policies activated.
There is no compatibility window in which unattributed ordinary content becomes visible.

Activation is a Go-authorized DB2 maintenance operation, not a direct generic-role SQL call. Its
authorization binds the operator actor, schema/policy version, exact attribution and preflight
digest, request identity, decision identity, and expiry. Go `execution-policy` authorizes the
operation; Go `db2` verifies the bound decision and applies the migration atomically. A stale
preflight, changed attribution set, mismatched actor/request, expired decision, partial migration,
or direct call denies and leaves the prior activation state intact.

The existing `kb_content_scope_enable()` / disable seam remains a private compatibility mechanism
until Go DB2 owns the operation. Generic application roles cannot execute it. SQL checks the bound
preconditions mechanically and never decides that a broader visibility policy is acceptable.

This Go-owned activation contract is a new architecture decision required by the Go-only policy
boundary. Its exact operation payload and rollback evidence must be frozen through roundtable
review before implementation.

## Implementation slices

1. **Typed Go policy contract.** Add the bounded content resource input and
   `ContentVisibilityDecision` to Go `execution-policy`; generate typed bus codecs and rejection
   reasons; prove missing, malformed, replayed, cross-actor, cross-request, and expired decisions
   deny.
2. **Go workspace producer and consumer.** Resolve exact actor/workspace/code-project/tenancy-project
   facts, request a decision, and require it for workspace list/open/search and runner I/O. Retain C
   containment as a narrowing adapter until the Go path replaces it.
3. **Go DB2 tenancy consumption.** Add decision-bound transaction scope to the stable DB2 module
   contract, restrict database credentials/setters to the admitted module, clear scope on pool
   return, and prove direct/generic callers cannot establish it.
4. **Exact database enforcement.** Convert document, file-index, vector, PDF, structured-document,
   descendant, and code-index policies to exact decision-bound enforcement. Add `files` coverage
   through `projects.kb_project`. During parity, old SQL membership logic may narrow only; remove it
   as a policy owner after equivalent denial evidence passes.
5. **Memory attribution and recall.** Attribute new learned/user-derived memories to the active
   project, migrate legacy rows explicitly, require separate authority for shared/global memory,
   and close ordinary `include_all` access.
6. **Go-authorized activation.** Implement versioned preflight, attribution digest, atomic
   enable/rollback, audit evidence, and generic-role denial through Go DB2 maintenance.
7. **End-to-end promotion.** Exercise every surface with two actors and two projects, freeze the
   negative evidence, remove remaining policy decisions from compatibility C/SQL, then update this
   proposal to done.

The slices intentionally distinguish future Go policy and orchestration work from legacy adapter,
migration, and negative-parity work. Existing C/SQL behavior is a compatibility baseline, not
evidence that this pending proposal is implemented.

## Acceptance evidence

### Mechanical and unit

- The content decision schema is bounded and versioned; unknown versions, enums, oversized fields,
  missing exact IDs, or mismatched digests deny.
- Go policy tests cover team-open and restricted projects without duplicating the rule in a
  consumer.
- Workspace and DB2 cannot proceed without a matching allow decision and cannot reuse one across
  actor, project, workspace, action, resource, request, or expiry boundaries.
- No C or SQL function can independently convert a denied or absent decision to visible content.

### Database and migration

- An unauthorized actor reads zero rows from each document, file-index, embedding, PDF,
  structured-document, descendant, asset, and code-index table; tests assert each table rather than
  an aggregate.
- Missing identity, missing scope, unattributed content, ambiguous mappings, and mismatched exact
  project IDs return nothing.
- Generic roles cannot establish decision scope or call activation. Stale/mismatched preflight,
  attribution digest, schema version, actor, request, or expiry fails atomically.
- Removing the policy makes the negative test fail, proving the test actually traverses the
  protected surface.

### Workspace and memory

- For two actors and two projects, workspace list/open/search and runner I/O expose only the
  authorized exact project; a path under the shared environment root does not grant visibility.
- Ordinary recall excludes another project's memory and all legacy untagged memory.
- Shared/global memory and `include_all` deny without their separate authority and succeed only for
  the explicitly authorized class or migration/audit operation.

### Integration and promotion

- CLI/thinclient, web, MCP, background work, search, code-index lookup, workspace operations, and
  recall all consume the same Go-owned decision path.
- A Go denial remains a denial at every downstream enforcement point, and each denial emits a
  stable redacted audit reason without leaking hidden project or resource metadata.
- Roundtable approves the frozen implementation diff and evidence. Only then may the lifecycle
  state change from pending to done.

## Open implementation details

The event kind/stage allocation, exact wire size limits, database transaction-local field names,
and migration rollback encoding are intentionally not decided in this lifecycle correction. They
must be resolved against the implementation-time catalogs and reviewed by the roundtable. They may
not weaken exact binding, fail-closed behavior, sole Go authorization ownership, or the negative
acceptance contract above.
