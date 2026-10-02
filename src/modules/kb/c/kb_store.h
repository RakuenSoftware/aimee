/* Knowledge-store domain API. PostgreSQL owns connections and pooling.
 * Runtime callers lease host-scoped provider sessions; bootstrap uses its
 * separate migration identity. This header exposes no database driver. */
#ifndef DEC_KB_STORE_H
#define DEC_KB_STORE_H 1

#include <stddef.h>
#include "lifecycle.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Open a private provider session at an explicit PostgreSQL URL. Applies schema.
    * Returns 0 on success. */
   int kb_store_init(const char *libpq_url);

   /* Select supervised runtime, exclusive bootstrap, or configured local sessions. */
   int kb_store_init_runtime(void);
   int kb_store_init_migration(void);
   int kb_store_init_configured(void);

   /* Bracket a unit of work so the thread's pooled connection is returned
    * between units (instead of held for the thread's life). Re-entrant
    * (refcounted): nested begin/end pairs reuse the same lease. kb_store_conn()
    * returns the current lease (lazily leasing one if none is held). Outside any
    * begin/end, a lazily-leased connection is returned when the thread exits. */
   /* Take (or nest into) this thread's connection lease for a unit of work.
    * The macro records the call site so a lease that is never ended is reported
    * by the reaper as the code that took it rather than a pool member index. */
   void kb_store_lease_begin_at(const char *site);
#define KB_STORE_LEASE_STRINGIFY_(x) #x
#define KB_STORE_LEASE_SITE_(f, l)   f ":" KB_STORE_LEASE_STRINGIFY_(l)
#define kb_store_lease_begin()       kb_store_lease_begin_at(KB_STORE_LEASE_SITE_(__FILE__, __LINE__))
   void kb_store_lease_end(void);
   void kb_store_lease_release_idle(void);

   /* Verify the KB_STORE connection is queryable and report a small health
    * summary needed by `aimee doctor`.
    *
    * `schema_ok` receives 1 when the current schema contains the
    * `memories` table, else 0. `have_pg_trgm` receives 1 when the
    * extension is available on the server, else 0. Either output may be
    * NULL if the caller does not need it.
    *
    * Returns 0 on success, -1 if KB_STORE is not initialized or the probe
    * queries fail. */
   int kb_store_health_probe(int *schema_ok, int *have_pg_trgm);
   int kb_store_kb_health_probe(int *kb_tables_ok);

   /* Close KB_STORE. Safe to call if not initialized, or more than once. */
   void kb_store_shutdown(void);

   /* KB_STORE subsystem APIs. */
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
 * directly when they need kb_store_kind_lifecycle_load. */
#include "memory_payload.h"
#include "notes.h"
#include "rules.h"
#include "stopwords.h"
#include "tasks.h"
#include "tool_registry.h"

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_H */
