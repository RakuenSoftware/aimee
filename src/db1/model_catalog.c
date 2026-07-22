/* db1/model_catalog.c: provider model catalog cache. */

#include "model_catalog.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

void db1_model_catalog_free(char **models, int n)
{
   if (!models)
      return;
   for (int i = 0; i < n; i++)
      free(models[i]);
   free(models);
}

int db1_model_catalog_is_fresh(const char *provider, int ttl_seconds)
{
   if (!provider || !provider[0] || ttl_seconds <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT COUNT(*) FROM model_catalog"
       " WHERE provider = ? AND fetched_at > datetime('now', '-' || ? || ' seconds')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, ttl_seconds);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count > 0;
}

int db1_model_catalog_get(const char *provider, char ***models_out, int *n_out)
{
   if (!provider || !provider[0] || !models_out || !n_out)
      return -1;
   *models_out = NULL;
   *n_out = 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT model FROM model_catalog WHERE provider = ? ORDER BY model";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_TRANSIENT);
   int cap = 16;
   int n = 0;
   char **models = calloc((size_t)cap, sizeof(char *));
   if (!models)
   {
      sqlite3_finalize(stmt);
      return -1;
   }

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      if (n == cap)
      {
         cap *= 2;
         char **grown = realloc(models, (size_t)cap * sizeof(char *));
         if (!grown)
         {
            sqlite3_finalize(stmt);
            db1_model_catalog_free(models, n);
            return -1;
         }
         models = grown;
      }
      const unsigned char *model = sqlite3_column_text(stmt, 0);
      models[n] = strdup(model ? (const char *)model : "");
      if (models[n])
         n++;
   }
   sqlite3_finalize(stmt);
   if (n == 0)
   {
      free(models);
      return -1;
   }

   *models_out = models;
   *n_out = n;
   return 0;
}

int db1_model_catalog_replace(const char *provider, char **models, int n)
{
   if (!provider || !provider[0] || !models || n < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;

   sqlite3_stmt *del = NULL;
   static const char *delete_sql = "DELETE FROM model_catalog WHERE provider = ?";
   if (sqlite3_prepare_v2(db, delete_sql, -1, &del, NULL) != SQLITE_OK)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   sqlite3_bind_text(del, 1, provider, -1, SQLITE_TRANSIENT);
   int ok = sqlite3_step(del) == SQLITE_DONE;
   sqlite3_finalize(del);
   if (!ok)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }

   sqlite3_stmt *ins = NULL;
   static const char *insert_sql =
       "INSERT INTO model_catalog"
       " (provider, model, context_window, pricing_tier, tool_support, streaming_support,"
       "  fetched_at, metadata_json)"
       " VALUES (?, ?, 0, 0, 0, 0, datetime('now'), '{}')";
   if (sqlite3_prepare_v2(db, insert_sql, -1, &ins, NULL) != SQLITE_OK)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   for (int i = 0; i < n; i++)
   {
      if (!models[i] || !models[i][0])
         continue;
      sqlite3_bind_text(ins, 1, provider, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 2, models[i], -1, SQLITE_TRANSIENT);
      if (sqlite3_step(ins) != SQLITE_DONE)
      {
         sqlite3_finalize(ins);
         db1_txn_end(db, "ROLLBACK");
         return -1;
      }
      sqlite3_reset(ins);
      sqlite3_clear_bindings(ins);
   }
   sqlite3_finalize(ins);

   if (db1_txn_end(db, "COMMIT") != 0)
   {
      /* gate already released; a failed COMMIT auto-rolls-back in sqlite */
      return -1;
   }
   return 0;
}
