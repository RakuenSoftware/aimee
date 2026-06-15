# Proposal: aimee workflows — a user-definable, autonomy-first development engine

- **State:** roundtable-converged READY (5 diverse-panel rounds; all lenses incl.
  contrarian); user-approved (proposal gate). Implementation remains BLOCKED-ON §0.
- **Author:** JBailes
- **Date:** 2026-06-14
- **Scope:** ship a **workflow engine** in aimee that executes **declarative,
  user-definable development workflows** as **autonomously as each workflow
  allows**. aimee ships a default "build" workflow (the base lifecycle below);
  users can adjust it or define their own. This replaces the per-deployment
  convention living in an operator's memory/CLAUDE.md.
- **BLOCKED-ON (hard prerequisite):** roundtable-panel-composition — see §0. The
  roundtable gate cannot be a real multi-lens review until that ships.
  **RESOLVED 2026-06-15: implemented and moved to `docs/proposals/done/`.** Builds
  on the delegate/coord system and the `docs/proposals/` convention.

## Goal

A workflow is a **first-class, editable object**, not hard-coded control flow. The
engine runs any workflow against a *work item*, holds the authoritative state,
drives delegates for every automatable step, and **only ever pauses where the
workflow says a human is required** — and even then only as long as that gate's
autonomy policy demands. Two properties are co-equal:

1. **User-definable** — the base workflow is the default; users edit it or author
   new ones (different stages, gates, panels, loop-backs).
2. **Autonomy-first** — everything between gates is delegate-automated; human gates
   are opt-in per workflow and can be pre-authorized so an autonomous run proceeds
   without blocking. "As autonomous as possible" is the default posture, bounded
   only by cost caps and the human gates the workflow author chose to keep.

## §0. Hard prerequisite & fail-closed gate

