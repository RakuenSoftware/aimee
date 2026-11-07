/* delegate_run_phases.h: phases lifted out of delegate_worker (server_compute.c)
 * into delegate_run_phases.c to keep that file under the line cap. These are the
 * self-contained steps that do not touch the worker's thread-local run state. */
#ifndef DEC_DELEGATE_RUN_PHASES_H
#define DEC_DELEGATE_RUN_PHASES_H 1

#include "client_constants.h"        /* MAX_PATH_LEN (agent_types.h needs it) */
#include "agent.h"                   /* agent_t, agent_result_t, agent_config_t */
#include "cmd_agent_delegate_impl.h" /* DELEGATE_DRIFT_PATH_MAX */

#include <stddef.h>
#include <time.h>

/* mtime + size snapshot of a named path, used to tell whether a write delegate
 * actually changed its owned files. */
typedef struct
{
   int exists;
   time_t mtime_sec;
   long mtime_nsec;
   long long size;
} delegate_file_snapshot_t;

/* Snapshot a path (exists=0 when stat fails). */
delegate_file_snapshot_t delegate_file_snapshot(const char *path);

/* Resolved workspace for a delegate run: where it executes (worktree_path), the
 * managed git root when a sibling worktree was created (git_root, empty for a
 * shared/dedicated worktree), the deterministic sibling work-name, and the flags the
 * caller keys cleanup/apply-back off. */
typedef struct
{
   char worktree_path[MAX_PATH_LEN];
   char git_root[MAX_PATH_LEN];
   char work_name[32];
   int attempted; /* a sibling worktree creation was attempted */
   int shared;    /* the delegate shares the parent/session worktree */
   int dedicated; /* the delegate EXCLUSIVELY owns this pre-existing worktree (a WFE
                   * per-slice tree, $AIMEE_HOME/wfe-worktrees/wi_<id>). Isolation
                   * does not grant writes: the resolved role still controls the
                   * container mount. Distinct from a sibling worktree (which
                   * has a git_root for apply-back) and from legacy parent sharing. */
} delegate_worktree_t;

/* Resolve the managed root and, when needs_worktree is set, create an isolated
 * sibling or retain an exclusively owned WFE tree. A new sibling also becomes
 * the thread-local run cwd. The current server requests isolation for every
 * repository role; mount write permission remains a separate role decision.
 * The legacy allows_writes && !needs_worktree mode shares the parent and clears
 * git_root to suppress sibling cleanup/apply-back. No-op with no git root. */
void delegate_resolve_worktree(const char *cwd, const char *deleg_id, const char *branch,
                               int allows_writes, int needs_worktree, delegate_worktree_t *out);

/* Persist operational IE_DELEGATE_EXIT metrics only. Reusable experience is
 * derived exclusively from protected canonical child/parent source records.
 * Returns -1 on metric persistence failure; never alters the execution result. */
int delegate_record_exit_metrics(const char *sid, const char *role, const agent_result_t *result,
                                 int rc, int max_turns, agent_config_t *acfg, agent_t *target_agent,
                                 const char *source_ref);

/* Detect a no-op write delegate (success reported but owned files / worktree /
 * HEAD unchanged). Returns 1 and fills err with the "treated as incomplete"
 * message; 0 otherwise (logging INFO for the benign cases). */
int delegate_detect_noop_write(int writes_allowed, int handoff_json, int rc,
                               const char named_paths[][DELEGATE_DRIFT_PATH_MAX],
                               int named_path_count, const delegate_file_snapshot_t *pre_run_files,
                               const char *pre_run_head_sha, const char *worktree_path,
                               const char *cwd, const char *deleg_id, const char *sid,
                               const char *role, char *err, size_t errsz);

#endif /* DEC_DELEGATE_RUN_PHASES_H */
