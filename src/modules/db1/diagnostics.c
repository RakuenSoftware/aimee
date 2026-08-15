/* db1/diagnostics.c: implementation for db1_diag_inspect. */

#include "diagnostics.h"
#include "maintenance.h"

#include "db.h"

#include <sqlite3.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

void db1_diag_inspect(const char *path, int check_fts, db1_diag_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->fts5_ok = -1;
   out->size_bytes = -1;

   if (!path || !path[0])
      return;

   struct stat st;
   if (stat(path, &st) == 0)
      out->size_bytes = (long)st.st_size;

   sqlite3 *db = NULL;
   int flags = check_fts ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
   if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK || !db)
   {
      if (db)
         sqlite3_close(db);
      return;
   }
   out->opened = 1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK)
   {
      if (sqlite3_step(stmt) == SQLITE_ROW)
         out->schema_version = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
   }

   if (check_fts)
   {
      out->fts5_ok = 1;
      stmt = NULL;
      if (sqlite3_prepare_v2(db,
                             "INSERT INTO window_fts(window_fts) "
                             "VALUES('integrity-check')",
                             -1, &stmt, NULL) == SQLITE_OK &&
          stmt)
      {
         int rc = sqlite3_step(stmt);
         if (rc != SQLITE_OK && rc != SQLITE_DONE)
            out->fts5_ok = 0;
         sqlite3_finalize(stmt);
      }
   }

   sqlite3_close(db);
}

/* Open the DB1 file as a standalone read-write sqlite handle for an
 * integrity check.  We don't go through db1_init because that's the
 * production lifecycle and would commit module state to a possibly
 * corrupted file; we only need the raw file open + PRAGMA quick_check.
 */
static sqlite3 *db1_diag_raw_open(const char *path)
{
   sqlite3 *db = NULL;
   if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
   {
      if (db)
         sqlite3_close(db);
      return NULL;
   }
   return db;
}

int db1_diag_quick_check_recover(const char *path)
{
   if (!path || !path[0])
      return -2;

   sqlite3 *db = db1_diag_raw_open(path);
   if (!db)
      return -2;

   int qc = db1_quick_check_sqlite(db);
   sqlite3_close(db);
   if (qc == 0)
      return 0;

   if (db1_recover(path, 0) != 0)
      return -1;

   /* Recovery rewrote the file; re-open to confirm it's now usable. */
   db = db1_diag_raw_open(path);
   if (!db)
      return -1;
   sqlite3_close(db);
   return 1;
}
