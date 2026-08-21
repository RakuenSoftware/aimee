/* db2_test_shim.h: test-only DB2 shim lifecycle helpers.
 *
 * Production DB2 talks to Postgres through libpq. Unit tests link the
 * sqlite3-backed aimee_pg_* shim instead and call these helpers from
 * setUp / tearDown so the test code does not touch sqlite3,
 * db2_register_shared_sqlite, or db2_apply_schema_sqlite_shim
 * directly. Only src/tests/aimee_pg_sqlite_shim.c (the shim impl) and
 * src/modules/db2/c/ retain knowledge of the sqlite backing.
 *
 * Not used by aimee-server, aimee-kb, or any production code path.
 */
#ifndef DEC_DB2_TEST_SHIM_H
#define DEC_DB2_TEST_SHIM_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Open a fresh in-memory sqlite handle, apply the DB2 sqlite-shim
    * schema, register the handle as the shim backing, and call
    * db2_init("shim"). Closes any handle the helper previously
    * opened. Aborts via assert on any failure. */
   void db2_test_shim_open(void);

   /* Same as db2_test_shim_open but at the given filesystem path
    * (use ":memory:" for an in-memory instance). */
   void db2_test_shim_open_path(const char *path);

   /* Tear down the shim: db2_shutdown, clear DB1 stmt-cache entries
    * tied to the handle, unregister the handle, close the sqlite
    * handle. Safe to call when no handle is open. */
   void db2_test_shim_close(void);

   /* The sqlite3* backing the shim, or NULL if none is open.
    * Returned as void* so callers do not need <sqlite3.h>. Tests
    * cast back to sqlite3* for direct seed SQL during the gradual
    * seed-SQL migration; new tests should prefer
    * aimee_pg_exec(db2_conn(), ...) instead. */
   void *db2_test_shim_handle(void);

   /* Non-zero when the shim is backed by a real Postgres (AIMEE_TEST_DB2_TEMPLATE_URL
    * is set) rather than sqlite. In that mode db2_test_shim_handle() returns NULL,
    * because there is no sqlite handle to hand out — a test that reaches for the
    * raw handle to run seed SQL must either skip or go through
    * aimee_pg_exec(db2_conn(), ...), which works on both backends. */
   int db2_test_shim_is_postgres(void);

   /* Print a skip notice naming `test_name` and return 1 when the shim is on
    * Postgres, else 0. For the tests still seeding through the raw sqlite handle:
    *   if (db2_test_shim_skip_on_postgres("curator_queue")) return;
    * Keeps the PG run green and self-documenting instead of silently passing a
    * test whose fixture never ran. */
   int db2_test_shim_skip_on_postgres(const char *test_name);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TEST_SHIM_H */
