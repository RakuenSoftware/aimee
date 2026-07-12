/* wfe_blocks.h: the non-gate block executors (author/implement/freeze/pr/merge)
 * and a few standalone, unit-testable helpers (git freeze). Registered behind
 * the wfe_iface vtable via wfe_register_default_executors(). */
#ifndef DEC_WFE_BLOCKS_H
#define DEC_WFE_BLOCKS_H 1

#include <stddef.h>

/* Register the real executors for every non-gate block type. Gates are
 * registered by their own slices (W4 human, W5 roundtable). */
void wfe_register_default_executors(void);

/* Compute a frozen diff for the implementation gate. base_ref defaults to the
 * merge-base of HEAD against `base_branch` (e.g. "origin/testing" or "main").
 * Fills the short SHAs and the sha256 hex of the cumulative diff. Returns 0 on
 * success. Standalone (no engine ctx) so it is directly unit-testable. */
int wfe_git_freeze(const char *repo_dir, const char *base_branch, char out_base_sha[64],
                   char out_head_sha[64], char out_diff_hash[65], char *err, size_t errlen);

/* ---- Forge seam for the safety blocks (gate.ci / check.mergeable / merge).
 * The default provider calls `gh` and is integration-gated; tests inject a mock
 * so the state-mapping + idempotent-merge logic is unit-testable. ---- */
typedef enum
{
   WFE_CI_SUCCESS = 0, /* all checks green */
   WFE_CI_FAILURE,     /* any failed/error/cancelled/timed_out */
   WFE_CI_PENDING,     /* still running */
   WFE_CI_NONE         /* no checks / unknown / unreachable -> fail closed */
} wfe_ci_status_t;

typedef enum
{
   WFE_MERGE_OK = 0,        /* merged now */
   WFE_MERGE_ALREADY,       /* already merged -> idempotent no-op success */
   WFE_MERGE_NOT_MERGEABLE, /* conflict / lost race -> loop */
   WFE_MERGE_ERROR          /* forge error -> fail closed */
} wfe_merge_result_t;

typedef struct
{
   wfe_ci_status_t (*ci_status)(const char *repo, const char *pr_ref);
   int (*mergeable)(const char *repo, const char *pr_ref); /* 1 yes, 0 conflict, -1 unknown */
   int (*is_merged)(const char *repo, const char *pr_ref); /* 1 merged, 0 open, -1 unknown */
   wfe_merge_result_t (*merge)(const char *repo, const char *pr_ref);
   /* Push `branch` and open a PR AGAINST `base` (the target branch: the autonomous
    * base for a top-level PR, or the parent feature branch aimee/feat/<parent> for a
    * slice sub-PR); write its forge ref (number/url) into out_pr_ref. `base` is never
    * empty and is guaranteed non-protected by the caller. Returns 0 on success, -1 on
    * failure. May be NULL on a provider that predates this field -> pr.open fails
    * closed and the gate that follows re-loops. */
   int (*open)(const char *repo, const char *branch, const char *base, const char *title,
               const char *body, char out_pr_ref[128]);
   /* Publish a durable feature branch to the forge (push the local aimee/feat/<id>
    * branch so slice sub-PRs can target it as their base). Returns 0 on success, -1
    * on failure. May be NULL (predates this field / no live forge) -> branch.open
    * still produces the branch name, but the base won't exist remotely until a live
    * provider is installed, so slice sub-PRs fail closed. */
   int (*publish_base)(const char *repo, const char *branch);
} wfe_forge_t;

/* Install a forge provider (NULL restores the default live/gh provider). */
void wfe_set_forge_provider(const wfe_forge_t *p);

/* ---- Delegate seam for the producing blocks (author/implement/document).
 * The wfe library must not depend on server/ delegate internals
 * (module-boundary-check), so blocks dispatch real delegate work through this
 * registered hook. The default provider is NULL -> producing blocks fail closed
 * (the gate that follows re-loops), exactly the pre-seam integration-gated
 * behavior. The server registers a live provider that runs delegates (decompose
 * -> fan out -> verify -> re-delegate is owned by the live provider; see the
 * full-autonomous-development plan). Tests inject a mock. ---- */
