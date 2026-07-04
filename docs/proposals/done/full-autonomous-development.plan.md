# Full Autonomous Development — Implementation Plan

Plan for the approved proposal (`full-autonomous-development.md`). Phased; each
phase is independently shippable and roundtable-gated. **Scope invariant: aimee
never self-initiates — every run originates from a human-submitted proposal.**

## Phase A — real delegate-driven blocks + forge (WP-1, WP-2)

Goal: the existing `build.yaml` lifecycle produces *real* work when driven. No
scheduler yet (a human still advances it in the webchat); this proves the blocks.

### A1. Delegate dispatch seam (mirrors the `g_forge` pattern)
The wfe library must not hard-depend on `server/` delegate internals
(module-boundary-check). Add a registered hook, mockable in unit tests:

```c
/* wfe_iface.h */
typedef struct {
  /* Run one bounded delegate turn in `worktree`, role/persona + prompt given,
   * writing files into the worktree. Returns 0 on success; fills commit_sha if
   * it committed. Non-zero => the caller emits a verdict/fail, never a crash. */
  int (*run)(const char *worktree, const char *role, const char *prompt,
             const char *artifact_path, char commit_sha[64], char *err, size_t n);
} wfe_delegate_provider_t;
void wfe_set_delegate_provider(const wfe_delegate_provider_t *p);  /* test mock */
```
Server registers a `LIVE_DELEGATE` that calls `delegate_run_inline` (sync core,
per [[delegate-refactor-and-roundtable-delegate]]) with per-delegate
source-authority TLS context (race-safe, per [[delegate-concurrency-env-race]]).

### A2. Per-work-item worktree
Each work item gets an isolated `git worktree` + branch
(`aimee/wi/<work_item_id>`), created on first `implement`/`author`, reusing the
existing worktree machinery + worktree-GC (per [[worktree-gc-dead-wired-fix]]).
`wfe_ctx` gains a resolved `worktree` accessor. All A1 delegate runs and the
freeze/pr.open git ops act on this worktree, not the server cwd.

### A3. Wire the executors
- `exec_author` (proposal/plan): dispatch a delegate (role `architect` for
  proposal, `engineer`/`architect` for plan) to author/edit the artifact at
  `wfe_ctx_proposal_path` inside the worktree; commit; hash the artifact as the
  produced handle (keep the existing fail-closed-if-absent behavior).
- `exec_implement`: the **manager loop** (WP-1b). The primary (autonomy driver +
  a coordinator delegate) only *manages* — it never writes code or does hands-on
  verification:
  1. split the plan into independent work units;
  2. fan out each unit to an `engineer` delegate (via `delegate_patch_coordinator`);
  3. verify each unit via delegates/gates — mechanical (build/test/lint), review
     (`reviewer` panel), adversarial (skeptic refuters); the primary only
     *adjudicates* the verdicts;
  4. on reject, **re-delegate the unit to a *different* delegate** (bounded
     retries, Q2); on exhaustion, park (Q4).
  Each accepted unit lands a commit on the branch; fail-closed if nothing landed.
  Decomposition + verification policy are config-driven (fan-out width, retry
  caps, which verification tiers are mandatory).
- `exec_document`: delegate writes docs/comments onto the branch; commit.
- `exec_pr_open`: `git push` the work-item branch, then `g_forge->open(repo,
  branch, title, body)` (new vtable method) → store the PR ref on the work item
  so `pr_ref()` resolves a real PR (today it returns the work-item id). Honor
  [[no-coauthor-trailers]] in commit/PR text.

### A4. Tests
Unit: mock `wfe_delegate_provider` + `g_forge`; assert author/implement/document
advance with a commit, pr.open stores a PR ref, failures emit verdict/fail (not
crash). Integration-gated live path unchanged in spirit.

## Phase B — server-owned scheduler + intake (WP-3, WP-4)

### B1. Autonomy scheduler (Q1)
A server-owned component that owns active autonomous work items and resumes
`wfe_autonomy_run()` when a park-clearing event fires:
- a work-item delegate turn completes (hook off `turn_registry`);
- `gate.ci` transitions (Q3 — webhook vs poll);
- a human satisfies a `gate.human` (already an API mutation → fire resume);
- periodic backstop sweep (crash recovery) over persisted `lifecycle_*`.
Bounded concurrency; per-work-item single-flight (mirror `turn_registry` lock
discipline). Built on the Phase-1 server-owned turn lifecycle so the driver and
its turns survive client disconnect.

### B2. Intake
`POST /v1/dev/submit {proposal_md, workflow?, limits?}` → create work item bound
to `build.yaml`, seed the proposal artifact into its worktree, register with the
scheduler. Webchat "Submit for autonomous development" affordance; live via
presence ring, post-disconnect via cursor replay. Human gates surface as
actionable notifications. **Intake is the ONLY way a run begins — no
self-initiation path exists.**

## Phase C — policy hardening (WP-5 depth)
Failure taxonomy (Q4: park vs auto-retry vs terminal-reject), budget tuning,
multi-run isolation validation (Q5), CI-loop retry caps (Q3).

## Safety rails (all phases)
Human gates never auto-satisfied unless preauthorized; gate-override stays
human-only + capped; hard per-run budget ceiling → park (never run away);
autonomous merges only to `testing` (promotion to `main` stays human); every
commit/PR audited and attributed to the run.

## Open questions → roundtable (Q1–Q5 from the proposal)
Q1 scheduler home; Q2 implement granularity + retry bound; Q3 CI feedback
loop + red-CI retry cap; Q4 failure taxonomy; Q5 multi-run worktree/source-auth
isolation under load.

## Roundtable resolutions (2026-06-19, 2 mistral lenses — architect + security)

