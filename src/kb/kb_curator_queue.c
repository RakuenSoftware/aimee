/* kb_curator_queue.c: curator job-queuing layer for Phase 1 extraction.
 * Enqueues extract_doc jobs in kb_async_jobs after ingest completes.
 * Enqueues extract_code_unit rows when the curator code gate is enabled.
 *
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_queue.h"
#include "aimee.h"
#include "config.h"
#include "log.h"
#include "db2/kb_payload.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CQ_ERRBUF 256

int kb_curator_queue_docs_for_project(const char *project)
{
   if (!project || !project[0])
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_curator_extract_docs_enabled)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT id FROM kb_documents"
                            " WHERE project = ?1"
                            " AND id NOT IN ("
                            "   SELECT document_id FROM kb_async_jobs"
                            "   WHERE kind = 'extract_doc'"
                            "   AND status IN ('pending', 'running', 'done')"
                            " )";

   char err[CQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      aimee_log(LOG_WARN, "kb.curator.queue", "failed to query kb_documents for project '%s': %s",
                project, err);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", project);

   int enqueued = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int64_t doc_id = aimee_pg_column_int64(st, 0);
      int rc = db2_kb_async_enqueue("extract_doc", doc_id, project);
      if (rc > 0)
         enqueued++;
   }
   aimee_pg_finalize(st);

   if (enqueued > 0)
      aimee_log(LOG_INFO, "kb.curator.queue", "queued %d extract_doc job(s) for project '%s'",
                enqueued, project);
   return enqueued;
}

int kb_curator_queue_code_unit(const char *project, const char *file_path, const char *symbol,
                               int line)
{
   if (!project || !file_path || !symbol)
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_curator_extract_code_enabled)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO kb_code_unit_jobs"
                            " (project, file_path, symbol, line)"
                            " VALUES (?1, ?2, ?3, ?4)"
                            " ON CONFLICT DO NOTHING";

   char err[CQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      aimee_log(LOG_WARN, "kb.curator.queue", "failed to prepare code_unit insert: %s", err);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   aimee_pg_bind_text(st, "?3", symbol);
   aimee_pg_bind_int(st, "?4", line);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

int kb_curator_queue_code_units_for_project(const char *project, const char *root_path)
{
   if (!project || !project[0])
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_curator_extract_code_enabled)
      return 0;

   (void)root_path;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* NOT EXISTS (anti-join), not `(f.path, t.name) NOT IN (subquery)`: the
    * row-constructor NOT IN can't be planned as a hash anti-join (it degrades to a
    * per-row subquery scan — observed holding a pooled connection for minutes on a
    * large `terms` table) and is NULL-unsafe (a single NULL file_path/symbol in
    * kb_code_unit_jobs makes NOT IN drop ALL rows). NOT EXISTS fixes both. */
   static const char *sql = "SELECT f.path, t.name, t.kind, t.line::int"
                            " FROM terms t"
                            " JOIN files f ON t.file_id = f.id"
                            " JOIN projects p ON f.project_id = p.id"
                            " WHERE p.name = ?1"
                            " AND NOT EXISTS ("
                            "   SELECT 1 FROM kb_code_unit_jobs j"
                            "   WHERE j.project = ?1 AND j.status IN ('pending','running','done')"
                            "     AND j.file_path = f.path AND j.symbol = t.name"
                            " )";

   char err[CQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      aimee_log(LOG_WARN, "kb.curator.queue", "failed to query terms for project '%s': %s", project,
                err);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", project);

   int enqueued = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *fp = aimee_pg_column_text(st, 0);
      const char *sym = aimee_pg_column_text(st, 1);
      int ln = aimee_pg_column_int(st, 3);
      if (kb_curator_queue_code_unit(project, fp ? fp : "", sym ? sym : "", ln) == 0)
         enqueued++;
   }
   aimee_pg_finalize(st);

   if (enqueued > 0)
      aimee_log(LOG_INFO, "kb.curator.queue", "queued %d extract_code_unit job(s) for project '%s'",
                enqueued, project);
   return enqueued;
}

void kb_curator_queue_counts(kb_curator_queue_counts_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return;

   char err[CQ_ERRBUF] = "";
   /* extract_doc pending/done from kb_async_jobs (one scan). */
   static const char *sql_doc = "SELECT"
                                " COALESCE(SUM(CASE WHEN status='pending' THEN 1 ELSE 0 END),0),"
                                " COALESCE(SUM(CASE WHEN status='done' THEN 1 ELSE 0 END),0)"
                                " FROM kb_async_jobs WHERE kind='extract_doc'";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql_doc, err, sizeof(err));
   if (st)
   {
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         out->extract_pending = (int)aimee_pg_column_int64(st, 0);
         out->extract_done = (int)aimee_pg_column_int64(st, 1);
      }
      aimee_pg_finalize(st);
   }

   /* code_unit pending/done from kb_code_unit_jobs. */
   static const char *sql_code = "SELECT"
                                 " COALESCE(SUM(CASE WHEN status='pending' THEN 1 ELSE 0 END),0),"
                                 " COALESCE(SUM(CASE WHEN status='done' THEN 1 ELSE 0 END),0)"
                                 " FROM kb_code_unit_jobs";
   st = aimee_pg_prepare(conn, sql_code, err, sizeof(err));
   if (st)
   {
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         out->code_unit_pending = (int)aimee_pg_column_int64(st, 0);
         out->code_unit_done = (int)aimee_pg_column_int64(st, 1);
      }
      aimee_pg_finalize(st);
   }
}
