/* db1/model_catalog.c: provider model catalog cache. */

#include "model_catalog.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void db1_model_catalog_free(provider_model_t *models, int n)
{
   (void)n; /* one contiguous block; no per-row owned pointers */
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

static void copy_text_col(sqlite3_stmt *stmt, int col, char *dst, size_t cap)
{
   const unsigned char *s = sqlite3_column_text(stmt, col);
   snprintf(dst, cap, "%s", s ? (const char *)s : "");
}

int db1_model_catalog_get(const char *provider, provider_model_t **models_out, int *n_out)
{
   if (!provider || !provider[0] || !models_out || !n_out)
      return -1;
   *models_out = NULL;
   *n_out = 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT model, display_name, context_window, max_output, caps, deprecated"
       " FROM model_catalog WHERE provider = ? ORDER BY model";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_TRANSIENT);
   int cap = 16;
   int n = 0;
   provider_model_t *models = calloc((size_t)cap, sizeof(*models));
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
         provider_model_t *grown = realloc(models, (size_t)cap * sizeof(*models));
         if (!grown)
         {
            sqlite3_finalize(stmt);
            free(models);
            return -1;
         }
         models = grown;
      }
      provider_model_t *m = &models[n];
      memset(m, 0, sizeof(*m));
      copy_text_col(stmt, 0, m->id, sizeof(m->id));
      if (!m->id[0])
         continue; /* a row with no id is unusable, not an error */
      copy_text_col(stmt, 1, m->display_name, sizeof(m->display_name));
      m->context_window = sqlite3_column_int(stmt, 2);
      m->max_output = sqlite3_column_int(stmt, 3);
      m->caps = (unsigned)sqlite3_column_int(stmt, 4);
      m->deprecated = sqlite3_column_int(stmt, 5);
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

int db1_model_catalog_replace(const char *provider, const provider_model_t *models, int n)
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
       " (provider, model, display_name, context_window, max_output, caps, deprecated,"
       "  pricing_tier, tool_support, streaming_support, fetched_at, metadata_json)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, 0, 0, 0, datetime('now'), '{}')";
   if (sqlite3_prepare_v2(db, insert_sql, -1, &ins, NULL) != SQLITE_OK)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   for (int i = 0; i < n; i++)
   {
      if (!models[i].id[0])
         continue;
      sqlite3_bind_text(ins, 1, provider, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 2, models[i].id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 3, models[i].display_name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(ins, 4, models[i].context_window);
      sqlite3_bind_int(ins, 5, models[i].max_output);
      sqlite3_bind_int(ins, 6, (int)models[i].caps);
      sqlite3_bind_int(ins, 7, models[i].deprecated);
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
