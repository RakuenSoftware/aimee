/* Worktree garbage collection.
 *
 * Session worktrees live under `<git_root>/.aimee/worktrees/<sid>/<name>`.
 * They are temporary by design — once a session ends and its branch is
 * merged (or the work is abandoned), the worktree is dead weight on disk
 * and clutters `git worktree list`. This module finds those candidates
 * and removes them.
 *
 * Safety: never removes a worktree with uncommitted changes. Without
 * `--force`, never removes a worktree with commits ahead of the base
 * branch. Operator-driven via `aimee worktree gc`; auto-driven by a
 * daily startup pass when `worktree_gc.enabled` is set. */

#ifndef AIMEE_WORKTREE_GC_H
#define AIMEE_WORKTREE_GC_H

#include "aimee.h"
#include <time.h>

#define WORKTREE_GC_MAX_CANDIDATES 256
#define WORKTREE_GC_REASON_LEN     192
#define WORKTREE_GC_BRANCH_LEN     192

typedef struct
{
   int max_age_days; /* skip worktrees touched more recently than this */
   int force;        /* remove even when commits are ahead of base branch */
   int dry_run;      /* never call worktree remove; report only */
} worktree_gc_options_t;

typedef struct
{
   char path[MAX_PATH_LEN];
   char branch[WORKTREE_GC_BRANCH_LEN];
   char reason[WORKTREE_GC_REASON_LEN]; /* why this is / isn't a candidate */
   time_t last_activity;                /* max of HEAD commit time and dir mtime */
   int commits_ahead;                   /* commits ahead of base branch (-1 = unknown) */
   int has_uncommitted;                 /* dirty working tree */
   int eligible;                        /* 1 = safe to remove under current opts */
} worktree_gc_candidate_t;

/* Initialize options to safe defaults: 14d, no force, no dry-run. */
void worktree_gc_options_init(worktree_gc_options_t *opts);

/* Walk `<git_root>/.aimee/worktrees/<sid>/<name>` and populate `out` with
 * one candidate per discovered worktree. Returns the count, or -1 on
 * error. Skips entries that aren't directories. Each candidate's
 * `eligible` field reflects the current `opts`. */
int worktree_gc_scan(const char *git_root, const worktree_gc_options_t *opts,
                     worktree_gc_candidate_t *out, int max_out);

/* Remove every eligible candidate via the existing `worktree_cleanup`
 * primitive (which itself refuses on uncommitted / unpushed work). For a
 * candidate that was fully merged (0 commits ahead of base), its now-orphaned
 * branch is deleted too, so merged branches don't accumulate; a candidate
 * removed only under `--force` (commits ahead) keeps its branch.
 * Returns the count actually removed. When `opts->dry_run` is set,
 * returns the count that would be removed without touching anything. */
int worktree_gc_apply(const char *git_root, const worktree_gc_candidate_t *cands, int n,
                      const worktree_gc_options_t *opts);

#endif /* AIMEE_WORKTREE_GC_H */
