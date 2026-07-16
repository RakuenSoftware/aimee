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
#include "kb_curator_extract.h"
#include "kb/kb_curator_sidecar.h"
#include "platform_test_util.h"
#include "db2_test_shim.h"
#include "../kb_curator_queue.h"
#include "../kb_curator_extract.h"
#include "../kb/kb_memory_facts.h"
#include "config.h"

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

static const char *job_status(sqlite3 *db, int64_t id)
{
   static char buf[64];
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT status FROM kb_async_jobs WHERE id=?1", -1, &st, NULL) ==
          SQLITE_OK);
   sqlite3_bind_int64(st, 1, id);
   assert(sqlite3_step(st) == SQLITE_ROW);
   snprintf(buf, sizeof(buf), "%s", (const char *)sqlite3_column_text(st, 0));
   sqlite3_finalize(st);
   return buf;
}

static int job_attempts(sqlite3 *db, int64_t id)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT attempts FROM kb_async_jobs WHERE id=?1", -1, &st, NULL) ==
          SQLITE_OK);
   sqlite3_bind_int64(st, 1, id);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int v = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return v;
}

/* The exact production regression: kb_curator_extract_one only ever CLAIMS
 * status='pending', so an extract_doc job orphaned in 'running' (worker crash,
 * restart, wedged sidecar) stayed there forever — one sat for 15h on the .254
 * appliance, never retried, pinning a db2 pool member past its 300s ceiling.
 * The code-unit stage had a lease reclaim from the start; kb_async_jobs had none.
 *
 * The reclaim self-throttles (it runs at most once a minute, since the drain
 * calls the entry point once per job), so this exercises ONE call and seeds every
 * case around it. max_attempts=1 makes every reclaimed row terminal, which keeps
 * the follow-on claim from re-running these jobs and muddying the assertions. */
static void test_reclaim_stale_running_extract_doc(sqlite3 *db)
{
   /* Backing docs: kb_async_jobs.document_id is a FK onto kb_documents. The ids
    * sit deliberately far above the docs seeded earlier in this process, because
    * kb_async_jobs is UNIQUE(kind, document_id) and those already hold
    * extract_doc rows. */
   seed(db, "INSERT INTO kb_documents (id,project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES (9001,'p','r1.md','rh1',0,'t','')");
   seed(db, "INSERT INTO kb_documents (id,project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES (9002,'p','r2.md','rh2',0,'t','')");
   seed(db, "INSERT INTO kb_documents (id,project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES (9003,'p','r3.md','rh3',0,'t','')");

   /* (a) stale extract_doc job, attempts exhausted -> reclaimed to 'failed'. */
   seed(db, "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,claimed_by,"
            "claimed_at,created_at,updated_at) VALUES (9001,'extract_doc',9001,'p','running',1,"
            "'kb.curator.drain',datetime('now','-60 minutes'),datetime('now','-60 minutes'),"
            "datetime('now','-60 minutes'))");

   /* (b) a job claimed just now is in-flight, NOT orphaned — must be left alone,
    *     or the reclaim would yank live work out from under a running sidecar. */
   seed(db, "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,claimed_by,"
            "claimed_at,created_at,updated_at) VALUES (9002,'extract_doc',9002,'p','running',1,"
            "'kb.curator.drain',datetime('now'),datetime('now'),datetime('now'))");

   /* (c) another kind, equally stale: kb_async_jobs is shared, and memory_facts
    *     owns its own claim lifecycle. Reclaiming it from the doc stage would be
    *     cross-stage theft. attempts is at MF_MAX_ATTEMPTS so that when its own
    *     drain reclaims it below, it lands on the terminal 'failed' branch and
    *     no follow-on claim can process it — keeping that assertion about the
    *     reclaim alone. */
   seed(db, "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,claimed_by,"
            "claimed_at,created_at,updated_at) VALUES (9003,'memory_facts',9003,'p','running',3,"
            "'kb.memory.facts',datetime('now','-60 minutes'),datetime('now','-60 minutes'),"
            "datetime('now','-60 minutes'))");

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 1;
   snprintf(opts.extract_command, sizeof(opts.extract_command), "%s", "true");
   (void)kb_curator_extract_one(&opts);

   assert(strcmp(job_status(db, 9001), "failed") == 0);  /* orphan reclaimed */
   assert(strcmp(job_status(db, 9002), "running") == 0); /* in-flight untouched */
   assert(strcmp(job_status(db, 9003), "running") == 0); /* other kind untouched */
   printf("  PASS: reclaim_stale_running (orphan reclaimed; in-flight + other kinds untouched)\n");
}

/* memory_facts shares kb_async_jobs and had the same missing-reclaim gap: its
 * claim also only selects 'pending', so a wedged LLM call stranded the job
 * forever. Job 9003 above is the stale memory_facts row the extract_doc reclaim
 * correctly refused to touch — here its OWN drain must reclaim it, which also
 * pins the kind scoping from the other side. */
