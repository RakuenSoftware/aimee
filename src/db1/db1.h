/* db1.h: DB1 (user-local tier) — public umbrella header.
 *
 * Code outside src/db1/ never includes sqlite3.h, never sees a handle,
 * never constructs SQL. It calls typed domain functions declared in the
 * per-subsystem headers listed below, and handles return codes.
 *
 * Lifecycle: db1_init() is called once at startup with the DB1 file path.
 * db1_shutdown() is called at exit. The connection and all per-subsystem
 * state are private to src/db1/.
 */
#ifndef DEC_DB1_H
#define DEC_DB1_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Open DB1 at the given path. Applies schema. Returns 0 on success. */
   int db1_init(const char *path);

   /* Close DB1. Safe to call if not initialized, or more than once. */
   void db1_shutdown(void);

   /* Returns 1 if db1_init has succeeded and the connection is live, 0 otherwise. */
   int db1_is_initialized(void);

   /* Apply aimee-server-mode pragmas (larger cache, mmap, wal_autocheckpoint).
    * Called once after db1_init in the server process. No-op if db1 is not
    * initialized. */
   void db1_apply_server_pragmas(void);

   /* Subsystem APIs owned by DB1. */
#include "wm.h"
#include "fsnap.h"
#include "clarify.h"
#include "diagnose.h"
#include "workflow_session.h"
#include "eval.h"
#include "caches.h"
#include "primary_sessions.h"
#include "webchat_claude_sessions.h"
#include "env.h"
#include "delegations.h"
#include "delegation_checkpoint.h"
#include "checkpoints.h"
#include "decisions.h"
#include "runtime_state.h"
#include "server_sessions.h"
#include "session_state.h"
#include "session_paths.h"
#include "cost_fold.h"
#include "token_audit.h"
#include "agent_log.h"
#include "agent_jobs.h"
#include "coord_jobs.h"
#include "work_queue.h"
#include "execution_plans.h"
#include "pipelines.h"
#include "secrets.h"
#include "cognify_jobs.h"
#include "local_operator.h"
#include "project_clones.h"
#include "git_ownership.h"
#include "tool_local_availability.h"
#include "model_catalog.h"
#include "execution_trace.h"
#include "working_profile_local.h"
#include "db1_windows.h"
#include "diagnostics.h"
#include "maintenance.h"
#include "guardrail_events.h"
#include "db1_trigger.h"
#include "mcp_osv_cache.h"

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_H */
