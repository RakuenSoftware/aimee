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
