/* wfe_iface.h -- the NARROW frozen execution seam for the workflow engine.
 *
 * This header freezes ONLY what the engine itself switches on: the block
 * executor vtable and the wfe_step_result discriminated union. Gate-internal
 * types (verdicts, panel specs, signed approvals) deliberately live in their
 * own slice headers (wfe_verdict.h, wfe_approval.h) and may grow freely
 * without touching this contract -- so adding a verdict field in a later
 * slice never breaks the W1 contract test.
 *
 * See docs/proposals/pending/aimee-dev-lifecycle-workflow.{md,plan.md}.
 */
#ifndef DEC_WFE_IFACE_H
#define DEC_WFE_IFACE_H 1

#include <stddef.h>

/* ---- Closed artifact type system (handles carry one of these) ---- */
typedef enum
{
   WFE_ART_NONE = 0, /* no artifact (e.g. author.proposal's user-overview input) */
   WFE_ART_PROPOSAL,
   WFE_ART_PLAN,
   WFE_ART_BRANCH,
   WFE_ART_FROZEN_DIFF,
   WFE_ART_PR,
   WFE_ART_VERDICT,
   WFE_ART_APPROVAL,
   /* primary-as-manager (S0): understand emits a scoped-intent record. split
    * reuses WFE_ART_PLAN (a delegate packet-plan IS what `implement` fans out),
    * so it composes with the existing implement block -- no new artifact. */
   WFE_ART_INTENT, /* understand: a scoped-intent record (what/why/acceptance) */
   WFE_ART__COUNT
} wfe_artifact_type_t;

/* ---- Block catalog (the composable parts; full vocabulary, frozen in W1) ---- */
typedef enum
{
   WFE_BLK_UNKNOWN = 0,
   WFE_BLK_AUTHOR_PROPOSAL,
   WFE_BLK_AUTHOR_PLAN,
   WFE_BLK_IMPLEMENT,
   WFE_BLK_DOCUMENT,
   WFE_BLK_FREEZE,
   WFE_BLK_GATE_ROUNDTABLE,
   WFE_BLK_GATE_HUMAN,
   WFE_BLK_PR_OPEN,
   WFE_BLK_MERGE,
   WFE_BLK_GATE_CI,         /* built-in: poll the PR's CI, fail-closed */
   WFE_BLK_CHECK_MERGEABLE, /* built-in: refuse on a merge conflict */
   WFE_BLK_CUSTOM,          /* config-defined block (spec carried on the node) */
   /* primary-as-manager (S0): the interactive manager loop + delivery gate. */
   WFE_BLK_UNDERSTAND,   /* primary scopes intent WITH the user -> intent */
   WFE_BLK_SPLIT,        /* primary decomposes intent into delegate packets */
   WFE_BLK_REVIEW,       /* primary reviews delegate output (read-only) -> verdict */
   WFE_BLK_GATE_DELIVER, /* terminal enforcement gate: only crossable once the
                          * upstream review + roundtable have passed */
   /* sliced-lifecycle build workflow (appended, enum-stable):
    * branch.open opens a durable feature branch that per-slice sub-PRs target and
    * merge into; foreach.workflow fans each split packet out to a child "slice"
    * workflow run and parks the parent until every child is terminal. */
   WFE_BLK_BRANCH_OPEN,      /* open/return a durable feature branch -> branch */
   WFE_BLK_FOREACH_WORKFLOW, /* per-packet child workflow fan-out -> branch */
   /* Retire the run's triggering source file (trigger-first lifecycle): move the
    * watched file that filed this run (content-addressed by the materialized
    * artifact's blob-sha name) from its watch dir to a done dir, committed on the
    * run branch so the final PR retires the trigger input atomically with the
    * work. No-op advance for runs without a content-addressed trigger artifact. */
   WFE_BLK_SOURCE_ARCHIVE, /* branch -> branch */
   WFE_BLK__COUNT
} wfe_block_type_t;

/* ---- Engine-facing step result (the ONLY struct the contract test pins) ---- */
typedef enum
{
   WFE_STEP_ADVANCED = 0, /* block produced its output; follow `next`/`on_pass` */
   WFE_STEP_PENDING,      /* parked; see pause_reason; resume re-runs this node */
   WFE_STEP_FAILED,       /* hard failure; engine records and stops */
   WFE_STEP_LOOPED        /* gate REQUEST_CHANGES etc.; follow `on_fail` loop-back */
} wfe_step_status_t;

/* Stable tagged enum -- NEVER a free-form string, so the narrow seam cannot
 * silently re-acquire a surface. */
typedef enum
{
   WFE_PAUSE_NONE = 0,
   WFE_PAUSE_PENDING_HUMAN,
   WFE_PAUSE_PANEL_DEGRADED,
   WFE_PAUSE_BUDGET_EXCEEDED, /* the dollar cost cap (WFE_STEP: real spend ceiling) */
   WFE_PAUSE_PANEL_UNREACHABLE,
   WFE_PAUSE_CI_PENDING,    /* gate.ci: CI still running / not yet conclusive */
   WFE_PAUSE_MERGE_PENDING, /* check.mergeable / merge: forge merge state could not
                             * be determined (transient/unknown) -- re-drive later */
   WFE_PAUSE_TURN_CAP,      /* autonomy runaway backstop: cumulative audit-event
                             * (turn) cap reached -- NOT a spend issue */
   WFE_PAUSE_WALL_CAP       /* autonomy runaway backstop: this resume exceeded its
                             * wall-clock ceiling -- NOT a spend issue */
} wfe_pause_reason_t;