Verdict: **no blocking flaws** in Phase A; invariant (all-autonomy-through-wfe)
**confirmed sound** by both. Converged decisions:

- **Q1 — scheduler home:** Extend the existing **coord-dispatcher / `turn_registry`**,
  not a new subsystem. The scheduler is "just another turn type" that resumes
  `wfe_autonomy_run()`; reuse single-flight, crash recovery, and presence
  integration from PR #514.
- **Q2 — implement granularity:** Fan-out over plan units (one delegate per
  file/module unit) + a **patch-coordinator** merge delegate. Retry bound **2–3
  per unit**; if >50% of units fail, park. Model the patch-coordinator as a
  sub-block of `implement` (its own verdict/retry, audited) so it stays inside
  the wfe catalog (the only invariant gap either lens found).
- **Q3 — CI loop:** **Webhook** for `gate.ci` (not poll), firing the scheduler's
  resume hook. Red-CI auto-retry cap **1–2**, looping back to `implement` with
  the CI failure log as input; then park.
- **Q4 — failure taxonomy:** transient delegate/CI errors → bounded auto-retry;
  model refusal / permanent error → terminal-reject; roundtable-degraded,
  budget-breach, git/forge failure → park (`pending_human`); worktree corruption
  → terminal. **Invariant: never auto-retry without new input** (CI log /
  roundtable verdict) — prevents infinite loops.
- **Q5 — multi-run isolation:** existing per-work-item worktree + source-authority
  TLS holds; **use `git worktree lock` during delegate runs** so worktree-GC
  can't prune an active run; load-validate (N≈100) in Phase C.

Additional adopted refinements: verdict aggregation must distinguish
partial-success from total-failure and log partial commits; patch-coordinator
actions audited + attributed to the work item.

**Plan status: done — implemented.** Filed alongside the (already-closed) proposal
`full-autonomous-development.md`. See **Implementation status** below.

## Implementation status

Phases A/B and the Phase-C *floor* shipped with the proposal itself (closed to `done/`
via **PR #874**, 2026-06-29): the delegate seam + live provider, per-work-item worktree
isolation, the server-owned scheduler, authed intake (`POST /v1/dev/submit`), the
mechanical verify gate, the WP-5 budget/merge-target rails, and the live forge behind
`wfe_live_forge_enabled` (default-OFF).

The three ratified-deferred **Phase-C DEPTH** items (this closeout) are now implemented +
merged to `testing`, each roundtable-reviewed, all **default-safe / opt-in**:

- **Q4 — richer failure taxonomy (PC1, PR #1026).** `wfe_failure_class_t` classifies a
  `WFE_STEP_FAILED` into a retry (LOOPED, only with new input) / terminal-reject /
  park-human / park-stuck disposition. Core invariant: **never auto-retry without new
  input**. Additive + inert for existing executors.
- **Q3 — CI-webhook resume + red-CI retry cap (PC2, PR #1028).** HMAC-signed
  `POST /v1/dev/ci-event` (fail-closed if `AIMEE_CI_WEBHOOK_SECRET` unset) records the CI
  outcome + resumes the scheduler immediately; `exec_gate_ci` bounds the red-CI loop
  per-work-item by `AIMEE_AUTONOMY_CI_RETRY_MAX` → then parks (PC1 DEGRADED). Validated by
  a `.253` runtime smoke (400/401/404/503 + fail-closed).
- **Q2 — N-skeptic adversarial fan-out + patch-coordinator (PC3a #1028, PC3b #1029).**
  A `wfe_judge_provider` seam + `wfe_implement_adversarial_ok` (reviewer + N skeptics,
  accept iff `refutes*2 < K`, even-K tie rejects) gated by `AIMEE_AUTONOMY_SKEPTICS`; a
  live read-only judge provider (worktree-reset-enforced); and an engine-level fan-out
  manager loop (`AIMEE_AUTONOMY_FANOUT`): coordinator decompose → per-unit engineer +
  verify + retry-different-delegate → sequential-commit patch-coordinator → **mandatory
  aggregate verify** → **no silent partial advance** (any unit fail → park).

Clarifications (from the closeout verification roundtable):
- `DEGRADED` is a failure **class** that maps to the **park-human** disposition (the CI
  retry-cap exhaustion parks for a human). `AIMEE_AUTONOMY_SKEPTICS=0` short-circuits the
  adversarial gate to *pass* before the `refutes*2 < K` rule (so K=0 is "no review", never
  a silent block).
- **Aggregate verify** = the same mechanical `git_verify` (format=json, top-level
  `verdict:passed`) run on the whole merged worktree after fan-out — plus the opt-in
  adversarial gate. Fan-out is **sequential** (scheduler concurrency=1): units land as
  successive commits on one branch, so there is no parallel-merge conflict; any
  integration breakage is caught by the aggregate verify → loop/park, never a silent
  partial advance.
- The **live judge is fail-closed**: a missing/unreachable agent, a dispatch error, or an
  unparseable verdict is treated as REFUTED (the gate blocks), mirroring the webhook's
  fail-closed posture; and the judge worktree is hard-reset after each judgment so it
  cannot mutate the change. The CI webhook dedupes on `(status, head_sha)` and the
  scheduler's existing per-work-item single-flight serializes concurrent resumes.

**Carried (ratified GA gates — NOT code I land autonomously):** the real
seccomp/namespace/cgroup execution sandbox (hardware tier); branch protection on the
forge; scoped/rotated forge creds + break-glass; multi-forge; the default-ON of the live
forge; a production live-forge round-trip. These gate operator/GA enablement, not the code
floor + depth shipped here. The fan-out + live-judge tiers are integration-gated (like the
existing live providers) and default-OFF.
