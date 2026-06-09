/* DB2 (Postgres) idempotent schema bootstrap.
 * See docs/proposals/accepted/three-db-split-user-shared-vectors.md. */

#include "db_schema.h"
#include "db_postgres.h"
#include "../schema_data.h"

#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
typedef struct sqlite3 sqlite3;
#else
#include <sqlite3.h>
#endif
#include <stdio.h>

#if !defined(AIMEE_DISABLE_DB2_SQLITE_SHIM) && (defined(__GNUC__) || defined(__clang__))
#pragma weak sqlite3_errmsg
#pragma weak sqlite3_exec
#pragma weak sqlite3_free
#endif

static void copy_sqlite_err(char *errbuf, size_t errlen, const char *src)
{
   if (!errbuf || errlen == 0)
      return;
   snprintf(errbuf, errlen, "%s", src ? src : "");
}

/* The DB2 SQLite shim is test/support infrastructure for DB2's Postgres
 * domain APIs. The SQLite-flavoured DB2 schema lives in
 * src/db2/schema_sqlite.sql and is embedded as AIMEE_DB2_SCHEMA_SQLITE_SQL. */

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM
static void db2_run_sqlite_migrations(sqlite3 *db)
{
   /* Each statement is independent; duplicate-column / missing-table errors are
    * ignored so legacy and fresh DBs both continue to the canonical schema. */
   static const char *migrations[] = {
       "ALTER TABLE code_embeddings ADD COLUMN body_hash TEXT NOT NULL DEFAULT ''",
       NULL,
   };
   for (int i = 0; migrations[i]; i++)
      sqlite3_exec(db, migrations[i], NULL, NULL, NULL);
}
#endif

int db_apply_schema_postgres(void *pg_conn, char *errbuf, size_t errlen)
{
   if (!pg_conn)
      return -1;
   return aimee_pg_exec(pg_conn, AIMEE_DB2_SCHEMA_SQL, errbuf, errlen);
}

int db2_apply_schema_sqlite_shim(sqlite3 *db, char *errbuf, size_t errlen)
{
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   (void)db;
   copy_sqlite_err(errbuf, errlen, "sqlite shim unavailable");
   return -1;
#else
   if (!db)
      return -1;
   if (!sqlite3_exec || !sqlite3_errmsg || !sqlite3_free)
   {
      copy_sqlite_err(errbuf, errlen, "sqlite shim unavailable");
      return -1;
   }
   db2_run_sqlite_migrations(db);
   char *err = NULL;
   int rc = sqlite3_exec(db, AIMEE_DB2_SCHEMA_SQLITE_SQL, NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      copy_sqlite_err(errbuf, errlen, err ? err : sqlite3_errmsg(db));
      sqlite3_free(err);
      return -1;
   }
   return 0;
#endif
}
