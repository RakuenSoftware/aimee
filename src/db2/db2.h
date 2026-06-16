/* db2.h: DB2 (shared relational tier) — public umbrella header.
 *
 * Code outside src/db2/ never includes libpq headers, never sees a
 * connection handle, and never constructs DB2 SQL. Callers use typed
 * domain functions declared by DB2 subsystem headers as they land.
 *
 * Lifecycle: db2_init() is called once at startup with the DB2 libpq
 * connection URL. db2_shutdown() is called at exit. The connection and
 * all subsystem state are private to src/db2/.
 */
#ifndef DEC_DB2_H
#define DEC_DB2_H 1

#include <stddef.h>
#include "lifecycle.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Open DB2 at the given libpq connection URL. Applies schema.
    * Returns 0 on success. */
   int db2_init(const char *libpq_url);

   /* Set the connection-pool size used by db2_init (default 16). Call before
    * db2_init, like db2_set_embedding_dim — keeps the db2 layer config-free. */
   void db2_set_pool_size(int size);

   /* Bracket a unit of work so the thread's pooled connection is returned
    * between units (instead of held for the thread's life). Re-entrant
    * (refcounted): nested begin/end pairs reuse the same lease. db2_conn()
    * returns the current lease (lazily leasing one if none is held). Outside any
    * begin/end, a lazily-leased connection is returned when the thread exits. */
   void db2_lease_begin(void);
   void db2_lease_end(void);
   void db2_lease_release_idle(void);

   /* Snapshot the DB2 connection URL for forked child reopen. Returns 1 when
    * a URL is available, else 0. */
   int db2_fork_conn_url(char *out, size_t cap);

   /* Verify the DB2 connection is queryable and report a small health
    * summary needed by `aimee doctor`.
    *
    * `schema_ok` receives 1 when the current schema contains the
    * `memories` table, else 0. `have_pg_trgm` receives 1 when the
    * extension is available on the server, else 0. Either output may be
    * NULL if the caller does not need it.
    *
    * Returns 0 on success, -1 if DB2 is not initialized or the probe
    * queries fail. */
   int db2_health_probe(int *schema_ok, int *have_pg_trgm);
   int db2_kb_health_probe(int *kb_tables_ok);

   /* Close DB2. Safe to call if not initialized, or more than once. */
   void db2_shutdown(void);

   /* DB2 subsystem APIs. */
#include "agent_hints.h"
#include "agent_outcomes.h"
#include "anti_patterns.h"
#include "canonical_index.h"
#include "code_index.h"
#include "collab_rules.h"
#include "curiosity.h"
#include "decision_log.h"
#include "entity_edges.h"
#include "entity_profiles.h"
/* epistemic_directives.h is intentionally NOT in the umbrella — it
 * forward-references memory_directive_t which lives in headers/memory.h.
 * The orchestrator (memory_directives.c) includes it directly. */
#include "failed_queries.h"
#include "feedback.h"
#include "kb_payload.h"
#include "kb_runtime_state.h"
/* kind_lifecycle.h is intentionally NOT in the umbrella — it pulls
 * headers/memory.h to access kind_lifecycle_t and most consumers
 * already include memory.h. Callers include "kind_lifecycle.h"
 * directly when they need db2_kind_lifecycle_load. */
#include "memory_payload.h"
#include "memory_scenes.h"
#include "notes.h"
#include "prospective_memories.h"
#include "rules.h"
#include "stopwords.h"
#include "tasks.h"
#include "tool_registry.h"
#include "trace_mining.h"

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_H */
