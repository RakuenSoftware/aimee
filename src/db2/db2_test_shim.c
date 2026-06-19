/* db2_test_shim.c: helpers that own the DB2 shim lifecycle so tests
 * do not need to touch sqlite3 directly. See db2_test_shim.h.
 *
 * Compiled only when the shim is enabled (kb builds set
 * AIMEE_DISABLE_DB2_SQLITE_SHIM and skip this translation unit's
 * body). Linked into test binaries via tests/Rules.mk; not included
 * in DB2_SRCS, so production server/CLI builds carry the symbols
 * but never call them. */

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM

#include "db2_test_shim.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_schema.h"
#include "lifecycle.h" /* db2_set_embedding_dim */

#include <assert.h>
#include <sqlite3.h>
#include <stddef.h>

/* Weakly link to DB1's per-handle statement-cache flush. The shim
 * uses one sqlite handle to back the DB2 surface, and any DB1 helper
 * that prepared a statement against it (test seed paths, stray
 * db1_prepare callers in tests still being migrated) must drop its
 * cached entry before the handle closes. Production binaries that
 * link db1 expose the symbol; standalone test variants without db1
 * resolve it to NULL and skip the call. */
extern void db1_stmt_cache_clear_for_sqlite(struct sqlite3 *db) __attribute__((weak));

static sqlite3 *g_shim_handle;

void db2_test_shim_open(void)
{
   db2_test_shim_open_path(":memory:");
}

void db2_test_shim_open_path(const char *path)
{
   db2_test_shim_close();

   sqlite3 *raw = NULL;
   int rc = sqlite3_open(path && *path ? path : ":memory:", &raw);
   assert(rc == SQLITE_OK && raw != NULL);

   /* Foreign keys must be on for cascade-delete schema to behave like
    * production; the shim schema relies on it (memory_workspaces FK to
    * memories, etc.). */
   sqlite3_exec(raw, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

   /* Unit tests embed with the builtin (384-dim) embedder. Default the active
    * embedding dim to 384 so the upsert dim guard accepts builtin vectors; tests
    * that exercise a specific tier (0.6b=1024, 4b=2560) call db2_set_embedding_dim
    * themselves after open and override this. */
   db2_set_embedding_dim(384);

   char err[512] = {0};
   rc = db2_apply_schema_sqlite_shim(raw, err, sizeof(err));
   assert(rc == 0);

   db2_register_shared_sqlite(raw);
   rc = db2_init("shim");
   assert(rc == 0);

   g_shim_handle = raw;
}

void db2_test_shim_close(void)
{
   if (!g_shim_handle)
   {
      db2_register_shared_sqlite(NULL);
      return;
   }

   db2_shutdown();
   if (db1_stmt_cache_clear_for_sqlite)
      db1_stmt_cache_clear_for_sqlite(g_shim_handle);
   db2_register_shared_sqlite(NULL);
   sqlite3_close(g_shim_handle);
   g_shim_handle = NULL;
}

void *db2_test_shim_handle(void)
{
   return g_shim_handle;
}

#endif /* !AIMEE_DISABLE_DB2_SQLITE_SHIM */
