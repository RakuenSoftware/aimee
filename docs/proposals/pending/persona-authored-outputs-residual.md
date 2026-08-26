# Persona-authored outputs: Go composition residual

- **State:** PENDING. Corrective Go-owned residual restored 2026-08-16 after rejection #2688.

**Archived parents:** [`persona-authored-outputs.md`](../done/persona-authored-outputs.md) and
[`persona-authored-outputs.plan.md`](../done/persona-authored-outputs.plan.md)

## Correction

PR #2688 rejected this residual because permission resolution had moved to Go while persona
composition, workflow blocks, and configuration still lived in C. That was not a valid reason to
discard the objective. The remaining decisions form one coherent Go boundary: `response-composition`
owns the authored output envelope, voice application, and authorship provenance; `delegates` supplies
the resolved actor, persona, and permission evidence; and `execution-policy` authorizes every concrete
side effect. Existing C workflow, Git, and output paths are compatibility adapters or relocation work,
not policy owners.

This document restores lifecycle state only. It does not claim that the runtime contract below has
been implemented.

## Why

An agent can currently produce a commit message, PR body, document, or review artifact through several
call paths. A persona name in a prompt is not proof of who authored the result, and a legacy caller can
lose or replace that name without preserving the actor, permission decision, source digest, or voice
outcome. Styling and provenance can consequently disagree even when the eventual repository or forge
operation is guarded.

The fix is to make Go composition return one immutable, provenance-bearing envelope, then require the effect owner to authorize and apply that
exact envelope. Voice is presentation; it must never become authority. Missing identity evidence,
impersonation, replay, and action denial therefore fail closed, while an optional voice transform may
fall back to the caller's exact draft without inventing authorship.

## Ownership and boundaries

- Go [`response-composition`](../../modules/response-composition.md) is the sole owner of authored
  envelope validation, persona voice application, source/output digest binding, and the typed
  composition result. Its existing deduplication stage is live; broader finalization is explicitly a
  relocation boundary and this proposal defines the next bounded stage.
- Go [`delegates`](../../modules/delegates.md) resolves one immutable authorship evidence value for an
  invocation. It owns role/persona eligibility and the full effective permission set; downstream
  callers do not recompute either.
- [`execution-policy`](../../modules/execution-policy.md) remains the final authorization owner for a
  commit, PR create/update, publication, or other effect. Composition cannot grant an effect merely by
  returning content.
- `git`, `workflows`, gateway, and legacy C callers consume the Go result. They may encode, transport,
  and apply an allowed envelope, but cannot choose its actor or persona, restyle it, rewrite its
  provenance, downgrade its version, or turn a denial into an allow.
- `audit` records composition and action decisions. It does not infer missing identity or repair an
  incomplete envelope after an effect.

## Versioned contracts

### `AuthorshipEvidenceV1`

Go `delegates` resolves this value once before authored output is requested. An authenticated human
ingress may construct the same value for operator-authored compatibility traffic. The evidence contains:

- schema version `1`, the admitted producer identity, invocation/correlation identity, and actor
  principal;
- the effective persona identity and persona-definition version, or explicit `none` only for an
  authorized compatibility request;
- the complete canonical ordered permission set. Every entry carries its normalized name, ordered
  scopes, and enforcement point. A digest, lookup key, or later re-resolution is not an alternative to
  carrying this set;
- exactly one trusted authoring mode: `persona_required` or `unstyled_compat`; and
- the source/workspace authority needed to bind the evidence to the producing invocation.

The mode is asserted by the admitted producer, not copied from an untrusted request field.
`persona_required` is the normal agent path. `unstyled_compat` is accepted only from an authenticated
human ingress or another producer explicitly authorized for that mode. Missing evidence, an absent
mode, stripped persona fields, or an unauthorized compatibility assertion is a denial.

### `AuthoredOutputRequestV1`

The request carries `AuthorshipEvidenceV1` plus:

- output kind (`commit_message`, `pull_request`, `document`, `review_artifact`, or a subsequently
  versioned kind), source kind and stable source identity;
- the caller's draft in the canonical shape for that kind and its digest;
- destination/effect class and target identity, so the same result cannot be replayed onto another
  repository, branch, forge item, document, or review;
- requested voice identity and definition version when mode is `persona_required`; and
- a bounded idempotency identity tied to actor, invocation, source, and destination.

The admitted producer identity is part of the module invocation context. A caller cannot override it
inside the request. `response-composition` accepts version 1 exactly; an unknown version or kind returns
a typed denial and is never interpreted as an older shape.

### `AuthoredOutputEnvelopeV1`

After validation and composition, Go `response-composition` returns an immutable version-1 envelope
containing:

- the actor, effective persona (or explicit `none`), admitted producer, invocation, source, destination,
  output kind, and complete permission evidence;
- the canonical output payload plus source and output digests;
- the requested voice identity/version and a voice outcome of `applied`, `not_requested`, or
  `fallback_used`;
