# Autonomous-dev webchat GUI + Random delegate — plan

Two user-requested additions on top of the (API-first) autonomous-development
feature. Scope-honest: the webchat is currently a read-only run viewer.

## Status: SHIPPED (2026-06-23)

All three slices are implemented and unit-tested:

- **C — `$random` delegate.** `wfe_resolve_delegate` / `wfe_resolve_delegate_seed`
  (`src/server/wfe_delegate_resolve.c`): CSPRNG uniform pick over enabled agents,
  seedable, fail-fast on an empty roster; wired into the live delegate + block
  providers; frontend composer option present. Test
  `unit-test-wfe-random-delegate`.
- **B1 — submit form.** `POST /v1/dev/submit` + webchat `POST /api/dev/submit`
  proxy + the Workflows-tab "Submit proposal" panel.
- **B2 — operator gate approve/reject.** `POST /v1/workflow/items/<id>/gate`
  (`CAP_WORKFLOW_ADMIN`, outside `CAPS_AUTHENTICATED`) + proxy + Approve/Reject
  buttons shown on `pause_reason == pending_human`. The decision is applied via a
  single guarded `db1_work_item_gate_apply` UPDATE (WHERE current_stage +
  content_hash match AND pause_reason='pending_human') so a stale / double /
  concurrent decision returns 409 instead of corrupting state. Approve is
  idempotent (signed approval pre-check); **reject is terminal by default**, with
  an opt-in `retry_on_reject: true` gate param that loops back along the gate's
  `on_fail` edge (`wfe_gate_reject_target`), bounded by `GATE_MAX_REJECT_RETRIES`
  (default 3) and degrading to terminal if the `on_fail` target no longer
  resolves. Every decision appends an append-only `lifecycle_event`
  (`approve | reject | reject_retry`). Tests `unit-test-wfe-gate-reject`,
  `unit-test-wfe-gate-apply`, `unit-test-workflow-gate-caps`.

This plan addressed the roundtable's five blocking review items (reject-side
TOCTOU/idempotency, the content-hash invariant, audit-event signing semantics,
unbounded retries, and pause→reject def staleness). Two follow-ups are tracked,
not silently deferred:

- **F1 — version-pinned def loading** for in-flight work items (engine-wide; the
  pause→reject window is one instance). Mitigated here by the helper validating
  the `on_fail` target against the freshly-loaded def.
- **F2 — UI discoverability of retry eligibility** (badge / "Reject → loops back"
  label) so an operator knows a `retry_on_reject` gate's Reject loops rather than
  terminates. Frontend-only.
- **F3 — idempotent signed approval.** The approve path records the signed approval
  before the atomic pause-clear; on a concurrent advance the clear returns 0 (409)
  and the signed approval is orphaned. Bounded and harmless (it is content-hash-keyed
  to `(work_item, gate, content_hash)`; a later approve sees it present and returns an
  idempotent 200). Making `wfe_approval_record` treat a duplicate as success would
  remove the orphan entirely.

## B — webchat GUI for autonomous development

### B1. Submit a proposal (form)
- **Backend:** webchat Go proxy `POST /api/dev/submit` → `/v1/dev/submit`
  (carrying the attested webuser identity, like the other `/api/*` proxies). The
  `/v1/dev/submit` route already exists.
- **Frontend:** a "Submit proposal" panel in the **Workflows** tab — a Markdown
  textarea (`proposal_md`) + a workflow `<select>` (default `build`, from
  `/api/workflow/defs`) + a Submit button. On success show the new
  `work_item_id` and refresh the run list.

### B2. Approve / reject a parked human gate
- **Backend (new):** `POST /v1/workflow/items/<id>/gate {gate, decision}` where
  decision ∈ {approve, reject}. Approve → `wfe_approval_sign` +
  `wfe_approval_record` (HMAC with the operator key, **server-side** — the
  webchat user never sees the key); reject → record a `reject` lifecycle event
  and drive the gate's fail path. Then `wfe_scheduler_notify()` to resume.
  Webchat proxy `POST /api/workflow/items/<id>/gate`. Capability: CAP_DELEGATE
  (webuser-attested), matching `/v1/dev/submit`.
