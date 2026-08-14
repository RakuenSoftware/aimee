# Every surface needs an identity, and two of them have nothing to forward

Amends `per-user-content-scope-reader-identity.md`, which said "forward the token". That is right for
one surface and impossible for two others. This is what it takes to cover MCP, CLI and browser.

No backwards compatibility is required: the next release moves a full version, so a caller with no
identity is refused rather than accommodated.

## What each surface is authenticated by today

aimee-server already attests every caller. It has never had to tell the KB.

| surface | attested by | KB principal kind | identity_key |
|---|---|---|---|
| browser / webchat | root-UDS-gated `webuser:<name>` assertion, PAM behind it | `KB_PRIN_HOST` | bare username |
| local CLI over UDS | `SO_PEERCRED` peer uid | `KB_PRIN_HOST` | bare username |
| thin client over TCP | mTLS client certificate | `KB_PRIN_CERT` | `cert:<issuer>:<serial>` |
| browser via OIDC | OIDC login, kb-signed data-plane token | `KB_PRIN_OIDC` | `oidc:<iss>:<sub>` |
| MCP | rides the `/v1` surfaces above | whichever applies | as above |

The KB's identity vocabulary already admits all four forms, `KB_PRIN_HOST` included, and
`kb_identity_key()` derives the canonical key for each. Nothing new needs inventing at the KB.

## The part the earlier proposal got wrong

Only the OIDC surface carries a **kb-signed token**. The webchat and UDS identities are attested by
the operating system and by PAM, on the machine aimee-server is running on. There is no signed
artefact to forward, and the KB cannot independently verify a peer uid it never observed.

So "the KB verifies, it does not trust" cannot hold uniformly. Pretending otherwise would mean
either dropping the two surfaces most people use, or inventing a proof the KB has no way to check.

## Decision: verify what carries proof, trust what cannot, and say which is which

Two paths, chosen by what the caller actually presented:

**Carries its own proof, so verify it.** An OIDC data-plane token is forwarded verbatim and the KB
checks the signature against the key it minted with, plus `typ` and audience so a management JWT can
never arrive here wearing the wrong hat. aimee-server is not trusted for this at all.

**Attested locally, so trust an authorized service, explicitly.** For host-account subjects,
aimee-server asserts the attested username over its scoped service credential, and the KB accepts it
only from a credential enrolled to speak for host subjects. The trust is real; the point is that it
is stated, narrow, revocable per credential, and enforced at one choke point rather than implied on
every read.

Write it down as the compromise it is: a compromised aimee-server can name any host account. It
cannot forge an OIDC subject, and it cannot widen a subject's teams, because membership is still the
KB's to resolve.

### Why not mint a token per local identity instead

The KB holds the signing key, so aimee-server would have to ask for one, and at mint time the KB is
trusting exactly the same assertion. It buys an audit record and costs a round trip on every new
subject. Worth revisiting if the audit record turns out to matter more than the latency; it does not
change who is trusted.

## Non-goals

- Changing the identity vocabulary. All four kinds exist.
- Backwards compatibility for callers with no identity. The version moves; they are refused.
- Per-user memory. `memories` is out of scope by decision: global is global, and per-user memory is
  DB1's concern.

## The surfaces that are nobody

Two callers legitimately have no user, and both must be answered before content scope is enabled:

- **UDS local operator.** OS-attested and, today, structurally exempt from the write tier because the
  local operator "keeps full capability". Under content scope, a caller with no principal sees
  nothing, which would take the local CLI dark. It maps to a host account, so the fix is to give it
  one rather than to exempt it.
- **Background work.** Ingest, re-embed, curator and the code indexer act on nobody's behalf. Named
  maintenance scope, per-project iteration, or leaving the job tables out. Still open.

## Bounded slices

1. **KB accepts a forwarded data-plane token** as an actor source: verify against its own authority
   key, check `typ`/audience, build the actor. No verifier exists KB-side today; `kb_identity_token.h`
   is a builder only.
2. **KB accepts an asserted host subject** from a service credential enrolled for it, and refuses it
   from any other credential.
3. **aimee-server forwards** whichever it has, on data-plane calls. With RLS off this changes no
   result, which is what makes it safe to land and watch.
4. **Content reads open a tenant scope** from the actor.
5. **The local operator and background work** get their answer.
6. Only then set `kb_meta.content_scope_reader_ready = '1'`.

## Acceptance checks

- **Per surface, end to end.** Webchat, local CLI, thin client and MCP each reach a KB content read
  with the right `identity_key` in `aimee.principal`. A surface that silently arrives as nobody is
  the failure this whole map exists to prevent, so each is asserted by name rather than in aggregate.
- **Assertion is not universal.** A service credential NOT enrolled for host subjects is refused when
  it asserts one, and the read returns nothing rather than everything.
- **No proof, no identity.** An expired, wrong-audience or wrong-`typ` token yields no actor at all,
  never a fallback.
- **No inheritance.** A service call with no user does not pick up the previous request's principal
  on a pooled connection. Pinned already by the tenant-scope leak test.

## Status

Pending. Supersedes the "forward the token" decision in
`per-user-content-scope-reader-identity.md` for the host-account surfaces, and keeps it for OIDC.
