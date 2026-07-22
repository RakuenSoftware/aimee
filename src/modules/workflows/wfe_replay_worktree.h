/* wfe_replay_worktree.h: a worktree-grounded evidence-replay backend for the
 * wfe roundtable gate.
 *
 * The process-wide replay backend grounds panelist evidence against the CODE
 * INDEX — correct for the compute roundtable, which reviews indexed code, but
 * wrong for a wfe gate: the index lags the change under review, so a factual
 * claim about NEW code would be falsely CONTRADICTED. This backend grounds the
 * same structured queries (symbol / callers / lexical search) against the gate's
 * WORKTREE instead — the exact tree the panel reviewed.
 *
 * The replay_backend_t vtable carries no context pointer, so the worktree root
 * is THREAD-LOCAL: set it around the verification call on the gate executor's
 * thread (the same idiom as run_cmd_set_cwd). With no root set, project_count
 * reports 0 and every item DEGRADES (kept, unverified) — never penalized. */
#ifndef DEC_WFE_REPLAY_WORKTREE_H
#define DEC_WFE_REPLAY_WORKTREE_H 1

#include "evidence_replay.h"

/* Set (or clear, with NULL/"") this thread's worktree root for the backend. */
void wfe_replay_worktree_set_root(const char *root);

/* The worktree-grounded backend (static storage; safe to pass anywhere a
 * replay_backend_t* is expected for the life of the process). */
const replay_backend_t *wfe_replay_worktree_backend(void);

#endif /* DEC_WFE_REPLAY_WORKTREE_H */