- **Frontend:** in the Workflows run viewer, when a run's `pause_reason` is
  `pending_human`, show **Approve** / **Reject** buttons that call the proxy and
  refresh.

### B3. Docs
Flip the doc's "in progress" notes (just corrected in #527) to describe the
shipped submit form + in-tab gate controls.

## C — "Random" delegate

A workflow step (panel lens / producing step) can be assigned the delegate
**`$random`**; at runtime it resolves to a uniformly-random agent from the
configured roster.

- **Composer (frontend):** add `Random` to the delegate picker suggestions in
  `Workflows.tsx` (stored as the sentinel `$random`).
- **Runtime (backend):** a small resolver `wfe_resolve_delegate(name, acfg)` —
  if `name == "$random"`, pick a uniformly-random enabled agent from `acfg`
  (seedable for tests); else return `name`. Wire it everywhere a step's delegate
  name becomes an agent: the panel provider and the live delegate provider.

## Open questions → roundtable

- **Q1 (authz).** The approval key is operator-only by design; the webchat is
  single-principal. Is "any authenticated webchat user may approve a gate via the
  server-side signer" acceptable, or should gate-approval require a stronger
  attestation than `/v1/dev/submit`?
- **Q2 (reject semantics).** Should `reject` loop the gate's `on_fail` (retry) or
  force terminal `rejected`? Per-gate, or a fixed policy?
- **Q3 (random scope + fairness).** Should `$random` exclude the primary/just-used
  delegate (avoid repeats) or be pure-uniform? Does it apply to panels only, or
  also producing steps? How does it behave while the live panel provider is still
  `DEGRADED` (not yet composable)?
- **Q4 (idempotency).** Double-approve / approve-after-resume races — the signed
  approval is content-hash-bound; confirm a stale approval can't re-fire.

## Roundtable resolutions (2026-06-19, security + architect lenses — CHANGES-REQUESTED → incorporated)

Both lenses flagged the same blockers; adopted:

- **Q1 / B2 authz (BLOCKING — privilege escalation):** the operator-only gate
  signer must NOT be reachable by a mere `CAP_DELEGATE` webuser. Gate approval
  requires a **distinct operator-level capability** (`CAP_OPERATOR`), separate
  from `/v1/dev/submit`'s `CAP_DELEGATE`. The signing key stays server-side; the
  webchat (single-principal) maps its admin to the operator cap. Documented as a
  single-principal assumption with per-gate ACLs as a future extension.
- **Q2 reject semantics (BLOCKING):** `reject` is **terminal (`rejected`) by
  default**; a gate may opt into retry via `retry_on_reject: true` in its def
  (follow `on_fail`). Prevents unintended loops/DoS.
- **Q3 `$random` (BLOCKING — degraded/empty behavior):** resolver picks a
  uniformly-random **enabled** agent via a CSPRNG, **seedable for tests**; **pure
  uniform** (no exclusions); applies to producing steps and panels. If the roster
  is empty it **fails fast with a clear error** — it never leaks the literal
  `$random` sentinel downstream. (The live panel provider is still DEGRADED, so
  `$random` is effective on producing steps now; panel use lands when the panel
  provider is wired.)
- **Q4 idempotency:** approvals are content-hash-bound; additionally **guard with
  `wfe_approval_present`** before recording, and return success (not an error) on
  a duplicate approve so the UI is race-safe.

**Design status: resolved.** Implementation order: C (random resolver + composer
option, lowest risk) → B1 (submit proxy + form) → B2 (operator-capped gate
endpoint + approve/reject UI), each compiled + tested, then the doc flips from
"in progress" to shipped.