static void test_reclaim_stale_running_memory_facts(sqlite3 *db)
{
   assert(strcmp(job_status(db, 9003), "running") == 0); /* still stale from above */

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.typed_facts_enabled = 1;
   (void)kb_memory_facts_drain(&cfg, 8);

   assert(strcmp(job_status(db, 9003), "failed") == 0);  /* orphan reclaimed */
   assert(strcmp(job_status(db, 9002), "running") == 0); /* extract_doc untouched */
   printf("  PASS: reclaim_stale_running_memory_facts (orphan reclaimed; other kinds untouched)\n");
}

/* Retry backoff: a failed job must not be instantly re-claimable.
 *
 * The production shape this guards: ce_mark_retry_or_fail set status='pending'
 * with no delay and ce_claim_job filtered on status alone — so a job failing for
 * a persistent reason was re-claimed on the very next poll and burned its whole
 * attempt budget in milliseconds. When a tier filled and every sidecar call
 * failed at once, ~5,300 jobs spun to 'failed' in minutes while the drain thread
 * did nothing else.
 *
 * Driven through kb_curator_extract_one (the public entry) and asserted on
 * attempts, not on the column: a next_attempt_at that is written but not honoured
 * by the claim query would be pure decoration. attempts only moves when the claim
 * actually takes the job. */
static void test_retry_backoff_defers_reclaim(sqlite3 *db)
{
   /* This drain claims the lowest-id pending job, and earlier cases leave their
    * own rows behind — park them so the only claimable job is this one, and the
    * assertions are about backoff rather than about queue ordering. */
   seed(db, "UPDATE kb_async_jobs SET status='done' WHERE status='pending'");

   seed(db, "INSERT INTO kb_documents (id,project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES (9101,'p','bo.md','boh',0,'t','')");
   /* Backoff still running: the job is pending but must be passed over. */
   seed(db,
        "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,next_attempt_at)"
        " VALUES (9101,'extract_doc',9101,'p','pending',1,'2999-01-01 00:00:00')");

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 1;
   snprintf(opts.extract_command, sizeof(opts.extract_command), "%s", "true");

   (void)kb_curator_extract_one(&opts);
   assert(job_attempts(db, 9101) == 1); /* untouched: still deferred */
   assert(strcmp(job_status(db, 9101), "pending") == 0);

   /* Backoff elapsed: claimable again. */
   seed(db, "UPDATE kb_async_jobs SET next_attempt_at='2000-01-01 00:00:00' WHERE id=9101");
   (void)kb_curator_extract_one(&opts);
   assert(job_attempts(db, 9101) == 2); /* claimed: attempts advanced */

   printf("  PASS: test_retry_backoff_defers_reclaim (deferred, then claimed)\n");
}

/* A never-failed job carries next_attempt_at='' and must always be claimable —
 * the new filter must not strand the common case. */
static void test_retry_backoff_ignores_fresh_jobs(sqlite3 *db)
{
   seed(db, "UPDATE kb_async_jobs SET status='done' WHERE status='pending'");
   seed(db, "INSERT INTO kb_documents (id,project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES (9102,'p','fresh.md','frh',0,'t','')");
   seed(db,
        "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,next_attempt_at)"
        " VALUES (9102,'extract_doc',9102,'p','pending',0,'')");

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 1;
   snprintf(opts.extract_command, sizeof(opts.extract_command), "%s", "true");

   (void)kb_curator_extract_one(&opts);
   assert(job_attempts(db, 9102) == 1); /* '' never defers */
   printf("  PASS: test_retry_backoff_ignores_fresh_jobs\n");
}

/* The delay curve itself: exponential, clamped, never zero (a zero would
 * reintroduce the instant respin this exists to prevent). */
static void test_retry_delay_curve(void)
{
   assert(kb_curator_retry_delay_seconds(1) == KB_CURATOR_RETRY_BASE_S);
   assert(kb_curator_retry_delay_seconds(2) == KB_CURATOR_RETRY_BASE_S * 2);
   assert(kb_curator_retry_delay_seconds(3) == KB_CURATOR_RETRY_BASE_S * 4);
   assert(kb_curator_retry_delay_seconds(99) == KB_CURATOR_RETRY_MAX_S);
   assert(kb_curator_retry_delay_seconds(1000000) == KB_CURATOR_RETRY_MAX_S); /* no overflow */
   assert(kb_curator_retry_delay_seconds(0) == KB_CURATOR_RETRY_BASE_S);      /* degenerate */
   assert(kb_curator_retry_delay_seconds(-5) == KB_CURATOR_RETRY_BASE_S);
   printf("  PASS: test_retry_delay_curve\n");
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

   test_reclaim_stale_running_extract_doc(db);
   test_reclaim_stale_running_memory_facts(db);
   test_retry_delay_curve();
   test_retry_backoff_defers_reclaim(db);
   test_retry_backoff_ignores_fresh_jobs(db);

   printf("test_curator_queue: all tests passed\n");
   return 0;
}
