/* src/modules/db2/c/shadow_delta.c: Phase 7 shadow-mode rank-delta persistence. */

#include "shadow_delta.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define SD_ERRBUF 256

int db2_shadow_delta_insert(const db2_shadow_delta_row_t *row)
{
   if (!row || !row->query_hash[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "INSERT INTO memory_recall_shadow_deltas"
                            " (query_hash, project, mode, result_count, delta_json)"
                            " VALUES (?1, ?2, ?3, ?4, ?5)";
   char err[SD_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", row->query_hash);
   aimee_pg_bind_text(st, "?2", row->project);
   aimee_pg_bind_text(st, "?3", row->mode[0] ? row->mode : "shadow");
   aimee_pg_bind_int64(st, "?4", row->result_count);
   aimee_pg_bind_text(st, "?5", row->delta_json ? row->delta_json : "{}");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int64_t db2_shadow_delta_count(const char *project)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[256];
   if (project && project[0])
      snprintf(sql, sizeof(sql),
               "SELECT COUNT(*) FROM memory_recall_shadow_deltas WHERE project = ?1");
   else
      snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM memory_recall_shadow_deltas");
   char err[SD_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   if (project && project[0])
      aimee_pg_bind_text(st, "?1", project);
   int64_t n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_shadow_delta_cleanup(const char *project, int max_rows, int retention_days)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   if (max_rows <= 0)
      max_rows = SHADOW_DELTA_DEFAULT_MAX_ROWS;
   if (retention_days <= 0)
      retention_days = SHADOW_DELTA_DEFAULT_RETENTION_DAYS;

   int deleted = 0;
   char err[SD_ERRBUF] = "";
   int scoped = (project && project[0]) ? 1 : 0;

   /* 1) Age-based deletion: rows older than retention_days. */
   {
      char sql[512];
      if (scoped)
         snprintf(sql, sizeof(sql),
                  "DELETE FROM memory_recall_shadow_deltas"
                  " WHERE project = ?1"
                  "   AND created_at < to_char(CURRENT_TIMESTAMP - INTERVAL '%d days',"
                  "       'YYYY-MM-DD HH24:MI:SS')",
                  retention_days);
      else
         snprintf(sql, sizeof(sql),
                  "DELETE FROM memory_recall_shadow_deltas"
                  " WHERE created_at < to_char(CURRENT_TIMESTAMP - INTERVAL '%d days',"
                  "       'YYYY-MM-DD HH24:MI:SS')",
                  retention_days);
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         if (scoped)
            aimee_pg_bind_text(st, "?1", project);
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
            deleted += aimee_pg_stmt_changes(st);
         aimee_pg_finalize(st);
      }
   }

   /* 2) Row-cap trim: keep only the newest max_rows rows. */
   {
      char sql[768];
      if (scoped)
         snprintf(sql, sizeof(sql),
                  "DELETE FROM memory_recall_shadow_deltas"
                  " WHERE project = ?1 AND id NOT IN ("
                  "   SELECT id FROM memory_recall_shadow_deltas"
                  "   WHERE project = ?2 ORDER BY id DESC LIMIT %d)",
                  max_rows);
      else
         snprintf(sql, sizeof(sql),
                  "DELETE FROM memory_recall_shadow_deltas"
                  " WHERE id NOT IN ("
                  "   SELECT id FROM memory_recall_shadow_deltas"
                  "   ORDER BY id DESC LIMIT %d)",
                  max_rows);
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         if (scoped)
         {
            aimee_pg_bind_text(st, "?1", project);
            aimee_pg_bind_text(st, "?2", project);
         }
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
            deleted += aimee_pg_stmt_changes(st);
         aimee_pg_finalize(st);
      }
   }

   return deleted;
}
