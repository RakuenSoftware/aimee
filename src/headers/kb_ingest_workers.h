#ifndef DEC_KB_INGEST_WORKERS_H
#define DEC_KB_INGEST_WORKERS_H 1

/* kb_ingest_workers.h: aimee-kb's in-process KB ingest driver.
 *
 * aimee-kb owns DB2 directly, so it claims ingest jobs from the DB2
 * queue itself (db2_kb_ingest_queue_claim_next) and runs the full build
 * in-process (kb_build + canonical_index_scan_project) — no RPC round-trip
 * back to a server-side compute pool. This replaces the former
 * server_kb_workers.c dispatcher that ran while ingest compute lived in
 * aimee-server. */

#include "kb_service.h"

/* Start the ingest worker pool, periodic enqueue timer, and inotify watcher
 * on ctx (sized by cfg.kb_worker_count, clamped to KB_WORKER_MAX). No-op when
 * the cap is 0 or DB2 is unavailable. */
void kb_ingest_workers_start(kb_service_ctx_t *ctx);

/* Stop and join all ingest threads on ctx. */
void kb_ingest_workers_stop(kb_service_ctx_t *ctx);

#endif /* DEC_KB_INGEST_WORKERS_H */