- content-authorship provenance that distinguishes a persona transformation from an unchanged caller
  draft; and
- the correlation/idempotency identity and composition decision version.

A denied request returns a typed denial result and no applicable envelope. Consumers must reject an
unsupported envelope version; they cannot downgrade it or reconstruct missing fields.

## Composition and failure semantics

1. `response-composition` verifies the admitted producer, evidence version, actor and invocation
   binding, permission representation, authoring mode, source digest, destination binding, persona,
   and requested voice before producing an envelope.
2. A caller cannot name a different actor or persona alongside trusted evidence. A mismatch, forged or
   incomplete evidence, invalid persona, disallowed permission state, digest mismatch, cross-target
   replay, or duplicate idempotency identity with different content denies before application and emits
   bounded denial evidence.
3. `persona_required` denies when the effective persona or its trusted definition/version is missing.
   There is no silent fallback to `engineer`, a default persona, or a caller-provided name.
4. `unstyled_compat` preserves the caller's exact canonical draft, records persona `none` and voice
   `not_requested`, and binds the authenticated actor. It is an explicit trusted mode, not the result of
   an empty or stripped persona field.
5. Voice transformation runs at most once. If trusted evidence is valid but the optional voice provider
   is unavailable, returns invalid output, or reduces to empty, composition preserves the exact draft,
   records `fallback_used`, and records that no persona transformation authored the fallback. It does
   not attribute caller text to the requested persona.
6. Identity and provenance validation never fail open. Neither does action authorization. A style
   fallback can preserve content only after all trusted evidence has validated.
7. Composition is pure with respect to repositories, forges, documents, and review stores. The effect
   owner submits the envelope and concrete action/target to `execution-policy`; only an allow permits
   applying the exact payload.

## Compatibility and migration

The first migration adds the version-1 Go stage beside current callers, then moves one consumer at a
time. Existing guarded-Git enforcement and the repository ban on AI-attribution trailers remain the
outer controls and are not reimplemented here.

An old caller may enter `unstyled_compat` only through a trusted wrapper whose admitted identity is
authorized to make that declaration. An empty legacy persona does not opt in. Adapters preserve the
canonical draft bytes and all v1 fields, and compare the applied payload with the authorized output
digest. Malformed, missing, unsupported, timed-out, or cancelled composition results fail closed for
effectful paths. No adapter retains a local persona, voice, permission, or provenance decision.

Migration evidence must name every commit, PR, document/publication, and review-artifact consumer; show
which path is authoritative; and prove that any retained C implementation is a byte-preserving adapter
or parity fixture. Removal waits for live caller and packaging evidence rather than source relocation
alone.

## Acceptance

- Go unit tests construct `AuthorshipEvidenceV1` from one resolved delegate invocation and prove that
  actor, persona/version, ordered permission names/scopes/enforcement points, authoring mode, and source
  authority survive unchanged into the envelope.
- A valid `persona_required` request applies the selected voice once and returns matching source/output
  digests and persona-transformation provenance for each supported output kind.
- Forged actor or persona fields, evidence from another invocation, missing or reordered permission
  evidence, unknown persona/version, stripped authoring mode, and an unauthorized `unstyled_compat`
  assertion each return a named denial and no applicable envelope.
- An absent persona denies in `persona_required`. An explicitly authorized `unstyled_compat` request
  preserves the exact draft, records persona `none` and `not_requested`, and never becomes a silent
  default-persona request.
- Source modification, destination substitution, replay across actor/repository/branch/forge/document/
  review identity, or reuse of an idempotency identity with different content is detected before the
  effect and leaves denial evidence.
- Voice-provider absence, malformed/empty output, timeout, or cancellation after evidence validation
  yields the exact draft with `fallback_used` and no false persona authorship. The same failures cannot
  bypass evidence validation or execution policy.
- `execution-policy` denial prevents commit, PR, publication, or review persistence even when composition
  succeeded. An allow applies payload bytes matching the authorized output digest exactly.
- Unsupported request/envelope versions and unknown output kinds fail closed. Version-1 adapters never
  downgrade, omit fields, authorize locally, or translate a denial into an allow.
- Integration tests cover commit messages, PR title/body, document/publication output, and review
  artifacts through their live consumers, including retained C adapter parity where present.
- Migration evidence enumerates all legacy consumers and proves no supported path can choose its own
  persona, permissions, voice, provenance, or side-effect verdict.
- Existing guarded-git behavior and no-AI-attribution checks remain green.

## Non-goals

- This lifecycle correction does not implement the Go stage or mark the residual done.
- `response-composition` does not become a Git, forge, workflow, audit, or authorization engine.
- This proposal does not restore the delivered `require_aimee_git` work or create a general
  action-to-persona binding framework.
- Moving every legacy caller in one change is not required; allowing any retained adapter to make a
  policy or provenance decision is forbidden.
