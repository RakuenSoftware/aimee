/* wfe_engine.h: the workflow execution engine. Drives a pinned workflow graph
 * for a work item through the wfe_iface vtable, holding authoritative state in
 * DB1. Server-side (depends on db1). See wfe_iface.h for the frozen seam. */
#ifndef DEC_WFE_ENGINE_H
#define DEC_WFE_ENGINE_H 1

#include <stddef.h>
#include "wfe_def.h"
#include "wfe_iface.h"

/* Opaque per-step context handed to block executors. */
typedef struct wfe_ctx wfe_ctx;

/* Executor-facing accessors (stable; extend here without touching the seam). */
const char *wfe_ctx_work_item(const wfe_ctx *c);
const wfe_def_t *wfe_ctx_def(const wfe_ctx *c);
const wfe_node_t *wfe_ctx_node(const wfe_ctx *c);
const char *wfe_ctx_repo(const wfe_ctx *c);          /* normalized repo url */
const char *wfe_ctx_proposal_path(const wfe_ctx *c); /* proposal artifact path */
const char *wfe_ctx_pr_ref(const wfe_ctx *c);        /* forge PR ref from pr.open, or "" */
const char *wfe_ctx_worktree(const wfe_ctx *c);      /* per-work-item git worktree, or "" */

/* Load a workflow definition by name from $AIMEE_HOME/workflows/<name>.yaml.
 * Caller frees with wfe_def_free. */
wfe_def_t *wfe_load_workflow(const char *name, char *err, size_t errlen);

/* Mint a server-side, non-forgeable work item, pin its workflow + version, set
 * the start stage. Fills out_id (>= 80 bytes). Returns 0 on success. */
int wfe_work_item_create(const char *workflow_name, const char *repo, const char *proposal_path,
                         const char *mode, char out_id[80], char *err, size_t errlen);

typedef struct
{
   wfe_step_status_t last_status;
   char stage[WFE_ID_LEN];      /* stage that ran */
   char next_stage[WFE_ID_LEN]; /* stage now current ("" if terminal) */
   int terminal;                /* reached a terminal state this step */
   char state[24];              /* active | accepted | rejected | abandoned */
   wfe_pause_reason_t pause_reason;
} wfe_advance_result_t;

/* Advance the work item exactly one step. Returns 0 on a clean step (incl.
 * pause/terminal), -1 on error. */
int wfe_engine_advance(const char *work_item_id, wfe_advance_result_t *out, char *err,
                       size_t errlen);

/* Advance repeatedly until paused, terminal, or failed. Returns 0 on clean stop. */
int wfe_engine_run(const char *work_item_id, char *err, size_t errlen);

/* Register the built-in stub executors (every block -> ADVANCED). Real
 * executors (W3+) override per block type. */
void wfe_register_stub_executors(void);

#endif /* DEC_WFE_ENGINE_H */
