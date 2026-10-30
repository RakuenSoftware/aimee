/* wfe_blocks.h: the non-gate block executors (author/implement/freeze/pr/merge)
 * and a few standalone, unit-testable helpers (git freeze). Registered behind
 * the wfe_iface vtable via wfe_register_default_executors(). */
#ifndef DEC_WFE_BLOCKS_H
#define DEC_WFE_BLOCKS_H 1

#include <stddef.h>
#include <aimee/ir/aimee_ir.h>

/* ---- Forge seam for the safety blocks (gate.ci / check.mergeable / merge).
 * The default provider calls `gh` and is integration-gated; tests inject a mock
 * so the state-mapping + idempotent-merge logic is unit-testable. ---- */
typedef enum
{
   WFE_CI_SUCCESS = 0, /* all checks green */
   WFE_CI_FAILURE,     /* any failed/error/cancelled/timed_out */
   WFE_CI_PENDING,     /* still running */
   WFE_CI_NONE,        /* unknown / unreachable / forge error -> fail closed (park) */
   WFE_CI_NO_CHECKS    /* forge reached, but NO checks configured on this PR's branch
                        * (e.g. an intermediate slice->feature PR CI never targets) ->
                        * nothing to gate on, so advance (mirrors git_pr_ci_permits_merge,
                        * which already treats GIT_PR_CI_NONE as mergeable). Distinct from
                        * WFE_CI_NONE, which is a genuinely-undetermined state. */
} wfe_ci_status_t;

typedef enum
{
   WFE_MERGE_OK = 0,        /* merged now */
   WFE_MERGE_ALREADY,       /* already merged -> idempotent no-op success */
   WFE_MERGE_NOT_MERGEABLE, /* lost race (head/base moved) -> loop; self-resolves */
   WFE_MERGE_CONFLICT,      /* content conflict -> TERMINAL; retrying cannot win */
   WFE_MERGE_ERROR          /* forge error -> fail closed */
} wfe_merge_result_t;

typedef enum
{
   WFE_DELEGATE_ERROR = -1,
   WFE_DELEGATE_OK = 0,
   /* The delegate completed normally but the requested write was already
    * satisfied, so it left the worktree unchanged. Producing blocks may pass
    * the current tree to their authoritative gate instead of retrying blindly. */
   WFE_DELEGATE_NO_CHANGE = 1
} wfe_delegate_result_t;

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
   /* Run the block's delegate work in `workdir` as `role` with `prompt`. Agent
    * selection is the delegate system's routing decision (routed by `role` +
    * capabilities), not a per-step choice. If the block produces a file artifact,
    * `artifact_path` is its path (else NULL). On success returns 0 and, if a commit
    * was made, fills out_commit_sha (else ""). Return WFE_DELEGATE_NO_CHANGE
    * only when the delegate completed but the write-role no-op detector proved
    * the worktree stayed unchanged; return WFE_DELEGATE_ERROR for transport,
    * provider, or execution failures. */
   int (*run)(const char *workdir, const char *role, const aimee_message_t *message,
              const char *artifact_path, char out_commit_sha[64], char *err, size_t errlen);
} wfe_delegate_provider_t;

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

#endif /* DEC_WFE_BLOCKS_H */
