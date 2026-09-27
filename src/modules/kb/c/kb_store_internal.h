/* kb_store_internal.h: private to src/modules/kb/c/.
 *
 * Shared state between subsystem .c files and the init/shutdown pair
 * in kb_store_init.c. NOT installed; NOT included by anything outside
 * src/modules/kb/c/ or focused unit tests that verify lifecycle behavior.
 */
#ifndef DEC_KB_STORE_INTERNAL_H
#define DEC_KB_STORE_INTERNAL_H 1

struct sqlite3;
struct sqlite3_stmt;
struct aimee_pg_stmt;

#include <stddef.h>
void *kb_store_scope_connection_open(char *error, size_t capacity);
#include "../headers/aimee.h" /* memory_t */

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Format "now" as an ISO-8601 UTC timestamp (YYYY-MM-DDTHH:MM:SSZ) into buf.
    * The shape several kb_store subsystem files each open-coded as a private static. */
   static inline void kb_store_now_utc(char *buf, size_t len)
   {
      time_t now = time(NULL);
      struct tm tm_buf;
      gmtime_r(&now, &tm_buf);
      strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   }

   /* Returns the calling thread's Postgres connection, or NULL if KB_STORE is closed
    * or a non-init thread cannot acquire its own connection. The process-global
    * init connection is never shared across threads: libpq forbids concurrent
    * use of one PGconn. */
   void *(kb_store_conn)(void);

   /* Bracket a unit of work so the thread's pooled connection is returned to the
    * pool between units (refcounted; see lifecycle.h). */
   /* See kb_store.h: the kb_store_lease_begin macro records the caller's file:line. */
   void kb_store_lease_begin_at(const char *site);
#ifndef kb_store_lease_begin
#define KB_STORE_LEASE_STRINGIFY_(x) #x
#define KB_STORE_LEASE_SITE_(f, l)   f ":" KB_STORE_LEASE_STRINGIFY_(l)
#define kb_store_lease_begin()       kb_store_lease_begin_at(KB_STORE_LEASE_SITE_(__FILE__, __LINE__))
#endif
   void kb_store_lease_end(void);
   void kb_store_lease_release_idle(void);

   /* Open a new dedicated KB_STORE connection for the calling thread and set
    * it as the thread's active connection (returned by kb_store_conn()).
    * Returns the new connection on success, NULL on failure (errbuf
    * filled). Only callable after kb_store_init has succeeded. */
   void *kb_store_thread_conn_open(char *errbuf, size_t errlen);

   /* Close and clear the calling thread's dedicated KB_STORE connection.
    * No-op if no thread connection is open. */
   void kb_store_thread_conn_close(void);

   /* Shared 12-column row mapper used by every kb_store read helper that returns
    * memory_t via the standard SELECT shape: (id, tier, kind, key, content,
    * confidence, use_count, last_used_at, created_at, updated_at,
    * source_session, salience). Implemented in kb_store/memory_row_mapper_pg.c.
    * Reads via aimee_pg_column_*. Callers inside src/modules/kb/c/ only. */
   void kb_store_fill_memory_12col_pg(struct aimee_pg_stmt *stmt, memory_t *m);

   /* Test-shim sqlite accessor. Production KB_STORE callers use kb_store_conn()
    * (libpq); this returns the raw sqlite3* that backs the in-memory
    * shim used by unit tests. Returns NULL when no shim handle is
    * registered. Callers inside src/modules/kb/c/ only. */
   struct sqlite3 *kb_store_shared_sqlite(void);

   /* Register the raw sqlite3 handle the test shim should use. Tests
    * call this with their own ephemeral handle; production code never
    * calls it. Safe to call with NULL to clear. */
   void kb_store_register_shared_sqlite(struct sqlite3 *h);

   /* §2c: the kb_store_init mutex, exposed so the dim-change reset serializes its
    * destructive execute + in-memory dim swap against kb_store_init / a concurrent reset.
    * Non-recursive; do not call while already held on the same thread. */
   void kb_store_init_lock(void);
   void kb_store_init_unlock(void);

   /* Mark the registered handle as ephemeral. Cleared to 0 automatically
    * by kb_store_register_shared_sqlite. */
   void kb_store_set_ephemeral(int ephemeral);

   /* Ephemeral flag for the memory/embed path.  Eval harnesses and
    * benchmarks set this to 1 when using a throwaway scratch database;
    * the memory sync helpers check it to skip pgvector upserts that
    * would leak beyond the test's isolation. */
   int kb_store_is_ephemeral(void);

   /* --- Generic scalar/exec convenience helpers (kb_store/kb_store_init.c) -------
    * Wrap the prepare/bind/step/finalize dance for the two patterns that
    * recur across the kb_store sources: a single-row scalar read, and a
    * fire-and-forget write. Each acquires kb_store_conn() internally and returns a
    * safe default when there is no connection, the prepare fails, or no row
    * is produced. Argument-bind suffixes name the placeholders bound in
    * order: `_id` binds ?1=int64, `_text` binds ?1=text, `_text_id` binds
    * ?1=text and ?2=int64. */

   /* SELECT one integer (column 0 of the first row). No bind. */
   int kb_store_scalar_int(const char *sql, int dflt);

   /* SELECT one integer with a single text bind to ?1. */
   int kb_store_scalar_int_text(const char *sql, const char *a1, int dflt);

   /* SELECT one double with a single int64 bind to ?1. */
   double kb_store_scalar_double_id(const char *sql, int64_t id, double dflt);

   /* Exec (INSERT/UPDATE/DELETE), single int64 bind ?1, result ignored. */
   void kb_store_exec_id(const char *sql, int64_t id);

   /* Exec, single text bind ?1, result ignored. */
   void kb_store_exec_text(const char *sql, const char *a1);

   /* Exec, text ?1 + int64 ?2 (the "SET col = text WHERE id = ?" shape),
    * result ignored. */
   void kb_store_exec_text_id(const char *sql, const char *a1, int64_t id);

   /* Exec on a caller-supplied connection (for use inside a caller-managed
    * transaction, where kb_store_conn() must not be re-acquired), single int64 bind
    * ?1. Returns 0 on success, -1 if conn is NULL or prepare/step fails. */
   int kb_store_exec_conn_int64(void *conn, const char *sql, int64_t arg);

   /* Copy `src` (or "" when NULL) into the `cap`-sized `dst`, truncating
    * safely. The per-file copy_text_pg helper that several kb_store row mappers
    * each defined. */
   void kb_store_copy_text(char *dst, size_t cap, const char *src);

   /* Copy column `col` of the current row (text, or "" when NULL) into the
    * `cap`-sized `dst`. The per-file copy_col_text helper that several kb_store row
    * mappers each defined. */
   void kb_store_copy_col_text(char *dst, size_t cap, struct aimee_pg_stmt *st, int col);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_INTERNAL_H */
