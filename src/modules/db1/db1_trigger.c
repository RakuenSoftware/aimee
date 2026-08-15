/* db1/db1_trigger.c: trigger_runs CRUD — event-triggered autopilot queue. */

#include "db1_trigger.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

int db1_trigger_insert(const char *id, const char *source, const char *event, const char *task,
                       const char *workspace, const char *metadata)
{
   if (!id || !id[0] || !source || !source[0] || !task || !task[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT OR IGNORE INTO trigger_runs"
                            " (id, source, event, task, workspace, metadata, status, queued_at)"
                            " VALUES (?, ?, ?, ?, ?, ?, 'queued', datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, source, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, event ? event : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, task, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, workspace ? workspace : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, metadata ? metadata : "{}", -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE && changes == 1) ? 0 : -1;
}

int db1_trigger_status_set(const char *id, const char *status, const char *pipeline_id,
                           const char *error)
{
   if (!id || !id[0] || !status || !status[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Determine which timestamp column(s) to set based on the transition. */
   int set_started = (strcmp(status, "running") == 0);
   int set_finished = (strcmp(status, "complete") == 0 || strcmp(status, "failed") == 0 ||
                       strcmp(status, "cancelled") == 0);

   char sql[512];
   if (set_started)
   {
      snprintf(sql, sizeof(sql),
               "UPDATE trigger_runs SET status = ?, pipeline_id = ?, error = ?,"
               " started_at = datetime('now') WHERE id = ?");
   }
   else if (set_finished)
   {
      snprintf(sql, sizeof(sql),
               "UPDATE trigger_runs SET status = ?, pipeline_id = ?, error = ?,"
               " finished_at = datetime('now') WHERE id = ?");
   }
   else
   {
      snprintf(sql, sizeof(sql),
               "UPDATE trigger_runs SET status = ?, pipeline_id = ?, error = ? WHERE id = ?");
   }

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, pipeline_id ? pipeline_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, error ? error : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, id, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE && changes == 1) ? 0 : -1;
}

int db1_trigger_get(const char *id, db1_trigger_run_t *out)
{
   if (!id || !id[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, source, COALESCE(event,''), task, COALESCE(workspace,''),"
                            " metadata, COALESCE(pipeline_id,''), status, queued_at,"
                            " COALESCE(started_at,''), COALESCE(finished_at,''), COALESCE(error,'')"
                            " FROM trigger_runs WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out->id, sizeof(out->id), stmt, 0);
      db1_copy_col_text(out->source, sizeof(out->source), stmt, 1);
      db1_copy_col_text(out->event, sizeof(out->event), stmt, 2);
      db1_copy_col_text(out->task, sizeof(out->task), stmt, 3);
      db1_copy_col_text(out->workspace, sizeof(out->workspace), stmt, 4);
      db1_copy_col_text(out->metadata, sizeof(out->metadata), stmt, 5);
      db1_copy_col_text(out->pipeline_id, sizeof(out->pipeline_id), stmt, 6);
      db1_copy_col_text(out->status, sizeof(out->status), stmt, 7);
      db1_copy_col_text(out->queued_at, sizeof(out->queued_at), stmt, 8);
      db1_copy_col_text(out->started_at, sizeof(out->started_at), stmt, 9);
      db1_copy_col_text(out->finished_at, sizeof(out->finished_at), stmt, 10);
      db1_copy_col_text(out->error, sizeof(out->error), stmt, 11);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

char *db1_trigger_list_json(const char *status_filter)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;

   char sql[512];
   if (status_filter && status_filter[0])
   {
      snprintf(sql, sizeof(sql),
               "SELECT id, source, COALESCE(event,''), task, COALESCE(workspace,''),"
               " metadata, COALESCE(pipeline_id,''), status, queued_at,"
               " COALESCE(started_at,''), COALESCE(finished_at,''), COALESCE(error,'')"
               " FROM trigger_runs WHERE status = ?"
               " ORDER BY queued_at DESC LIMIT 100");
   }
   else
   {
      snprintf(sql, sizeof(sql),
               "SELECT id, source, COALESCE(event,''), task, COALESCE(workspace,''),"
               " metadata, COALESCE(pipeline_id,''), status, queued_at,"
               " COALESCE(started_at,''), COALESCE(finished_at,''), COALESCE(error,'')"
               " FROM trigger_runs"
               " ORDER BY queued_at DESC LIMIT 100");
   }

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;

   if (status_filter && status_filter[0])
      sqlite3_bind_text(stmt, 1, status_filter, -1, SQLITE_TRANSIENT);

   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      sqlite3_finalize(stmt);
      return NULL;
   }

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
         break;

      const unsigned char *cols[12];
      for (int i = 0; i < 12; i++)
         cols[i] = sqlite3_column_text(stmt, i);

      static const char *keys[] = {"id",        "source",     "event",       "task",
                                   "workspace", "metadata",   "pipeline_id", "status",
                                   "queued_at", "started_at", "finished_at", "error"};
      for (int i = 0; i < 12; i++)
         cJSON_AddStringToObject(obj, keys[i], cols[i] ? (const char *)cols[i] : "");

      cJSON_AddItemToArray(arr, obj);
   }
   sqlite3_finalize(stmt);

   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
}
