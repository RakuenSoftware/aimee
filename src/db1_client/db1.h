/* db1.h: the store's C client — public umbrella header.
 *
 * Callers never see a database. They call typed domain functions declared in
 * the per-subsystem headers listed below, each of which is a bus request to the
 * store module, and handle return codes.
 *
 * WHAT THIS HEADER USED TO SAY, because the difference is the whole point: it
 * described code outside src/db1/ never including sqlite3.h, and a lifecycle
 * where db1_init() opened a file at a path and db1_shutdown() closed it. None
 * of that survives. src/db1/ is deleted, the store is PostgreSQL reached over
 * the bus, and no process on this side of the wire holds a connection.
 *
 * READINESS IS NOT A LOCAL FACT ANY MORE, which is the practical consequence
 * for anyone porting old code. A caller that used to guard on db1_init()
 * succeeding wants db1_store_ready(); a local open succeeding never said
 * anything about whether the store would answer, and now there is no local open
 * at all. See the two readiness calls below for which to use where.
 *
 * THIS CLIENT FLATTENS THE MODULE'S STATUSES, and a caller that needs the
 * difference will not find it here. The module answers five:
 *
 *   OK  MISSING  INVALID  TOO_LONG  FAILED
 *
 * and 303 call sites in this directory reduce every one of them to a single
 * failure return:
 *
 *   if (wire_status != (int)AIMEE_DB1_STATUS_OK) return -1;
 *
 * Only 16 sites mention MISSING at all. So "this row does not exist" and "the
 * store did not answer" arrive identically, and a caller cannot tell an absent
 * thing from a broken one. The consequences differ sharply -- absent is a fact
 * to record, unreachable is a reason to retry -- so anything that must
 * distinguish them needs the status word, not this return code.
 *
 * The Go caller-side does NOT have this problem: server-go/aimee's DecodeFields
 * returns the status alongside the fields, so a Go consumer sees MISSING
 * distinctly. This is a property of the C client, not of the wire.
 *
 * Not repaired here. Widening 303 returns means deciding, per caller, what it
 * should now do with a distinction it has never had, and a mechanical change
 * that gave them all a new return code would silently reclassify every existing
 * `if (rc != 0)` -- which is the same flattening again, in the other direction.
 *
 * The lifecycle entry points below (db1_init, db1_shutdown,
 * db1_is_initialized, db1_apply_server_pragmas) are RESIDUE. db1_init.c went
 * with the C module and was deliberately not replaced -- src/Makefile says why:
 * leaving the symbol linked would keep the daemon one call away from a new
 * direct caller. Only src/tests/support/db1_init_mock.c defines them now, and
 * of the four only db1_shutdown() still has callers, all of which are
 * exit paths where it is a no-op. Do not add new ones.
 */
#ifndef DEC_DB1_H
#define DEC_DB1_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Open DB1 at the given path. Applies schema. Returns 0 on success. */
   int db1_init(const char *path);

   /* Whether the DB1 store can be reached from this process. Callers that used
    * to guard on db1_init succeeding want this instead: since the families
    * moved behind the module, a local file opening says nothing about whether
    * the store will answer. */
   int db1_store_ready(void);

   /* Whether the store can be USED right now, asked by calling it. Costs a
    * round trip (cached for a second), so this is for dependency readiness, not
    * for the guard in front of every store-backed command -- that is
    * db1_store_ready above.
    *
    * The two disagree for as long as the bus takes to notice a module that
    * died: availability is registry state, corrected by a 30s heartbeat and a
    * reap that runs every 7.5s, so db1_store_ready keeps saying yes for about
    * 37 seconds after the store stops answering. */
   int db1_store_probe(void);

   /* Close DB1. Safe to call if not initialized, or more than once. */
   void db1_shutdown(void);

   /* Returns 1 if db1_init has succeeded and the connection is live, 0 otherwise. */
   int db1_is_initialized(void);

   /* Apply aimee-server-mode pragmas (larger cache, mmap, wal_autocheckpoint).
    * Called once after db1_init by the process that RUNS the queries, which
    * since the migration is the module rather than the server. No-op if db1 is not
    * initialized. */
   void db1_apply_server_pragmas(void);

   /* Subsystem APIs owned by DB1. */
#include "wm.h"
#include "fsnap.h"
#include "clarify.h"
#include "diagnose.h"
#include "ensemble.h"
#include "eval.h"
#include "caches.h"
#include "primary_sessions.h"
#include "webchat_claude_sessions.h"
#include "env.h"
#include "db1_client/delegations.h"
#include "delegation_checkpoint.h"
#include "checkpoints.h"
#include "decisions.h"
#include "runtime_state.h"
#include "db1_client/server_sessions.h"
#include "session_state.h"
#include "session_paths.h"
#include "cost_fold.h"
#include "token_audit.h"
#include "agent_log.h"
#include "db1_client/agent_jobs.h"
#include "coord_jobs.h"
#include "execution_plans.h"
#include "db1_client/pipelines.h"
#include "secrets.h"
#include "cognify_jobs.h"
#include "local_operator.h"
#include "project_clones.h"
#include "db1_client/git_ownership.h"
#include "tool_local_availability.h"
#include "model_catalog.h"
#include "execution_trace.h"
#include "working_profile_local.h"
#include "db1_windows.h"
#include "db1_client/guardrail_events.h"
#include "db1_client/db1_trigger.h"
#include "mcp_osv_cache.h"
#include "server_management_jti.h"
#include "db1_client/remote_client_grant.h"

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_H */