typedef struct
{
   /* Run the block's delegate work in `workdir` as `role` with `prompt`.
    * `delegate` is the step's assigned agent name, the sentinel "$random" (resolve
    * to a random roster agent), or "" to route by `role`. If the block produces a
    * file artifact, `artifact_path` is its path (else NULL). On success returns 0
    * and, if a commit was made, fills out_commit_sha (else ""). Non-zero => the
    * caller emits failed/looped, never a crash. */
   int (*run)(const char *workdir, const char *role, const char *delegate, const char *prompt,
              const char *artifact_path, char out_commit_sha[64], char *err, size_t errlen);
} wfe_delegate_provider_t;

/* Install a delegate provider (NULL restores the default fail-closed provider). */
void wfe_set_delegate_provider(const wfe_delegate_provider_t *p);

/* ---- Mechanical verify seam for the implement manager loop (WP-1b).
 * implement only advances a unit that PASSES the mechanical gate. The wfe library
 * must not call the server's git_verify directly (module-boundary-check), so it
 * runs verification through this registered hook, which returns the structured
 * (git_verify format=json) verdict. The server registers a live provider that
 * calls handle_git_verify; tests inject a mock. Default NULL -> implement skips
 * verification (the pre-WP-1b behavior), so the engine stays drivable without a
 * provider. ---- */
typedef struct
{
   /* Run the mechanical verify gate on `workdir`, writing the structured verdict
    * JSON (the git_verify format=json document) into out_verdict. Returns 0 if a
    * verdict was produced, -1 if the gate could not run at all (treated as a
    * non-pass — fail closed). */
   int (*verify)(const char *workdir, char *out_verdict, size_t n);
} wfe_verify_provider_t;

/* Install a verify provider (NULL = implement does not gate on verification). */
void wfe_set_verify_provider(const wfe_verify_provider_t *p);

/* ---- Adversarial judge seam for the implement manager loop (PC3 / Q2).
 * On top of the mechanical verify, implement can run a REVIEW + N adversarial
 * SKEPTIC judgments of the produced change. The wfe library cannot dispatch judge
 * delegates itself (module-boundary), so it runs them through this registered hook.
 * The server registers a live provider that dispatches a reviewer/skeptic delegate
 * and returns its verdict; tests inject a mock. Config-gated (AIMEE_AUTONOMY_SKEPTICS,
 * default 0 = OFF) so default behavior is unchanged; when ON and no provider is
 * installed, the tier FAILS CLOSED (refuted). ---- */
typedef struct
{
   /* Judge the change in `workdir` under `lens` ("reviewer" | "skeptic") — the
    * skeptic lens is prompted to REFUTE. Writes a verdict JSON {"refuted":bool,...}
    * into out_verdict. Returns 0 if a verdict was produced, -1 if the judge could not
    * run (treated as REFUTED — fail closed). */
   int (*judge)(const char *workdir, const char *lens, char *out_verdict, size_t n);
} wfe_judge_provider_t;

/* Install a judge provider (NULL restores the default fail-closed provider). */
void wfe_set_judge_provider(const wfe_judge_provider_t *p);

/* The implement ADVERSARIAL gate (PC3): with AIMEE_AUTONOMY_SKEPTICS=K>0, run a
 * reviewer + K skeptic judgments; accept (1) only if the reviewer did not refute AND
 * FEWER THAN HALF the skeptics refuted (an exact tie for even K REJECTS — safety bias;
 * i.e. accept iff refutes*2 < K). K==0 (a valid decimal >= 0, else a WARN + off) -> 1
 * (tier off, unchanged behavior). K>0 with no judge provider / an unrunnable /
 * unparseable / non-boolean verdict -> 0 (fail closed). Exposed for the unit test. */
int wfe_implement_adversarial_ok(const char *workdir);

/* The implement verify gate: 1 = advance ONLY when the top-level verdict is an
 * explicit "passed"; 0 = block in every other case (no provider, gate-unrunnable,
 * unparseable, or any non-pass verdict) -> FAIL CLOSED. Exposed for the unit test. */
int wfe_implement_verify_ok(const char *workdir);

