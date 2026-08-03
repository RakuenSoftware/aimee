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
#include "config_database.h" /* config_synth_chat_endpoint_current — the one resolver */
#include "index.h"           /* index_list_projects, project_info_t */
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
   if (!config_kb_curator_extract_docs_enabled())
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* structured-PDF safety: PDF chunks (doc_kind='pdf') are NEVER curated. Curator
    * extraction turns chunk content into searchable derived artifacts (claims/narrative)
    * that surface through general /v1/search and are not subject to the access-gated
    * search_chunks path or the quarantine withholding — so feeding PDF content here would
    * leak it (including restricted documents) out of the access-controlled surface. PDFs
    * are reachable only via the search_chunks tool. */
   static const char *sql = "SELECT d.id FROM kb_documents d"
                            " JOIN projects p ON p.name = d.project"
                            " WHERE d.project = ?1"
                            " AND p.lifecycle_state = 'current'"
                            " AND d.generation = p.current_generation"
                            " AND d.doc_kind <> 'pdf'"
                            " AND d.id NOT IN ("
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
   if (!config_kb_curator_extract_code_enabled())
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO kb_code_unit_jobs (project, generation, file_path, symbol, line)"
       " SELECT ?1, p.current_generation, ?2, ?3, ?4 FROM projects p"
       " WHERE p.name=?1 AND p.lifecycle_state='current'"
       " ON CONFLICT(project, generation, file_path, symbol) DO UPDATE SET"
       " line=excluded.line,"
       " updated_at=pg_now_text()";

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

