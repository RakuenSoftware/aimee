/* db1/model_pricing.c: server-owned per-model price table (DB1). See header. */

#include "model_pricing.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>

int db1_model_price_get(const char *model, double *in_per_mtok, double *out_per_mtok)
{
   if (in_per_mtok)
      *in_per_mtok = 0.0;
   if (out_per_mtok)
      *out_per_mtok = 0.0;
   if (!model || !model[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT cost_in_per_mtok, cost_out_per_mtok FROM model_pricing"
                          " WHERE model = ?1",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, model, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   int result;
   if (rc == SQLITE_ROW)
   {
      result = 1;
      if (in_per_mtok)
         *in_per_mtok = sqlite3_column_double(stmt, 0);
      if (out_per_mtok)
         *out_per_mtok = sqlite3_column_double(stmt, 1);
   }
   else if (rc == SQLITE_DONE)
      result = 0; /* no stored row */
   else
      result = -1; /* DB error — caller treats as "not authoritative" */
   sqlite3_finalize(stmt);
   return result;
}

int db1_model_price_set(const char *model, double in_per_mtok, double out_per_mtok)
{
   if (!model || !model[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO model_pricing (model, cost_in_per_mtok, cost_out_per_mtok,"
                          " updated_at) VALUES (?1, ?2, ?3, datetime('now'))"
                          " ON CONFLICT(model) DO UPDATE SET"
                          "  cost_in_per_mtok = excluded.cost_in_per_mtok,"
                          "  cost_out_per_mtok = excluded.cost_out_per_mtok,"
                          "  updated_at = excluded.updated_at",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, model, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 2, in_per_mtok);
   sqlite3_bind_double(stmt, 3, out_per_mtok);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_model_price_delete(const char *model)
{
   if (!model || !model[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM model_pricing WHERE model = ?1", -1, &stmt, NULL) !=
       SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, model, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}
