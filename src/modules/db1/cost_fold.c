/* db1/cost_fold.c: see cost_fold.h */

#include "cost_fold.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>

int db1_cost_fold_record(const char *parent_session_id, const char *child_session_id,
                         double cost_usd, const char *source)
{
   if (!parent_session_id || !parent_session_id[0] || !child_session_id || !child_session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   /* INSERT OR IGNORE so the UNIQUE constraint on
    * (parent_session_id, child_session_id) makes re-folds a no-op
    * without raising an error. */
   static const char *sql =
       "INSERT OR IGNORE INTO cost_fold_log (parent_session_id, child_session_id,"
       " cost_usd, cost_source) VALUES (?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, parent_session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, child_session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 3, cost_usd);
   sqlite3_bind_text(stmt, 4, source ? source : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(stmt);
   return changed;
}

double db1_cost_fold_total(const char *parent_session_id)
{
   if (!parent_session_id || !parent_session_id[0])
      return 0.0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0.0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COALESCE(SUM(cost_usd), 0.0) FROM cost_fold_log"
                            " WHERE parent_session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0.0;
   sqlite3_bind_text(stmt, 1, parent_session_id, -1, SQLITE_TRANSIENT);
   double total = 0.0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      total = sqlite3_column_double(stmt, 0);
   sqlite3_finalize(stmt);
   return total;
}
