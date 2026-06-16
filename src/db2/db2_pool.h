/* db2_pool.h: bounded DB2 (Postgres) connection pool.
 *
 * Decouples connection count (M) from thread count (N): instead of one libpq
 * connection per live thread (the legacy db2_conn() model, which scales 1:1
 * with threads and marches into Postgres max_connections), a fixed pool of M
 * reusable connections is leased/returned at unit-of-work boundaries. See
 * docs/proposals/pending/db2-connection-pool.md.
 *
 * This module is the raw bounded pool (lease/return/reset/reaper). The
 * thread-local, re-entrant lease scoping that preserves db2_conn() semantics
 * lives in db2_init.c (db2_lease_begin/_end, plus a lazy per-thread lease so the
 * 668 db2_conn() call sites are unchanged). The pool is ALWAYS ON — db2_init()
 * initializes it unconditionally; there is no opt-in flag and no per-thread
 * fallback. WP-0's re-entrancy audit (zero cross-connection nesting) is what
 * makes replacing the per-thread model outright safe.
 */
#ifndef DEC_DB2_POOL_H
#define DEC_DB2_POOL_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Hard upper bound on pool size (guards config). */
#define DB2_POOL_MAX_SIZE 256
   /* Default lease wait when a caller passes <= 0. */
#define DB2_POOL_DEFAULT_LEASE_TIMEOUT_MS 30000

   /* Open `size` connections to libpq_url and start the reaper. Returns 0 on
    * success, -1 on error (errbuf set). size is clamped to [1, DB2_POOL_MAX_SIZE].
    * Idempotent: a second call with the same url is a no-op success. */
   int db2_pool_init(const char *libpq_url, int size, char *errbuf, size_t errbuf_len);

   /* True once db2_pool_init has succeeded. */
   int db2_pool_active(void);

   /* Lease one connection, blocking up to timeout_ms (<=0 -> default). Returns a
    * PGconn* (void*) owned by the caller until db2_pool_return, or NULL on
    * timeout/exhaustion (the caller must fail its unit of work cleanly, NOT
    * fall back to an unbounded wait). */
   void *db2_pool_lease(int timeout_ms);

   /* Reset session state (ROLLBACK; RESET SESSION AUTHORIZATION; DISCARD ALL)
    * and return the connection to the pool. On reset failure the member is
    * poisoned: closed and replaced with a fresh connection (rate-capped). NULL
    * is ignored. */
   void db2_pool_return(void *conn);

   /* Drain + close all members; stop the reaper. After this db2_pool_active()
    * is false. */
   void db2_pool_shutdown(void);

   /* Snapshot for metrics / `aimee workers`. Any out-param may be NULL.
    * `stuck` = leases observed held past the hold ceiling (a live thread that
    * missed lease_end; logged, not reclaimed — see db2_pool_reaper_sweep). */
   void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants,
                       long *lease_timeouts, long *stuck, long *poisoned);

   /* Run one reaper sweep synchronously: logs + counts leases held past the
    * ceiling and returns that count. It does NOT reclaim — reclaiming a
    * still-leased connection a live thread may use would corrupt libpq;
    * reclaim-on-thread-death is the lease thread-local destructor's job
    * (db2_init.c). Production also runs an internal reaper thread. */
   int db2_pool_reaper_sweep(void);

   /* Test seam: override connection open/close/reset (any NULL restores the real
    * libpq op) so unit tests exercise lease/return/poison without a Postgres
    * server. Not for production use. */
   void db2_pool_set_test_ops(void *(*open_fn)(const char *, char *, size_t),
                              void (*close_fn)(void *), int (*reset_fn)(void *));

   /* Test seam: lower the stuck-lease ceiling so a sweep can flag in ms. */
   void db2_pool_set_test_ceiling_ms(int ceiling_ms);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_POOL_H */
