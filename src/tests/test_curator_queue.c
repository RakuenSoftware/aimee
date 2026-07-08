/* test_curator_queue.c: extract_doc job queueing — regression guard for the
 * doc-curation pipeline. Ingested docs MUST get extract_doc jobs, and the drain
 * MUST backfill docs that arrived via the drain (kb_doc_refresh), not only the
 * ingest route. See kb_curator_queue.c + kb_curator_drain.c.
 *
 * (Asserts on the observable side effect — rows in kb_async_jobs — not the
 * function return, whose exact value is a db2-backend detail.) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

#include "aimee.h"
#include "platform_test_util.h"
#include "db2_test_shim.h"
#include "../kb_curator_queue.h"

static sqlite3 *open_db(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   return db;
}
static void seed(sqlite3 *db, const char *sql)
{
   char *e = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &e) != SQLITE_OK)
   {
      fprintf(stderr, "seed failed: %s\n  sql: %s\n", e ? e : "?", sql);
      assert(0);
   }
}
static int jobs(sqlite3 *db)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM kb_async_jobs WHERE kind='extract_doc'", -1,
                             &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

int main(void)
{
   /* Deterministic config: HOME with no aimee.yaml -> config_load (called inside
    * the queue) returns built-in defaults (extract_docs default-ON). */
   platform_setenv("HOME", "/tmp");
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   printf("test_curator_queue:\n");
   sqlite3 *db = open_db();

   /* Contract: every non-pdf doc gets one extract_doc job; PDFs are excluded;
    * re-running enqueues nothing. Guard for "docs present but zero jobs". */
   seed(db, "INSERT INTO kb_documents (project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES ('p','a.md','h1',0,'t','')");
   seed(db, "INSERT INTO kb_documents (project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES ('p','b.md','h2',0,'t','')");
   seed(db, "INSERT INTO kb_documents (project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES ('p','c.pdf','h3',0,'t','pdf')");
   kb_curator_queue_docs_for_project("p");
   assert(jobs(db) == 2); /* a.md + b.md; c.pdf excluded */
   kb_curator_queue_docs_for_project("p");
   assert(jobs(db) == 2); /* idempotent: no duplicates */
   printf("  PASS: queue_docs_for_project (docs->jobs, pdf-excluded, idempotent)\n");

   /* The exact regression: the drain backfill sweeps indexed projects and queues
    * their docs. A drain-ingested doc (kb_documents + projects, no ingest hook)
    * MUST be curated; the sweep is a no-op when disabled. */
   seed(db, "INSERT INTO projects (name,root,scanned_at)"
            " VALUES ('proj','/r','2026-01-01 00:00:00')");
   seed(db, "INSERT INTO kb_documents (project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES ('proj','x.md','h4',0,'t','')");
   kb_curator_queue_docs_all_projects(0);
   assert(jobs(db) == 2); /* disabled: no new jobs */
   kb_curator_queue_docs_all_projects(1);
   assert(jobs(db) == 3); /* +proj/x.md */
   printf("  PASS: queue_docs_all_projects (drain backfill queues indexed docs; disabled=no-op)\n");

   printf("test_curator_queue: all tests passed\n");
   return 0;
}