The diverse, multi-model panel the gates rely on is exactly what
roundtable-panel-composition delivers; today `delegate_ensemble.c` passes no
persona to panelists and codex can't review (no `tools` cap). Therefore this
proposal is **BLOCKED-ON** that dependency and cannot be implemented or marked
approvable until it ships and a diverse panel is demonstrably exercised.
**Panel-eligibility predicate:** a delegate is eligible for a gate iff
`tools_enabled: 1` (or the dependency's no-tools review mode), `review` in its
`roles`, and a reachable endpoint. **Fail-closed:** if a gate's required panel
can't be composed, the gate **refuses to advance** and surfaces the reason — it
MUST NOT silently degrade to a single-lens approval.

## §1. Composable blocks (the unit of composition)

A workflow is **composed from reusable building blocks** — each of the parts of the
base lifecycle is one block. A workflow is a **declarative, versioned, validated**
document (`$AIMEE_HOME/workflows/<name>.yaml`; aimee ships defaults; deployments
add/override) that **arranges block instances into a directed graph with loop-back
edges** — not a hard-coded list. The default workflow holds no special status; it
is just one composition.

### Artifact type system (makes the contract enforceable)
Artifacts are a **closed enum** flowing on **named handles** through the graph:
`proposal | plan | branch | frozen_diff | pr | verdict | approval`. Each block
declares typed inputs/outputs; a block instance binds each input to a **specific
upstream producer by handle** (`in: { pr: stages.impl_pr.out }`), not just by type.
The validator resolves every input handle to exactly one reachable upstream output
of the matching type and **rejects** (a) an unbound/typed-mismatched input, (b) two
producers feeding one input without an explicit `select`, and (c) a cycle that isn't
an explicit loop-back edge. **Bind-by-instance applies to every `pr`-consuming block**
(`gate.human`, `merge`): when a graph has more than one `pr.open`, each consumer
names its specific producing instance (`in: { pr: stages.proposal_pr.out }` vs
`stages.code_pr.out`). This closes the "two PRs, which one?" ambiguity for *both* the
PR-review human gates and `merge` — there is one `pr` type, disambiguated by handle,
so the type enum stays small while remaining unambiguous.

### Block catalog (the parts, as first-class blocks)
The catalog covers every part of the base lifecycle:

| block | params | consumes → produces |
| --- | --- | --- |
| `author.proposal` | `with_user: bool` | (user overview) → `proposal` |
| `author.plan` | — | `proposal` → `plan` |
| `implement` | `fanout: max\|N` | `plan` → `branch` (delegate fan-out) |
| `freeze` | — | `branch` → `frozen_diff` (§7) |
| `gate.roundtable` | `panel`, `quorum`, `max_rounds`, `in` | `<in: proposal\|plan\|frozen_diff>` → `verdict` (§4) |
| `gate.human` | `policy: interactive\|pr_review\|preauthorized`, `optional` (only with `interactive`/`preauthorized`), `in` (bind-by-instance for `pr`) | `<in: artifact\|pr>` → `approval` (§5) |
| `pr.open` | `in` (the source artifact) | `proposal\|frozen_diff` → `pr` |
| `merge` | `in` (the approved `pr`) | approved `pr` → terminal `accepted` |

Blocks are **reusable across positions** — `gate.roundtable` appears in the default
workflow three times (different `in`/`panel`); `pr.open` appears twice. Users compose
new workflows by instancing blocks and wiring typed handles + control edges; new
block types register by declaring their typed I/O without changing existing
workflows.

### A block instance (a graph node) has
`id`, `block` (catalog name), its `params`, typed `in:` handle bindings, and
`next`/`on_pass`/`on_fail` **control** edges (may point backward → loop-backs), plus
optional `autonomy: auto | require_human` and `cost_cap`.

`aimee workflow list | show <name> | validate <name> | edit <name> | new <name>`
manage definitions; `aimee workflow blocks` lists the catalog. `validate` enforces
the artifact type system above plus graph health (unreachable nodes, dangling
control edges, no terminal). A workflow's **`version` is the content-hash of its
canonical (normalized) definition**; in-flight work items pin the version they
started on, and editing a workflow mints a new version without mutating them.

## §2. The default "build" workflow (one composition of §1 blocks)

Shipped as `build.yaml` — purely a composition of catalog blocks, with no engine
privilege over a user-authored one:

1. `author.proposal` (id `draft`) → `proposal`.
2. `gate.roundtable` (id `proposal_gate`, `in: draft.proposal`) → loops to (1) on
   REQUEST_CHANGES until APPROVE.
3. `pr.open` (id `proposal_pr`, `in: draft.proposal`) → `pr`.
4. `gate.human` (id `proposal_approve`, policy `pr_review`, `in: proposal_pr.out`) —
   user approves the proposal PR. **(human gate 1)**
5. `author.plan` (id `plan`) → `plan`.
6. `gate.roundtable` (id `plan_gate`, `in: plan.plan`) → loops to (5) until APPROVE.
7. `implement` (id `impl`) → `branch`. **As many delegates as possible.**
8. `freeze` (id `freeze`) → `frozen_diff`; `gate.roundtable` (id `impl_gate`,
   `in: freeze.frozen_diff`, §7) → loops to (7) until APPROVE.
9. `pr.open` (id `code_pr`, `in: freeze.frozen_diff`) → `pr`.
10. `gate.human` (id `pr_passfail`, policy `pr_review`, `in: code_pr.out`) —
    **pass → (11) merge; fail → loop back to (7) implement.** **(human gate 2)**
11. `merge` (`in: code_pr.out`) → terminal `accepted`.

Loop-backs (2→1, 6→5, 8→7, 10→7) are first-class edges. A user can clone
`build.yaml` and, e.g., drop a human gate, add a `security-audit` roundtable stage,
or set both human gates to `preauthorized` for a fully-autonomous variant.

## §3. Engine + authoritative state

- **State = `(work_item_id, workflow_name, workflow_version, current_stage,
  mode)`**, driven entirely by the pinned workflow graph — the engine has no
  hard-coded lifecycle. Default store DB1 (per-user sqlite); `lifecycle.store: db2`
  for team-shared.
- **Single source of truth = the work-item row.** Proposal front-matter carries a
  *derived/regenerated* status block (a cache); on divergence the row wins, `--force`
  to overwrite from front-matter.
- **Work-item id** is **server-minted and non-forgeable**: `wi_<random-128-bit>`
  generated server-side at creation (never derived from client-supplied or
  forgeable inputs like a client timestamp), with `(normalized_repo_url,
  proposal_path)` stored as columns and a UNIQUE constraint that rejects a second
  open work item for the same proposal (repo URL normalized — scheme, trailing
  `.git`, host case — so fork/re-push variants don't silently diverge). Surfaced,
  embedded in front-matter, the positional arg every verb takes.
- **Terminal states:** `accepted`, `rejected` (human-initiated where the workflow
  permits), `abandoned` (any non-terminal → terminal; requires `reason` + `actor`).
  `aimee lifecycle status` distinguishes in-progress / rejected / abandoned.
- **Loop-back accounting:** each loop-back increments an attempt counter on the
  target stage; a per-stage `max_attempts` (default from cost caps) prevents an
  infinite PR-fail→implement→fail cycle and forces a human decision when hit.
- **Audit:** every transition appends to an immutable `lifecycle_event` log
  (actor, stage, verdict/approval id, artifact content_hash, cost). Retained for the
  work item's lifetime + a configurable `lifecycle.audit_retention_days` after a
  terminal state, then GC'd; transcripts are referenced by id (stored with the
  delegate job records), not duplicated inline.
- **Concurrency:** ≤1 in-flight `advance` per work item (`BEGIN IMMEDIATE` / advisory
  lock) so two terminals can't double-spend.
- **Row schema** carries `pause_reason TEXT NULL` + `paused_state TEXT NULL` so
  `status` renders e.g. "paused: budget_exceeded" without scanning the audit log.

## §4. Roundtable gate primitive (fail-closed, contract-typed)

`gate.roundtable` runs the §1-configured panel across eligible models (§0); **the
panel is composed once per round, not re-composed mid-round.** Each panelist returns
a **strict, integrity-bound contract**:
`{schema_version: 1, reviewed_content_hash, verdict: APPROVE|REQUEST_CHANGES|COMMENT,
blockers:[{file,line,severity,summary,id}], comments:[...]}`.
- **Integrity (fail-closed):** unknown `schema_version`, malformed/empty output, or
  a `reviewed_content_hash` ≠ the lifecycle's hash of the artifact under review ⇒
  the verdict is treated as **REQUEST_CHANGES** (defends against a panelist
  approving a tampered/different artifact). A per-delegate consecutive-malformed
  **circuit breaker** (3 in a row) removes that delegate from the eligible pool for
  the rest of the work item and records a `lifecycle_event`.
- **Panel `required` vs `eligible`:** a panel declares `required` personas (each
  MUST be reachable and return a valid verdict, else the gate is "panel can't be
  composed" → refuse, §0) and an `eligible` pool (extra voters). The gate is composed
  once per round from reachable members.
- **Mid-round panelist failure:** if a panelist drops mid-round (network/delegate
  crash), the gate **pauses with `pause_reason=panel_degraded`, records the failure,
  and that round does NOT count toward `max_rounds`**; `aimee lifecycle retry-round
  <id> <stage>` (or the autonomous re-composer) re-composes and re-runs. A *required*
  panelist lost mid-round → refuse until restored; an *eligible* one → re-run with
  the remaining pool if quorum is still satisfiable.
- **Advance rule (precise):** the gate advances iff (a) every *required* persona
  returned a valid verdict, (b) the count of `APPROVE` verdicts (from required +
  eligible) ≥ `quorum`, where `COMMENT` counts as *present but not approve*, and (c)
  **zero unresolved high-severity blockers from any source**. Quorum is over
  *verdicts*; high-severity blockers gate independently of quorum. The **contrarian
  is one APPROVE-or-not vote, not a structural veto**: its non-high-severity blockers
  inform but don't block.
- **Quorum default + single-lens floor:** `quorum` defaults to `count(required)` and
  is clamped to a **hard floor of `max(2, count(required))`** — so even a workflow
  author who writes `required: [one_persona]` cannot produce a single-lens approval
  (the §0 invariant). `validate` **rejects at author time any `gate.roundtable` whose
  `required` set has size < 2** — the canonical single-lens footgun is caught before
  the workflow can run.
- **Approval binds the reviewed hash (parity with §5):** an APPROVE records the
  `content_hash` it approved. For `in: proposal|plan` that hash is the artifact's
  content; **for `in: frozen_diff` it is `hash(git diff <base> <freeze_sha>)`**. If
  the bound hash changes before the work item advances past the gate (a `proposal`/
  `plan` edited post-APPROVE, or a new commit changing the freeze SHA, §7), the
  recorded hash no longer matches → the gate **re-opens and re-reviews**. No
  silently-stale roundtable approval.
- **No deadlock (closes §4/§6 interaction):** a hash change means a *genuinely new
  artifact*, so re-opening **resets the round counter** — it is not the same stuck
  gate. `max_rounds` + `gate-override` apply only to a gate that keeps failing on the
  *same* (unchanged-hash) artifact. So "stuck at `max_rounds`" and "re-opened by an
  edit" are disjoint: an edit always gives a fresh review budget; override is only
  ever for genuine no-progress on a fixed artifact.
- **"Resolved" is defined:** a blocker (keyed by its `id`) is resolved iff the next
  round's verdict from the same persona no longer lists that `id` **or** the diff
  against the previously-hashed artifact no longer touches the cited `file:line`.
  Unresolved blockers carry forward.
- **Round cap:** on `max_rounds` the work item parks; the only way past is
  `aimee lifecycle gate-override <id> <stage> --reason ...` (interactive confirm;
  recorded; `override_count` capped at `max_overrides` default 2 → then `rejected`).
- **Anti-gaming:** per-round panel rotation with a recorded seed + a rubber-stamp
  metric (per-model, global rolling % of round-1 APPROVEs) surfaced in `status`.
- Records transcript + verdict ids + panel composition + reviewed artifact
  `content_hash`.

## §5. Human gates — non-repudiable, autonomy-aware

A human gate's `policy`:
- `interactive` — blocks on a TTY confirmation written only by `aimee lifecycle
  approve` under an operator-owned process (not a delegate child).
- `pr_review` — realized as a forge PR; the user's PR approval / merge *is* the
  gate (the natural home for "create PR with proposal, user approves").
- `preauthorized` — the user grants standing approval for a workflow or a class of
  work items ahead of time (signed token / config), so an autonomous run proceeds
  without pausing. This is the lever for "as autonomous as possible": a workflow
  can keep a human gate for safety yet still run unattended once pre-authorized.

`optional: true` (auto-advance without an approval) is permitted **only** with
`interactive` or `preauthorized` policy — `validate` rejects `optional: true` with
`pr_review`, since a `pr_review` gate is *realized by opening a PR* and "optionally
skip opening it" is contradictory.

**Approval mechanism (specified, not convention):** an approval record is signed
with an **operator approval key** held in `$AIMEE_HOME/.approval-key` (mode 0600,
owned by the operator account), which delegate processes run without read access to
(enforced by the same credential-isolation boundary as the cred vault — delegates
never receive it). `aimee lifecycle approve` signs `{actor, gate, work_item_id,
artifact content_hash, timestamp}`; the engine verifies the signature against the
public half recorded at deploy. A delegate cannot mint a valid approval because it
cannot read the key. For `pr_review` policy the forge's own PR-approval (GitHub
review state) is the trusted signal instead. Editing the artifact changes its hash →
the recorded `content_hash` no longer matches → the approval is visibly stale and
the gate re-opens. `preauthorization` is a signed grant over a workflow + work-item
class, same key, with an expiry.

## §6. Autonomy model

- A work item has `mode: interactive | autonomous`. In `autonomous` mode the engine
  auto-advances every `gate.roundtable` on APPROVE and every `gate.human` whose
  `policy: preauthorized` (or `optional: true`); it parks at any human gate not
  pre-authorized, recording `pending_human`, and resumes when the approval artifact
  appears. It **never** forges a human approval.
- **`gate-override` is itself a recorded human approval** (signed with the §5
  approval key, capped by `max_overrides`). Therefore **autonomous mode cannot
  self-override** a stuck roundtable — it parks as `pending_human` exactly like an
  un-pre-authorized gate. This preserves the "never forges a human approval"
  invariant: the escape hatch from a stuck gate is a human action by construction.
- **Pause/resume semantics (enumerated).** Each pause records a `pause_reason`:
  `pending_human` (human gate / override needed) and `panel_degraded` park for human
  or recomposition; `budget_exceeded` parks until `resume --budget-bump` (a human
  act); `panel_unreachable` auto-retries with backoff up to a bound, then parks. An
  autonomous run never silently leaks: a paused item is visible in `status` and
  resumes only on its named condition.
- The existing autonomous-proposal-loop becomes a **caller** of this engine (drive
  N work items through their workflows, hard-stop only at un-pre-authorized human
  gates), not a parallel system. Idempotent, resumable `advance` is what makes
  autonomy safe to interrupt.
- Autonomy is bounded by the §9 cost caps + a global concurrent-work-item rate
  limit — the safety envelope, not a human in the loop.

## §7. Implement → freeze → PR → loop-back

`implement` runs delegate-driven on the work item's branch, fanned across as many
delegates as the plan's tasks allow. **Freeze (stable, reproducible diff):** at
freeze the engine records both a **base SHA** (`merge-base` of the branch and its
target at freeze time, written as a `wi/<id>/freeze-base` ref) and a **freeze-commit
SHA** (the branch tip). The reviewed artifact is exactly `git diff <base> <freeze>`
— immutable for the gate's duration; its hash is the §4 `content_hash`. Implementing
delegates are paused; **any commit to the branch during the gate changes the freeze
SHA and re-opens the gate** (no diff drift mid-review, so verdicts are reproducible).
The engine distinguishes two re-open kinds so a trivial commit doesn't cost a review
round: `freeze_invalidated` (the freeze SHA changed but the resulting `frozen_diff`
hash is identical — e.g. a no-op/whitespace commit — so re-hash silently, **no
`max_rounds` increment**) vs `artifact_changed` (the `frozen_diff` hash differs — a
full re-review that **resets the round counter**, §4). APPROVE → `pr` stage. The `pr_passfail` human gate: **pass → `merge`
(terminal `accepted`); fail → loop back to `implement`** with the user's failure
notes attached (bounded by the stage's `max_attempts`). Branch and verdicts are
retained in the audit log across loops.

## §8. Panel configuration

Panels are versioned aimee config, not workflow-text drift: `gate.roundtable.panel`
references persona names resolved against `$AIMEE_HOME/personas/`. aimee ships a
default panel (security, architect, qa, contrarian-reviewer, standard-reviewer) so a
fresh install works; deployments override. A gate refuses to run if a required
persona is unavailable (fail-closed, §0).

## §9. Cost & budget (two-level + autonomy bound)

`ensemble.max_cost_usd` (USD, from the existing per-model pricing) bounds one call,
not a workflow. Add per-stage `cost_cap`, per-gate `gate_max_cost_usd`, and a hard
per-work-item `work_item_max_cost_usd`. Enforcement is **atomic, not racy**: before
each delegate invocation the engine **reserves** the call's estimated max cost
against `cum_cost_usd` in the same locked transaction that guards the work item (§3),
and reconciles to actual on completion — so concurrent invocations can't both pass a
stale-read pre-flight and overshoot. On cap the work item pauses with
`pause_reason=budget_exceeded`, requiring `aimee lifecycle resume --budget-bump ...`.
`cum_cost_usd` shows in `status`. Autonomous runs additionally obey a global
concurrent-work-item limit.

## §10. Trigger — what enters a workflow

A change is workflow-required iff any of: touches a configured
security/persistence/auth/network glob set, modifies a public API (detected by file
glob in v1; signature-diff analysis is a v2 refinement), or `--lifecycle` opt-in.
**Enforced, not opt-out:** for a repo with the lifecycle enabled, a matching change
that tries to bypass the workflow is **blocked at the commit/PR gate** (a guardrail
hook refuses the push), not merely suggested — otherwise "code, not convention"
fails. A human can still escape via the recorded `gate-override` path, never
silently. Per-repo config with shipped defaults; trivial edits skip. v1 heuristic.

## §11. Migration

First run maps existing `docs/proposals/pending/*` free-text `State:` lines to the
default workflow's stages, mints work-item ids, regenerates the derived front-matter
block; in-flight proposals adopt `mode: interactive` pinned to `build.yaml`. A
**missing or unrecognized `State:` line defaults to `draft`** (never to a terminal
state) and is flagged in `aimee lifecycle list` for operator confirmation, so a
malformed legacy file can't silently auto-advance or auto-close.

## §12. Web visual composer (webchat frontend)

The block-graph model (§1) maps directly to a **visual node-graph editor** in the
webchat SPA (`aimee/frontend/`). The editor is strictly a **view over the same
server-authoritative workflow definition** — it never becomes a second source of
truth:
- **Palette = the §1 block catalog** (served by `aimee workflow blocks` over a
  `/v1` route); drag a block onto the canvas to instance it, wire edges (including
  loop-backs) by connecting node ports, edit `params` in a side panel.
- **Save/load** read and write the same `<name>.yaml` via `/v1` routes. Save runs a
  **canonical-form normalize** server-side before persisting, so a CLI↔web round-trip
  is byte-stable and the content-hash `version` (§1) is meaningful; optimistic-lock
  save rejects on a `version` mismatch. Because loop-backs make the graph **cyclic**
  (topological sort is undefined), the canonical form is defined as: nodes emitted in
  **lexicographic `id` order**, each node's edges as an explicit `{kind, to}` list in
  fixed kind-then-target order, handles named deterministically from
  `<producer_id>.<output>` — a total order that is stable under any cycle.
  CLI-authored and web-authored workflows are interchangeable.
- **Validation is server-side** (the §1 `validate` — type system + graph health),
  surfaced inline on the canvas; the web layer adds no independent rules.
- **Live run view:** renders the workflow **version the work item is pinned to**
  (§3), not the latest definition — so the highlighted `current_stage` always exists
  in the rendered graph — and shows per-gate verdict/approval state (§3 audit log).
  The same graph doubles as a status dashboard.
- **Out of scope for v1 (separate slice):** real-time collaborative editing; v1 is
  single-editor with optimistic-lock save (reject on version mismatch). The visual
  composer is a deliverable slice *after* the engine + CLI land, since it depends on
  the `/v1` workflow routes and the block catalog being stable.

## §13. Test plan

- Unit: the §1 **artifact type system** validator (unbound/typed-mismatch input,
  ambiguous two-producer input, non-loopback cycle all rejected); canonical-form
  normalize is byte-stable across a CLI↔web round-trip; the graph engine incl.
  loop-backs + `max_attempts`; the verdict-contract validator (malformed ⇒
  REQUEST_CHANGES); quorum + COMMENT semantics; cost pre-flight; override cap;
  approval-hash staleness.
- Fixture: a gate driven by canned verdicts (APPROVE / mixed / malformed /
  perpetual-contrarian / COMMENT-only-from-a-required-persona); a PR-fail loop-back
  that re-enters `implement`; a commit landing mid-gate invalidating the freeze;
  `reviewed_content_hash` mismatch ⇒ REQUEST_CHANGES (tampered artifact); per-delegate
  malformed circuit-breaker; all-delegates-unreachable and panel-below-minimum both
  fail-closed with a recorded reason; a **required** panelist dropping mid-round →
  `panel_degraded` pause that does NOT burn a round; a roundtable APPROVE invalidated
  by a post-approval artifact edit; content-hash re-opening a human gate; atomic
  cost-reservation under concurrent invocations.
- End-to-end: the default `build.yaml` through both human gates (mocked) +
  roundtable gates; an `autonomous` run with both human gates `preauthorized` that
  completes unattended, **parks (cannot self-override) at a stuck roundtable**, and
  hard-stops a *non*-preauthorized human gate; a custom workflow that adds a stage
  and removes a human gate.

## Alternatives considered

- **One hard-coded lifecycle** (the round-1/2 framing of this doc): simpler, but the
  user requires adjustable workflows — rejected in favor of the engine + a default
  workflow.
- **Keep it a convention / docs-only guide:** not portable, drifts, no autonomy.
  Rejected (ship a CONTRIBUTING guide *alongside* as the human companion).

## Open questions (non-blocking; defaults chosen above)

1. Workflow definition format (YAML assumed) and how far the graph schema goes in v1
   (parallel stages? sub-workflows?) vs v2.
2. `preauthorization` mechanism strength (config flag vs signed token) for human
   gates in autonomous mode.
3. Forge abstraction breadth (GitHub first; local-forge/patch later).

## Risks

- **Dependency slip** (§0): stays blocked, gate fails closed — no weak-gate fallback.
- **Over-autonomy:** a fully pre-authorized workflow could merge unreviewed-by-human
  code — mitigated because the *roundtable* gates never auto-weaken (§0/§4) and cost
  caps + loop-back `max_attempts` bound runaway loops; pre-authorization is an
  explicit, recorded user act.
- **Workflow misconfiguration:** a user-authored workflow that removes all gates —
  `validate` warns; the engine still enforces §0 fail-closed on any roundtable stage
  present, but cannot add safety a user deleted (documented sharp edge).
- **Runaway spend / over-process / gate-gaming:** bounded by §9 caps, the §10
  trigger, and §4's fail-closed parsing + dissent carry-forward + rotation + the
  rubber-stamp metric.
