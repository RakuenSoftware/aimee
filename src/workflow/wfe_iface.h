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
   WFE_PAUSE_BUDGET_EXCEEDED,
   WFE_PAUSE_PANEL_UNREACHABLE,
   WFE_PAUSE_CI_PENDING,   /* gate.ci: CI still running / not yet conclusive */
   WFE_PAUSE_MERGE_PENDING /* check.mergeable / merge: forge merge state could not
                            * be determined (transient/unknown) -- re-drive later */
} wfe_pause_reason_t;

typedef struct
{
   wfe_step_status_t status;
   wfe_pause_reason_t pause_reason; /* meaningful iff status == WFE_STEP_PENDING */
   int reopen;                      /* gate re-open signal (bound hash changed) */
   char artifact_handle[64];        /* produced-artifact handle id, or "" */
   char content_hash[65];           /* sha256 hex of produced artifact, or "" */
   double cost_usd;                 /* cost incurred by this step */
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

#endif /* DEC_WFE_IFACE_H */