/* The TDD RED gate: after the test author commits (implement node `tdd: true`), a
 * genuine red step must leave at least one FAILING test — the change must NOT
 * already pass. Returns 1 to proceed to the GREEN (implementer) step, 0 to loop
 * (re-run the test author). Cases:
 *   - no verify provider installed -> 1 (cannot enforce mechanically; proceed, the
 *     same drivable-without-a-provider stance as the rest of implement),
 *   - a produced verdict that is anything OTHER than "passed" -> 1 (a real red),
 *   - an explicit "passed" verdict (tests pass / none were added), a gate that
 *     could not run, or an unparseable verdict -> 0 (red unconfirmed). It is the
 *     logical inverse of wfe_implement_verify_ok in the provider-present cases.
 * Exposed for the unit test. */
int wfe_tdd_red_ok(const char *workdir);

/* TDD anti-deletion guard: after the GREEN implementer commits, every file the RED
 * commit `red_sha` ADDED or MODIFIED (its tests) must still exist at HEAD — GREEN
 * makes them pass, it does not delete them. Returns 1 if all survive OR the check
 * is not applicable (empty args, or git cannot resolve the commit — the mandatory
 * freeze + aggregate verify remain the backstop), 0 if GREEN deleted a red-authored
 * file. Best-effort by design so a non-git/degraded workdir stays drivable. Exposed
 * for the unit test. */
int wfe_tdd_tests_survive(const char *workdir, const char *red_sha);

/* ---- Child-workflow fan-out seam for foreach.workflow (sliced-lifecycle build).
 * The block decomposes the split packets into one CHILD workflow run per packet
 * (the "slice" workflow: implement -> freeze -> roundtable -> sub-PR -> green CI ->
 * merge into the feature branch). SPAWNING children is DB + autonomy-driver territory
 * (a module-boundary concern), so it runs through this hook; the AGGREGATION (are all
 * slices merged?) lives in the executor, keyed off the DB parent<->child linkage
 * (db1_work_item_child_counts), so the fan-in logic is unit-testable. The default
 * provider is NULL -> foreach.workflow PARKS pending_human (fail closed: no child is
 * spawned and nothing silently advances). The server registers a live spawner; tests
 * inject a mock that creates child rows. ---- */
typedef struct
{
   /* Create one child `child_workflow` run per packet in the split packet-plan at
    * `packets_path` (the parent's `.wfe-<split>.json`; may be NULL/absent -> 0
    * children), each linked to `work_item_id` via db1_work_item_set_parent and
    * targeting its feature branch. `max_children` bounds the fan-out (a pathological/
    * hostile packet list). Idempotent: if children already exist it must not
    * double-spawn. Returns the number of children that exist after the call (0 = no
    * packets -> nothing to do), or -1 on a fatal error (fills `err`). */
   int (*spawn)(const char *work_item_id, const char *child_workflow, const char *packets_path,
                int max_children, char *err, size_t errlen);
} wfe_foreach_provider_t;

/* Install a child-workflow spawn provider (NULL restores the default park). */
void wfe_set_foreach_provider(const wfe_foreach_provider_t *p);

/* Register only the foreach.workflow executor (test seam: stub the rest, drive the
 * real fan-in aggregation). */
void wfe_register_foreach_block(void);

/* ---- per-work-item git worktree isolation (F2) ----
 * Create/return a locked per-work-item worktree (aimee/wi/<id>) so concurrent
 * autonomous runs don't share one checkout; tear it down on terminal. Exposed for
 * the unit test. ensure() returns 0 + out_path on success, -1 on any failure (the
 * caller falls back to the shared repo dir). */
int wfe_worktree_ensure(const char *work_item_id, const char *existing, const char *repo_local,
                        const char *base, char *out_path, size_t n);
int wfe_worktree_cleanup(const char *worktree, const char *repo_local);

/* Orphan GC: reap wfe-worktrees/<id> dirs that no LIVE (non-terminal) work item
 * owns — a vanished row (deleted item) or a terminal row whose terminal-cleanup
 * was missed would otherwise strand a full git worktree (~thousands of inodes)
 * forever. Only dirs older than `grace_secs` are reaped (grace_secs <= 0 reaps
 * immediately), so an in-flight worktree mid-creation is never raced. Force-removes
 * the worktree + its branch + lock, prunes git's admin refs, and clears any stale
 * DB worktree column. Returns the number reaped. Belt-and-suspenders to the
 * terminal-cleanup path (wfe_autonomy_cleanup_worktree / scheduler sweep). */
int wfe_worktree_orphan_gc(const char *repo_local, long grace_secs);

#endif /* DEC_WFE_BLOCKS_H */
