# Getting caller context to the KB, so content scope can be switched on

Companion to `per-user-content-scope-visibility.md`. That proposal built the database half; this one
is about the request-context half that has to exist before any of it can be enabled.

## Problem

`kb_content_scope_enable()` refuses today, and the refusal is correct: **no content read path sets
`aimee.principal`.** It is set only inside `db2_tenant_scope_begin`; `kb_payload.c`, `memory_query.c`
and `kb_service_backend.c` do not open that scope. Enabling before this is fixed returns nothing to
every content read.

This is an authorization-context gap, not a missing authentication system.

## The actual service boundary

CLI/thinclient, web and MCP all terminate at aimee-server. They do not independently connect to
aimee-kb, so the KB does not need a separate proof protocol for each surface.

Every networked Aimee-to-Aimee hop uses the standard triple layer. A remote CLI/thinclient therefore
connects to aimee-server with full mTLS, rotating token bearing and PAM/OIDC identity; the subsequent
aimee-server-to-aimee-kb hop independently applies the same rule. Certificate material is unique per
peer relationship and role, and rotates independently: aimee-server's client certificate toward
aimee-kb is different from its thinclient-facing server certificate and cannot be used in that role.
The local UDS path is the OS-authenticated host-local boundary, not a bearer-only network fallback.

The aimee-server-to-aimee-kb connection is required to be:

- mutually TLS-verified with unique per-pair, role-constrained, independently rotated certificates;
- token-bearing with the existing rotation/expiry/revocation lifecycle; and
- identified as aimee-server through federated OIDC when OIDC is configured, otherwise through a PAM
  identity that exists on aimee-kb.

All three layers must succeed. A missing layer fails closed; there is no bearer-only, anonymous or
caller-selected fallback for content calls.

## What needs to cross the boundary

The service principal proves which aimee-server made the request. The originating user, when there is
one, is caller context carried on that authenticated request:

- OIDC keeps the existing verifiable identity token path;
- web/PAM and local UDS callers use the principal aimee-server already authenticated;
- MCP inherits the authenticated `/v1` context rather than creating a new KB identity mode;

aimee-kb remains authoritative for team/project membership. It resolves the supplied caller context
against its own records and intersects it with the service principal's allowed scope. A caller or
server may name a principal or project context; it may not manufacture membership.

## Decision

**Propagate caller context over the existing authenticated service channel; add no new authentication
layer.** In particular:

- do not enroll a special service credential allowed to assert host subjects;
- do not require a new token-mint round trip for a local identity already authenticated by
  aimee-server;
- do not treat CLI, web or MCP as direct KB callers; and
- do not exempt local or background work from content scope merely because it has no independent
  end-user token.

The security boundary is already defense in depth: mTLS, a rotating bearer, and PAM/OIDC identity.
Each enrolled endpoint independently authenticates the other, so an Aimee-specific compromise of the
communication presupposes that both systems are already compromised. Minting a fourth artefact from
the same authenticated context adds no useful protection in that state. Physical compromise
sufficient to impersonate a local UDS caller is likewise outside the local-CLI threat model.

## Non-goals

- Replacing standard CLI, web or MCP authentication.
- Adding a principal kind or a second identity store.
- Letting aimee-server determine team/project membership.
- Post-compromise protection after both enrolled Aimee endpoints are already compromised.
- Per-user memory. `memories` remains global here; per-user memory is DB1's concern.

## Background and maintenance work

Ingest, re-embed, curator passes and the code indexer act on nobody's behalf. Their authorization
remains the open question from #2646. The options remain a named maintenance scope, per-project
iteration, or leaving job/queue tables out of content scope with a stated reason.

That decision must land before content scope is enabled, but it is separate from the rejected demand
for another CLI, web or MCP proof mechanism.

## Bounded slices

Slices 1–5 land while content RLS remains disabled, so the context wiring does not change query
results. Slice 6 declares reader readiness; content scoping becomes observable only after the
operator enables it.

1. Assert the existing three-layer connection on every network hop used by content routes, including
   thinclient-to-server, server-to-KB, pairwise certificate uniqueness, independent rotation,
   revocation and pooled connections.
2. Carry aimee-server's existing caller context on content calls without adding a credential type or
   token exchange.
3. Resolve the service principal and optional caller together through the existing identity/team
   resolver; reject conflicts and ungranted project selection.
4. Open and clear the content tenant scope from the resolved request context.
5. Cover web, local CLI, remote CLI/thinclient, and MCP end to end, and land the separately reviewed
   #2646 background-work decision.
6. Set `kb_meta.content_scope_reader_ready = '1'`, then allow an operator to call
   `kb_content_scope_enable()`.

## Acceptance checks

- **Connection.** Every network hop used by a content request has verified mTLS, a current bearer and
  a resolved OIDC-or-PAM identity; failure of any layer returns no content.
- **Pairwise mTLS.** Thinclient-to-server and server-to-KB use different role-constrained certificate
  material, rotate independently and reject cross-peer or cross-role reuse.
- **Ingress convergence.** Web, local CLI, remote CLI/thinclient, and MCP reach the same server-to-KB
  path, and each is tested by name to prove its caller context reaches the KB read.
- **Integration.** Two users, two teams, RLS enabled in a scratch database: each sees only their own
  project's documents through the ordinary search path.
- **Service work.** Background jobs follow the separately selected #2646 policy; this proposal does
  not preselect a maintenance identity or bypass.
- **Negative.** Caller context cannot widen the service principal's KB-resolved membership, and a
  request with no context cannot inherit the previous pooled request's principal.

## Status

Pending, and blocking `kb_content_scope_enable()` by construction: the marker it checks is set by
slice 6 above. The companion identity map records why no additional proof mechanism is required.
