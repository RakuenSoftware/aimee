# Getting caller context to the KB, so content scope can be switched on

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE — all six bounded reader-context slices delivered and archived 2026-08-15.
- **Owner:** KB request identity and content scope.

> **Archived after delivery.** This record covers the authenticated request-context half only. The
> broader table, filesystem, and memory proposal remains pending under Go authorization ownership
> in [`per-user-content-scope-visibility.md`](../pending/per-user-content-scope-visibility.md).

Companion to [`per-user-content-scope-visibility.md`](../pending/per-user-content-scope-visibility.md).
That proposal tracks the broader database half; this one
is about the request-context half that has to exist before any of it can be enabled.

## Problem

Authenticated content ingress opens `db2_tenant_scope_begin`, background content work has a separate
named project scope, and live RLS coverage proves ordinary searches for two users on two teams. The
release therefore records reader readiness. `kb_content_scope_enable()` still refuses until the
operator attributes every content-bearing code project to its exact tenancy `kb_project`; enabling
before that backfill is complete would make existing content disappear.

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

This is the adopted resolution of #2646's background-work question.

Ingest, re-embed, curator passes and the code indexer act on nobody's behalf. Their authorization
is a named, project-bound maintenance scope. Queue tables remain outside content RLS so a worker can
claim a durable job and learn its project without seeing content. The worker then opens a
transaction-local maintenance context for its own closed-name role (`ingest`, `reembed`, `curator`,
or `code-indexer`) and the exact attributed project from that job. Content policies admit only rows
for that project. The context is cleared before the pooled connection is returned and is never held
across an embedder, sidecar, or other external call.

This is an in-process database scope, not a fourth connection credential and not a synthetic user.
It does not change the rejected demand for another CLI, web or MCP proof mechanism. Durable queue
rows retain the project, claim owner, and lifecycle; the closed worker name is recorded by the
worker boundary and logs as the audit trail for why maintenance ran.

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
5. Cover web, local CLI, remote CLI/thinclient, and MCP end to end, and wire the named project-bound
   maintenance scope selected for #2646.
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
- **Service work.** Ingest, re-embed, curator, and code-index jobs use the closed worker names above,
  open content transactions for only the exact attributed project, and hold no such transaction
  across an embedder, model, or sidecar call.
- **Negative.** Caller context cannot widen the service principal's KB-resolved membership, and a
  request with no context cannot inherit the previous pooled request's principal.

## Delivery evidence

Implemented cumulatively through slice 6. [#2656](https://github.com/RakuenSoftware/aimee/pull/2656)
supplied pair-specific mTLS enforcement, UDS/web/thinclient/MCP identity convergence, and
caller-scoped reads; [#2658](https://github.com/RakuenSoftware/aimee/pull/2658) and
[#2661](https://github.com/RakuenSoftware/aimee/pull/2661) supplied exact-project background
maintenance; [#2670](https://github.com/RakuenSoftware/aimee/pull/2670) declared reader readiness
with live two-user/two-team ordinary-search RLS coverage.
Schema application records `content_scope_reader_ready = '1'` but does not enable content RLS. The
operator must finish attribution and call `kb_content_scope_enable()`; incomplete attribution still
fails closed. The [archived companion identity map](per-user-content-scope-identity-map.md)
records why no additional proof mechanism is required.

- [`kb_content_scope_enable`](../../../src/modules/db2/c/schema.sql) refuses activation until the reader
  readiness marker is present and all content-bearing projects are attributed.
- [`db2_maintenance_scope_begin`](../../../src/modules/db2/c/db2_tenant.c) opens the closed-name,
  exact-project maintenance context on a transaction-local lease.
- [`test_content_scope_pg.c`](../../../src/tests/test_content_scope_pg.c) verifies readiness, the
  unattributed-row refusal, ordinary two-user search isolation, and that a caller-less pooled search
  inherits no prior identity.
