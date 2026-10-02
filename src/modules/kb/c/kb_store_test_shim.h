/* kb_store_test_shim.h: test-only KB_STORE shim lifecycle helpers.
 *
 * Production KB_STORE talks to Postgres through libpq. Unit tests link the
 * sqlite3-backed aimee_pg_* shim instead and call these helpers from
 * setUp / tearDown so the test code does not touch sqlite3,
 * kb_store_register_shared_sqlite, or kb_store_apply_schema_sqlite_shim
 * directly. Only src/tests/aimee_pg_sqlite_shim.c (the shim impl) and
 * src/modules/kb/c/ retain knowledge of the sqlite backing.
 *
 * Not used by aimee-server, aimee-kb, or any production code path.
 */
#ifndef DEC_KB_STORE_TEST_SHIM_H
#define DEC_KB_STORE_TEST_SHIM_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Open a fresh in-memory sqlite handle, apply the KB_STORE sqlite-shim
    * schema, register the handle as the shim backing, and call
    * kb_store_init("shim"). Closes any handle the helper previously
    * opened. Aborts via assert on any failure. */
   void kb_store_test_shim_open(void);

   /* Same as kb_store_test_shim_open but at the given filesystem path
    * (use ":memory:" for an in-memory instance). */
   void kb_store_test_shim_open_path(const char *path);

   /* Tear down the shim: kb_store_shutdown, clear DB1 stmt-cache entries
    * tied to the handle, unregister the handle, close the sqlite
    * handle. Safe to call when no handle is open. */
   void kb_store_test_shim_close(void);

   /* The sqlite3* backing the shim, or NULL if none is open.
    * Returned as void* so callers do not need <sqlite3.h>. Tests
    * cast back to sqlite3* for direct seed SQL during the gradual
    * seed-SQL migration; new tests should prefer
    * aimee_pg_exec(kb_store_conn(), ...) instead. */
   void *kb_store_test_shim_handle(void);

   /* Non-zero when the shim is backed by a real Postgres (AIMEE_TEST_KB_STORE_TEMPLATE_URL
    * is set) rather than sqlite. In that mode kb_store_test_shim_handle() returns NULL,
    * because there is no sqlite handle to hand out — a test that reaches for the
    * raw handle to run seed SQL must either skip or go through
    * aimee_pg_exec(kb_store_conn(), ...), which works on both backends. */
   int kb_store_test_shim_is_postgres(void);

   /* Give the eval scratch store (kb_store_eval_open_temp_store) somewhere disposable
    * to work. Under the sqlite shim it opens its own in-memory handle and this is
    * a no-op; backed by Postgres it needs a real database, so this creates the
    * per-process clone if one is not open yet and exports AIMEE_KB_STORE_EVAL_URL
    * pointing at it. Call once from main() before the first eval-store open.
    * Returns 0 when the eval store is usable. */
   int kb_store_test_shim_prepare_eval_store(void);

   /* Print a skip notice naming `test_name` and return 1 when the shim is on
    * Postgres, else 0. For the tests still seeding through the raw sqlite handle:
    *   if (kb_store_test_shim_skip_on_postgres("curator_queue")) return;
    * Keeps the PG run green and self-documenting instead of silently passing a
    * test whose fixture never ran. */
   int kb_store_test_shim_skip_on_postgres(const char *test_name);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_TEST_SHIM_H */