int kb_curator_code_unit_jobs_delete_project(const char *project)
{
   if (!project || !project[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CQ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM kb_code_unit_jobs WHERE project = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : -1;
}

int kb_curator_queue_code_units_for_project(const char *project, const char *root_path)
{
   if (!project || !project[0])
      return 0;
   if (!config_kb_curator_extract_code_enabled())
      return 0;

   /* ENABLING A STAGE IS NOT THE SAME AS BEING ABLE TO RUN IT.
    *
    * These rows exist for exactly one consumer: extract_code, which runs the
    * synthesis sidecar. With no synthesis endpoint configured, not one of them can
    * ever reach 'done' -- and enqueuing them is not free. This runs on the
    * SYNCHRONOUS path of /v1/code/scan and inserts one row per symbol: measured on
    * a 4,018-file corpus, ~173,000 rows and 100 MB of table, adding ~215s to every
    * scan. The client's scan deadline is spent building a backlog that cannot start.
    *
    * It also compounds. The anti-join below scans kb_code_unit_jobs, so each scan
    * of each new project pays for every dead row every earlier scan left behind.
    *
    * Configured-ness, not reachability: a configured endpoint that is temporarily
    * down is a real outage, its rows are real work, and the drain's process-wide
    * provider gate already idles the queue instead of spinning. An UNCONFIGURED
    * endpoint is a steady state that no retry can resolve. Resolved through the
    * same accessor the sidecar uses, so the two cannot disagree about what an
    * operator's value means. */
   char synth_endpoint[512];
   if (!config_synth_chat_endpoint_current(synth_endpoint, sizeof(synth_endpoint)))
   {
      aimee_log(LOG_INFO, "kb.curator.queue",
                "code-unit enqueue skipped for '%s': no synthesis endpoint configured, so "
                "extract_code cannot consume these rows (set SYNTHESIS_ENDPOINT to enable)",
                project);
      return 0;
   }

   (void)root_path;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* NOT EXISTS (anti-join), not `(f.path, t.name) NOT IN (subquery)`: the
    * row-constructor NOT IN can't be planned as a hash anti-join (it degrades to a
    * per-row subquery scan — observed holding a pooled connection for minutes on a
    * large `terms` table) and is NULL-unsafe (a single NULL file_path/symbol in
    * kb_code_unit_jobs makes NOT IN drop ALL rows). NOT EXISTS fixes both. */
   /* ONE set-based INSERT, not a row-per-symbol loop. The SELECT below already
    * names exactly the rows to enqueue, so feeding them back one INSERT at a
    * time only bought a WAL fsync per symbol: each kb_curator_queue_code_unit()
    * call is its own autocommit transaction.
    *
    * That cost dominated indexing. Measured on a 4,018-file repository: the
    * enqueue produced ~183,000 rows, postgres sat in LWLock/WALWrite and
    * IO/WalSync, and the /v1/code/scan request that triggered it never answered
    * inside the client's 5-minute deadline — so scanning a mid-sized repo could
    * not complete at all. Folding the loop into the statement makes it one
    * transaction and one commit.
    *
    * DISTINCT ON is load-bearing, and is the one behaviour change the fold
    * requires: postgres refuses "ON CONFLICT DO UPDATE" that touches the same
    * row twice in a single statement, and `terms` can legitimately carry the
    * same symbol name twice for one file (two definitions, different lines).
    * Row-at-a-time tolerated that because each insert was its own statement.
    * Ordering by line makes the survivor the first definition, deterministically,
    * rather than whichever row the scan happened to emit first.
    *
    * NOT EXISTS (anti-join), not `(f.path, t.name) NOT IN (subquery)`: the
    * row-constructor NOT IN can't be planned as a hash anti-join (it degrades to a
    * per-row subquery scan — observed holding a pooled connection for minutes on a
    * large `terms` table) and is NULL-unsafe (a single NULL file_path/symbol in
    * kb_code_unit_jobs makes NOT IN drop ALL rows). NOT EXISTS fixes both. */
   static const char *sql =
       "INSERT INTO kb_code_unit_jobs (project, generation, file_path, symbol, line)"
       " SELECT DISTINCT ON (f.path, t.name)"
       "        p.name, p.current_generation, f.path, t.name, t.line::int"
       " FROM terms t"
       " JOIN files f ON t.file_id = f.id"
       " JOIN projects p ON f.project_id = p.id"
       " WHERE p.name = ?1 AND p.lifecycle_state = 'current'"
       " AND f.generation = p.current_generation"
       " AND NOT EXISTS ("
       "   SELECT 1 FROM kb_code_unit_jobs j"
       "   WHERE j.project = ?1 AND j.status IN ('pending','running','done')"
       "     AND j.generation = p.current_generation"
       "     AND j.file_path = f.path AND j.symbol = t.name"
       " )"
       " ORDER BY f.path, t.name, t.line"
       " ON CONFLICT(project, generation, file_path, symbol) DO UPDATE SET"
       " line=excluded.line,"
       " updated_at=pg_now_text()";

   char err[CQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      aimee_log(LOG_WARN, "kb.curator.queue", "failed to prepare code-unit enqueue for '%s': %s",
                project, err);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", project);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   if (rc != AIMEE_PG_DONE)
   {
      aimee_log(LOG_WARN, "kb.curator.queue", "code-unit enqueue failed for project '%s': %s",
                project, err);
      aimee_pg_finalize(st);
      return -1;
   }
   int enqueued = aimee_pg_stmt_changes(st);
   if (enqueued < 0)
      enqueued = 0;
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
   /* A terminally failed job is neither 'pending' nor 'done', so counting only
    * those two made a curator that failed EVERY job indistinguishable from one
    * with nothing to do. Do not key this on last_error alone: successful jobs do
    * not currently clear a prior retry error, and would warn forever. */
   static const char *sql_doc = "SELECT"
                                " COALESCE(SUM(CASE WHEN status='pending' THEN 1 ELSE 0 END),0),"
                                " COALESCE(SUM(CASE WHEN status='done' THEN 1 ELSE 0 END),0),"
                                " COALESCE(SUM(CASE WHEN status='failed' THEN 1 ELSE 0 END),0)"
                                " FROM kb_async_jobs j"
                                " JOIN kb_documents d ON d.id=j.document_id"
                                " JOIN projects p ON p.name=d.project"
                                " WHERE j.kind='extract_doc'"
                                " AND p.lifecycle_state='current'"
                                " AND d.generation=p.current_generation";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql_doc, err, sizeof(err));
   if (st)
   {
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         out->extract_pending = (int)aimee_pg_column_int64(st, 0);
         out->extract_done = (int)aimee_pg_column_int64(st, 1);
         out->extract_failing = (int)aimee_pg_column_int64(st, 2);
      }
      aimee_pg_finalize(st);
   }

   /* code_unit pending/done from kb_code_unit_jobs. */
   static const char *sql_code = "SELECT"
                                 " COALESCE(SUM(CASE WHEN status='pending' THEN 1 ELSE 0 END),0),"
                                 " COALESCE(SUM(CASE WHEN status='done' THEN 1 ELSE 0 END),0),"
                                 " COALESCE(SUM(CASE WHEN status='failed' THEN 1 ELSE 0 END),0)"
                                 " FROM kb_code_unit_jobs j"
                                 " JOIN projects p ON p.name=j.project"
                                 " WHERE p.lifecycle_state='current'"
                                 " AND j.generation=p.current_generation";
   st = aimee_pg_prepare(conn, sql_code, err, sizeof(err));
   if (st)
   {
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         out->code_unit_pending = (int)aimee_pg_column_int64(st, 0);
         out->code_unit_done = (int)aimee_pg_column_int64(st, 1);
         out->code_unit_failing = (int)aimee_pg_column_int64(st, 2);
      }
      aimee_pg_finalize(st);
   }

   /* Return the newest terminal failure across both curator queues. Selecting
    * MAX(last_error) chose the lexicographically greatest message, not the most
    * recent one, and looking only at code-unit jobs hid extract failures (the
    * live release candidate currently has exactly that shape). Pending or done
    * jobs may retain historical retry text, so status='failed' is mandatory. */
   static const char *sql_last_error =
       "SELECT last_error FROM ("
       " SELECT j.last_error,j.updated_at,j.id,0 AS source_order FROM kb_async_jobs j"
       " JOIN kb_documents d ON d.id=j.document_id"
       " JOIN projects p ON p.name=d.project"
       " WHERE j.kind='extract_doc' AND j.status='failed' AND j.last_error<>''"
       " AND p.lifecycle_state='current' AND d.generation=p.current_generation"
       " UNION ALL"
       " SELECT j.last_error,j.updated_at,j.id,1 AS source_order FROM kb_code_unit_jobs j"
       " JOIN projects p ON p.name=j.project"
       " WHERE j.status='failed' AND j.last_error<>''"
       " AND p.lifecycle_state='current' AND j.generation=p.current_generation"
       ") curator_failures"
       " ORDER BY updated_at DESC,id DESC,source_order DESC LIMIT 1";
   st = aimee_pg_prepare(conn, sql_last_error, err, sizeof(err));
   if (st)
   {
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *le = aimee_pg_column_text(st, 0);
         if (le && le[0])
         {
            snprintf(out->last_error, sizeof(out->last_error), "%s", le);
            out->last_error_len = (int)strlen(out->last_error);
         }
      }
      aimee_pg_finalize(st);
   }
}

void kb_curator_queue_docs_all_projects(int extract_docs_enabled)
{
   if (!extract_docs_enabled)
      return;
   /* The autonomous project sweeps support ordinary multi-project servers and
    * the 150-project standing benchmark.  The former 128-entry array silently
    * omitted every later project forever. */
   project_info_t projects[512];
   int np = index_list_projects(projects, (int)(sizeof(projects) / sizeof(projects[0])));
   for (int i = 0; i < np; i++)
      (void)kb_curator_queue_docs_for_project(projects[i].name);
}
