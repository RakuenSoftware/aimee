/* test_corpus_jobs.c: staged corpus processing pipeline. */

#include "corpus_jobs.h"
#include "db2_internal.h"
#include "db2_test_shim.h"
#include "db_postgres.h"
#include "kb_docs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

static int query_int(const char *sql)
{
   char err[512] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int out = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return out;
}

static void test_doc_write_seeds_job_and_version(void)
{
   open_db();

   int was_existing = 0;
   int64_t doc_id = db2_kb_doc_write("seed-hash", "docs/sample.md", "global", "passthrough", "",
                                     "# Sample\n", &was_existing);
   assert(doc_id > 0);
   assert(was_existing == 0);

   db2_corpus_job_t job;
   assert(db2_corpus_job_get(doc_id, &job) == 0);
   assert(strcmp(job.stage, "ingested") == 0);
   assert(strcmp(job.stage_status, "pending") == 0);
   assert(strcmp(job.content_hash, "seed-hash") == 0);
   assert(query_int("SELECT COUNT(*) FROM document_versions WHERE is_current = 1") == 1);

   int64_t same_id = db2_kb_doc_write("seed-hash", "docs/sample.md", "global", "passthrough", "",
                                      "# Sample\n", &was_existing);
   assert(same_id == doc_id);
   assert(was_existing == 1);
   assert(query_int("SELECT COUNT(*) FROM corpus_processing_jobs") == 1);

   int64_t next_id = db2_kb_doc_write("seed-hash-2", "docs/sample.md", "global", "passthrough", "",
                                      "# Sample v2\n", NULL);
   assert(next_id > 0);
   assert(next_id != doc_id);
   assert(query_int(
              "SELECT COUNT(*) FROM document_versions WHERE doc_key = 'global:docs/sample.md'") ==
          2);
   assert(query_int("SELECT COUNT(*) FROM document_versions WHERE doc_key = 'global:docs/sample.md'"
                    " AND is_current = 1 AND doc_id > 1") == 1);
   assert(query_int("SELECT MAX(version_no) FROM document_versions"
                    " WHERE doc_key = 'global:docs/sample.md'") == 2);
   assert(query_int("SELECT COUNT(*) FROM document_versions WHERE doc_key = 'global:docs/sample.md'"
                    " AND is_current = 0 AND superseded_at != ''") == 1);

   close_db();
   printf("  doc_write_seeds_job_and_version: ok\n");
}

static void test_drain_advances_to_complete_with_events(void)
{
   open_db();

   int64_t doc_id =
       db2_kb_doc_write("drain-hash", "docs/proposals/pending/demo.md", "global", "passthrough", "",
                        "# Demo\n\nSee missing.md.\n\n## Approach\n\nText.\n", NULL);
   assert(doc_id > 0);

   db2_corpus_pipeline_stats_t stats;
   assert(db2_corpus_pipeline_status(&stats) == 0);
   assert(stats.pending == 1);
   assert(stats.complete == 0);
   db2_corpus_pipeline_stage_count_t counts[8];
   int ncounts = db2_corpus_pipeline_stage_counts(counts, 8);
   assert(ncounts == 1);
   assert(strcmp(counts[0].stage, "ingested") == 0);
   assert(strcmp(counts[0].stage_status, "pending") == 0);
   assert(counts[0].count == 1);

   assert(db2_corpus_pipeline_drain(0, &stats) == 0);
   assert(stats.processed >= 14);
   assert(stats.pending == 0);
   assert(stats.failed == 0);
   assert(stats.complete == 1);

   db2_corpus_job_t job;
   assert(db2_corpus_job_get(doc_id, &job) == 0);
   assert(strcmp(job.stage, "complete") == 0);
   assert(strcmp(job.stage_status, "complete") == 0);
   assert(query_int("SELECT COUNT(*) FROM corpus_stage_events WHERE doc_id = 1") >= 14);
   assert(query_int("SELECT COUNT(*) FROM corpus_stage_events WHERE outcome = 'skipped'") > 0);
   assert(query_int("SELECT COUNT(*) FROM document_sections WHERE doc_id = 1") == 2);
   assert(query_int("SELECT COUNT(*) FROM document_references WHERE from_doc_id = 1") >= 1);

   close_db();
   printf("  drain_advances_to_complete_with_events: ok\n");
}

static void test_failure_and_running_recovery(void)
{
   open_db();

   int64_t doc_id = db2_kb_doc_write("recover-hash", "docs/recover.md", "global", "passthrough", "",
                                     "# Recover\n", NULL);
   assert(doc_id > 0);
   assert(db2_corpus_job_fail(doc_id, "boom") == 0);
   db2_corpus_pipeline_stats_t stats;
   assert(db2_corpus_pipeline_status(&stats) == 0);
   assert(stats.failed == 1);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "UPDATE corpus_processing_jobs SET stage_status = 'running' WHERE doc_id = ?1",
       err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_int64(st, "?1", doc_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);

   assert(db2_corpus_job_recover_running(0) == 1);
   db2_corpus_job_t job;
   assert(db2_corpus_job_get(doc_id, &job) == 0);
   assert(strcmp(job.stage_status, "pending") == 0);

   close_db();
   printf("  failure_and_running_recovery: ok\n");
}

static void test_restoration_candidate_queue(void)
{
   open_db();

   int64_t doc_id = db2_kb_doc_write("restore-hash", "docs/damaged.md", "global", "passthrough", "",
                                     "damaged fragment", NULL);
   assert(doc_id > 0);

   assert(db2_corpus_job_mark_restoration_candidate(doc_id, "restore-hash",
                                                    "[\"FLAT_TEXT\",\"UNDERSIZED_CHUNKS\"]") == 0);

   db2_corpus_job_t job;
   assert(db2_corpus_job_get(doc_id, &job) == 0);
   assert(strcmp(job.stage, "restore") == 0);
   assert(strcmp(job.stage_status, "pending") == 0);
   assert(strcmp(job.content_hash, "restore-hash") == 0);
   assert(query_int("SELECT COUNT(*) FROM corpus_stage_events"
                    " WHERE to_stage = 'restore' AND outcome = 'restoration_candidate'"
                    " AND detail LIKE '%UNDERSIZED_CHUNKS%'") == 1);

   close_db();
   printf("  restoration_candidate_queue: ok\n");
}

int main(void)
{
   printf("corpus_jobs:\n");
   test_doc_write_seeds_job_and_version();
   test_drain_advances_to_complete_with_events();
   test_failure_and_running_recovery();
   test_restoration_candidate_queue();
   printf("corpus_jobs: all tests passed\n");
   return 0;
}
