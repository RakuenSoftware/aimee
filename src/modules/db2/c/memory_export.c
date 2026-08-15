/* db2/memory_export.c: JSONL export helpers for the memories table.
 * Postgres via libpq. */

#include "memory_export.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEX_ERRBUF 256

void db2_memory_export_row_free(db2_memory_export_row_t *row)
{
   if (!row)
      return;
   free(row->key);
   free(row->content);
   free(row->source_session);
   free(row->created_at);
   free(row->updated_at);
   row->key = NULL;
   row->content = NULL;
   row->source_session = NULL;
   row->created_at = NULL;
   row->updated_at = NULL;
}

static char *mex_dup_text(const char *src)
{
   /* Mirror cmd_data's empty-string-on-NULL convention: importers expect
    * non-NULL strings on every column. Returns NULL only on alloc
    * failure — caller treats that as a hard error. */
   if (!src)
      src = "";
   size_t n = strlen(src);
   char *dup = (char *)malloc(n + 1);
   if (!dup)
      return NULL;
   memcpy(dup, src, n + 1);
   return dup;
}

int db2_memory_export_alloc_all(db2_memory_export_row_t **out, size_t *count)
{
   if (out)
      *out = NULL;
   if (count)
      *count = 0;
   if (!out || !count)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                            " source_session, created_at, updated_at"
                            " FROM memories ORDER BY id";
   char err[MEX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   db2_memory_export_row_t *rows = NULL;
   size_t row_count = 0;
   size_t row_cap = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (row_count == row_cap)
      {
         size_t ncap = row_cap ? row_cap * 2 : 256;
         db2_memory_export_row_t *grown =
             (db2_memory_export_row_t *)realloc(rows, ncap * sizeof(*grown));
         if (!grown)
         {
            for (size_t i = 0; i < row_count; i++)
               db2_memory_export_row_free(&rows[i]);
            free(rows);
            aimee_pg_finalize(st);
            return -1;
         }
         rows = grown;
         row_cap = ncap;
      }
      db2_memory_export_row_t *r = &rows[row_count];
      memset(r, 0, sizeof(*r));
      r->id = aimee_pg_column_int64(st, 0);
      const char *tier = aimee_pg_column_text(st, 1);
      const char *kind = aimee_pg_column_text(st, 2);
      snprintf(r->tier, sizeof(r->tier), "%s", tier ? tier : "");
      snprintf(r->kind, sizeof(r->kind), "%s", kind ? kind : "");
      r->key = mex_dup_text(aimee_pg_column_text(st, 3));
      r->content = mex_dup_text(aimee_pg_column_text(st, 4));
      r->confidence = aimee_pg_column_double(st, 5);
      r->use_count = aimee_pg_column_int(st, 6);
      r->source_session = mex_dup_text(aimee_pg_column_text(st, 7));
      r->created_at = mex_dup_text(aimee_pg_column_text(st, 8));
      r->updated_at = mex_dup_text(aimee_pg_column_text(st, 9));
      row_count++;
   }
   aimee_pg_finalize(st);

   *out = rows;
   *count = row_count;
   return 0;
}

int db2_memory_decisions_export_jsonl(const char *path)
{
   if (!path || !*path)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT tier, kind, key, content, confidence,"
                            " source_session, created_at FROM memories"
                            " WHERE kind = 'decision' ORDER BY id";
   char err[MEX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   FILE *f = fopen(path, "w");
   if (!f)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cJSON *obj = cJSON_CreateObject();
      const char *tier = aimee_pg_column_text(st, 0);
      const char *kind = aimee_pg_column_text(st, 1);
      const char *key = aimee_pg_column_text(st, 2);
      const char *content = aimee_pg_column_text(st, 3);
      const char *source_session = aimee_pg_column_text(st, 5);
      const char *created_at = aimee_pg_column_text(st, 6);
      cJSON_AddStringToObject(obj, "tier", tier ? tier : "");
      cJSON_AddStringToObject(obj, "kind", kind ? kind : "");
      cJSON_AddStringToObject(obj, "key", key ? key : "");
      cJSON_AddStringToObject(obj, "content", content ? content : "");
      cJSON_AddNumberToObject(obj, "confidence", aimee_pg_column_double(st, 4));
      cJSON_AddStringToObject(obj, "source_session", source_session ? source_session : "");
      cJSON_AddStringToObject(obj, "created_at", created_at ? created_at : "");

      char *line = cJSON_PrintUnformatted(obj);
      cJSON_Delete(obj);
      if (line)
      {
         fprintf(f, "%s\n", line);
         free(line);
         count++;
      }
   }
   fclose(f);
   aimee_pg_finalize(st);
   return count;
}
