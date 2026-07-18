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
 * shared worktree), the deterministic sibling work-name, and the two flags the
 * caller keys cleanup/apply-back off. */
typedef struct
{
   char worktree_path[MAX_PATH_LEN];
   char git_root[MAX_PATH_LEN];
   char work_name[32];
   int attempted; /* a sibling worktree creation was attempted */
   int shared;    /* the delegate shares the parent/session worktree */
} delegate_worktree_t;

/* Decide and (for write delegates that need isolation) create the delegate's
 * worktree. A read-only or foreground sole-writer delegate shares the parent
 * worktree (git_root cleared so apply-back/sibling-cleanup are skipped); a
 * concurrent/branch write delegate gets an isolated sibling worktree and this
 * sets the thread-local run cwd to it. No-op (all-empty out) with no git root. */
void delegate_resolve_worktree(const char *cwd, const char *deleg_id, const char *branch,
                               int allows_writes, int needs_worktree, delegate_worktree_t *out);

/* Classify a finished delegate run and persist a delegate_learning row + a
 * structured IE_DELEGATE_EXIT event. Pure post-run side effect. */
void delegate_record_exit_learning(const char *sid, const char *role, const agent_result_t *result,
                                   int rc, int max_turns, agent_config_t *acfg,
                                   agent_t *target_agent);

/* Detect a no-op write delegate (success reported but owned files / worktree /
 * HEAD unchanged). Returns 1 and fills err with the "treated as incomplete"
 * message; 0 otherwise (logging INFO for the benign cases). */
int delegate_detect_noop_write(int is_write_role, int allows_writes, int handoff_json, int rc,
                               const char named_paths[][DELEGATE_DRIFT_PATH_MAX],
                               int named_path_count, const delegate_file_snapshot_t *pre_run_files,
                               const char *pre_run_head_sha, const char *worktree_path,
                               const char *cwd, const char *deleg_id, const char *sid,
                               const char *role, char *err, size_t errsz);

#endif /* DEC_DELEGATE_RUN_PHASES_H */