/* Failure taxonomy (Phase-C Q4): how the autonomy run loop should react to a
 * WFE_STEP_FAILED result. The core invariant is NEVER auto-retry without genuinely
 * NEW input (a CI log / verify findings / a roundtable verdict) — a bare re-dispatch
 * of the same prompt is not a retry, it is a spin, so it parks instead. */
typedef enum
{
   WFE_FAIL_NONE = 0,  /* unclassified -> treated as PERMANENT (conservative: stop) */
   WFE_FAIL_TRANSIENT, /* retryable IFF has_new_input, else park stuck */
   WFE_FAIL_REFUSAL,   /* model/agent refused -> terminal-reject */
   WFE_FAIL_PERMANENT, /* a permanent error -> terminal-reject */
   WFE_FAIL_DEGRADED,  /* roundtable/panel degraded / exhausted retries -> park human */
   WFE_FAIL_BUDGET,    /* budget breach -> park human */
   WFE_FAIL_FORGE,     /* git/forge op failed -> park human */
   WFE_FAIL_CORRUPTION /* worktree/tree corruption -> terminal */
} wfe_failure_class_t;

typedef struct
{
   wfe_step_status_t status;
   wfe_pause_reason_t pause_reason; /* meaningful iff status == WFE_STEP_PENDING */
   int reopen;                      /* gate re-open signal (bound hash changed) */
   char artifact_handle[64];        /* produced-artifact handle id, or "" */
   char content_hash[65];           /* sha256 hex of produced artifact, or "" */
   double cost_usd;                 /* cost incurred by this step */
   /* Phase-C failure taxonomy (meaningful iff status == WFE_STEP_FAILED). Default 0
    * (WFE_FAIL_NONE, no new input) preserves the pre-taxonomy "stop" behavior for any
    * executor that has not been taught the classes. */
   wfe_failure_class_t failure_class;
   int failure_has_new_input; /* 1 iff a TRANSIENT failure carries genuinely new input */
} wfe_step_result_t;

/* ---- Executor vtable: gates ARE ordinary block executors (one call-site) ---- */
struct wfe_ctx;  /* opaque engine context; defined by the engine slice (W2) */
struct wfe_node; /* a graph node; defined in wfe_def.h */

typedef wfe_step_result_t (*wfe_block_exec_fn)(struct wfe_ctx *ctx, const struct wfe_node *node);

/* Register / look up the executor for a block type. W2 registers stubs; later
 * slices register real executors WITHOUT editing the engine. */
void wfe_register_block_executor(wfe_block_type_t type, wfe_block_exec_fn fn);
wfe_block_exec_fn wfe_lookup_block_executor(wfe_block_type_t type);
void wfe_reset_block_executors(void); /* test helper */

/* Convenience for building results. */
wfe_step_result_t wfe_step_advanced(const char *artifact_handle, const char *content_hash,
                                    double cost_usd);
wfe_step_result_t wfe_step_pending(wfe_pause_reason_t reason);
wfe_step_result_t wfe_step_failed(void);
wfe_step_result_t wfe_step_looped(void);

/* A classified failure (Phase-C Q4). has_new_input is honored only for
 * WFE_FAIL_TRANSIENT (retry vs park-stuck); ignored for the other classes. */
wfe_step_result_t wfe_step_failed_class(wfe_failure_class_t cls, int has_new_input);

/* Map a failure class to the run-loop disposition, so the routing lives in one
 * place (exposed for the unit test). */
typedef enum
{
   WFE_FDISP_TERMINAL = 0, /* cleanup + stop (terminal-reject / corruption) */
   WFE_FDISP_PARK_HUMAN,   /* park pending_human (degraded / budget / forge) */
   WFE_FDISP_RETRY,        /* loop back with new input (transient + new input) */
   WFE_FDISP_PARK_STUCK    /* transient without new input -> park stuck (no spin) */
} wfe_failure_disposition_t;
wfe_failure_disposition_t wfe_failure_disposition(wfe_failure_class_t cls, int has_new_input);

/* Resolve the local working repo for a work item from its stored `repo` column:
 * the repo itself when it names a local directory (a trigger rule's
 * pipeline.workspace binds the run to that repository), else the process-wide
 * $AIMEE_WORKFLOW_REPO, else cwd. Every per-work-item repo consumer (block
 * executors, roundtable workdir, worktree cleanup) resolves through this so a
 * run filed against a specific workspace executes there, not wherever the
 * server process happens to sit. */
const char *wfe_repo_local(const char *wi_repo);

/* ---- Autonomous merge-target rail (WP-5 safety) ----
 * The single source of truth for the branch an autonomous run may target for a
 * PR/merge. Autonomous merges only ever go to this branch (default "testing");
 * promotion to a protected branch (main/master/release*) stays a human action.
 * Overridable via AIMEE_AUTONOMY_BASE, but NEVER to a protected branch — the guard
 * refuses and pr.open/merge fail closed, so a misconfiguration can't reach main.
 * F4's live forge resolves its PR base + merge target through these. */
const char *wfe_autonomous_base(void);
int wfe_base_is_protected(const char *branch); /* 1 if main/master/release* (refused) */
int wfe_autonomous_target_ok(void);            /* 1 if the configured base is mergeable */

/* Authoritative server-side USD cost for a delegate turn of `elapsed_secs`
 * wall-clock (rate AIMEE_AUTONOMY_USD_PER_SEC, default 0.0005). Provider-agnostic,
 * so the per-run budget cap is enforced on a figure the provider can't understate.
 * Negative elapsed -> 0. */
double wfe_autonomy_cost_estimate(double elapsed_secs);

#endif /* DEC_WFE_IFACE_H */
