# Getting the caller's identity to the KB, so content scope can be switched on

Companion to `per-user-content-scope-visibility.md`. That proposal built the database half; this one
is about the half that has to exist before any of it can be enabled.

## Problem

`kb_content_scope_enable()` refuses today, and the refusal is correct: **no content read path sets
`aimee.principal`.** It is set only inside `db2_tenant_scope_begin`, and all 68 callers of that are
governance, management or vault paths. `kb_payload.c`, `memory_query.c` and `kb_service_backend.c`
call it zero times between them. The service connects as a non-owner with `NOBYPASSRLS`, by design,
so nothing rescues it. Enabling before this is fixed returns nothing to every content read, for
everyone.

## Why it is not simply "thread the principal through"

The principal is not in scope to thread. It stops at a service boundary.

`kb_client` presents one of two credentials to the KB, and neither is the end user:

- an **unscoped** kb bearer, which the KB treats as the install owner. `kb_client.c` warns about
  exactly this: "aimee-server is acting as the aimee-kb install owner and passes every
  administrative gate";
- a **scoped** service credential (`scope:service:<name>:<secret>`), for which `kb_http.c`
  deliberately sets no actor at all: "a scoped kb-token is a limited service credential, not a
  tenancy actor".

So every content read reaches the KB as aimee-server, not as the person who asked.

## What already exists, and makes this tractable

The end user's identity **does** reach aimee-server, as the kb-signed **data-plane identity token**
(`kb_identity_token.h`), carried on `/v1` by the browser or thin client for per-user
`remote_writes`. `server_write_tier.c` already verifies it against the KB's JWKS and reads its
claims, which include:

| claim | use |
|---|---|
| `subject` (577 bytes) | the caller. The same width as `kb_team_membership.identity_key`, whose CHECK is 1..600 |
| `team_id` | the team, which is also what `set_tenant_context` takes |
| `issuer`, `audience`, `kid`, `jti`, `expires_at` | verification, replay, key rotation |

So both halves exist. aimee-server knows who is asking; the KB knows how to turn a principal into a
team set (`kb_identity_resolve.c`) and how to enforce it (RLS). They are not connected for reads.

## Decision to make

**Forward the token; do not assert the identity.** The token is signed by the KB's own authority, so
the KB can verify it rather than trusting aimee-server's word. `kb_http.c` already builds an actor
from a verified credential for OIDC and for an unscoped kb-token; a data-plane token becomes a third
source, and everything downstream is unchanged.

The alternative, a header where aimee-server states who it is acting for, is less work and strictly
weaker: it makes the KB's isolation depend on aimee-server being uncompromised, which is exactly the
property RLS at the database was chosen to avoid. It should only be considered if forwarding proves
impossible, and then written down as the compromise it is.

### Non-goals

- Changing the token. It already carries subject and team.
- Changing what a scoped service credential means. Service calls that legitimately have no user stay
  actor-less, and therefore see nothing once content scope is on. Which paths those are, and what
  they should do instead, is the open question below.

## The open question this raises

**Background and maintenance work has no user.** Ingest, re-embed, curator passes and the code
indexer read and write content on nobody's behalf. Once content scope is enabled they see nothing,
because deny-by-default is doing its job.

Three ways out, in preference order:

1. **Run them under an explicit maintenance scope** that is allowed to see everything, entered
   deliberately and audited, so the bypass is a named thing rather than an accident.
2. **Give them a per-project scope**, iterating projects and processing each under its own tenancy.
   Strongest, slowest, and awkward for anything genuinely cross-project.
3. **Leave those tables out of content scope.** Honest for job queues, which carry paths rather than
   content, but it must be a decision with a reason, not the residue of not deciding.

The job and queue tables (`kb_ingest_queue`, `kb_async_jobs`, `kb_code_unit_jobs`, `code_index_ops`)
all carry a `project` column and are read by exactly these workers, which is why they were left out
of the earlier slices.

## Bounded slices

1. KB accepts a data-plane identity token as an actor source, alongside OIDC and the unscoped
   bearer. Verified against the same JWKS, same actor construction, no new trust.
2. `kb_client` forwards the caller's token on data-plane calls. With RLS still off this changes no
   result, which makes it safe to land and observe.
3. Content reads open a tenant scope from that actor. Still no behaviour change while RLS is off.
4. The maintenance answer above, whichever is chosen.
5. Only then, set `kb_meta.content_scope_reader_ready = '1'` and let an operator call
   `kb_content_scope_enable()`.

## Acceptance checks

- **Mechanical.** With a forwarded token, a KB content read runs inside a tenant scope and
  `current_setting('aimee.principal')` is the caller's `identity_key`. Without one, no scope is
  opened and no principal is set, which must stay true so a service call cannot silently inherit the
  last user's identity on a pooled connection.
- **Integration.** Two users, two teams, RLS enabled in a scratch database: each sees only their
  own project's documents through the ordinary search path, not merely through direct SQL.
- **Negative.** An expired or wrong-audience token yields no actor rather than a fallback identity,
  and the read returns nothing rather than everything.

## Superseded in part

`per-user-content-scope-identity-map.md` corrects the "forward the token" decision here. It holds for
OIDC, which carries its own proof. It cannot hold for the webchat and local-CLI surfaces, whose
identities are attested by the operating system on aimee-server's own machine and have no signed
artefact to forward.

## Status

Pending, and blocking `kb_content_scope_enable()` by construction: the marker it checks is set by
slice 5 above. Evidence gathered on the merged state of #2643 and #2644.
