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
#include "kb_curator_provider.h"
#include "kb_curator_sidecar.h"
#include "platform_test_util.h"
#include "db2_test_shim.h"
#include "db2/kb_payload.h"
#include "../kb_curator_queue.h"
#include "../kb_curator_extract.h"
#include "../kb/kb_memory_facts.h"
#include "config.h"

/* An upstream outage must not be charged to the job. The gateway opens a circuit
 * breaker and 503s everything for its cooldown, so three claims inside one
 * cooldown used to drive a job to terminal 'failed' — permanent data loss from a
 * condition that heals in a minute. Observed live: every failed row at exactly
 * attempts=3, last_error "provider HTTP 503".
 *
 * Classification is what decides retry-vs-terminal, so pin it directly: a
 * provider-availability failure is distinguishable from a real job failure. */
static void test_provider_unavailable_is_not_a_job_failure(void)
{
   /* The breaker's own 503, and the two other ways the upstream declines. */
   assert(kb_curator_error_is_provider_unavailable("provider HTTP 503"));
   assert(kb_curator_error_is_provider_unavailable("provider HTTP 429"));
   assert(kb_curator_error_is_provider_unavailable("provider HTTP -1"));
   /* Exact text emitted by the bundled curator-extract.py -> llm-chat.py path. */
   assert(kb_curator_error_is_provider_unavailable(
       "llm-chat.py exit 1: llm-chat: HTTP 503 from http://aimee-llm:8742/v1/chat/completions"));
   assert(kb_curator_error_is_provider_unavailable(
       "{\"error\": {\"code\": \"provider_unavailable\", \"message\": \"synth upstream "
       "circuit is open\"}}"));
   /* Exact timeout envelope emitted by llm-chat.py on a failed upstream
    * request. Live regression: this used to spend attempt 3/3 and permanently
    * fail the code-unit row even though the provider, not the row, was broken. */
   assert(kb_curator_error_is_provider_unavailable(
       "llm-chat.py exit 1: llm-chat: request to http://aimee-llm:8742/v1/chat/completions "
       "failed after 1 tries: timed out"));

   /* A 4xx that is ABOUT the request, a malformed reply, and a missing document
    * are all real job failures: retrying them forever would be the poison-job
    * loop the attempt budget exists to stop. */
   /* A missing SYNTHESIS_ENDPOINT is global, not per-row: no job can succeed
    * until an operator configures it, so it must open the provider circuit
    * rather than burn an attempt budget per symbol. Unmatched, this forked a
    * sidecar per symbol x3 attempts and pinned the box for hours. */
   assert(kb_curator_error_is_provider_unavailable(
       "sidecar exited 1: no synthesis endpoint configured: set SYNTHESIS_ENDPOINT"));

   assert(!kb_curator_error_is_provider_unavailable("provider HTTP 400"));
   assert(!kb_curator_error_is_provider_unavailable("provider HTTP 422"));
   assert(!kb_curator_error_is_provider_unavailable("sidecar returned non-JSON"));
   assert(!kb_curator_error_is_provider_unavailable("kb_documents row not found"));
   assert(!kb_curator_error_is_provider_unavailable("artifact write failed"));
   assert(!kb_curator_error_is_provider_unavailable("local parser timed out"));

   /* Absent/empty error text must not be guessed into a retry. */
   assert(!kb_curator_error_is_provider_unavailable(NULL));
   assert(!kb_curator_error_is_provider_unavailable(""));
   printf("  PASS: provider-unavailable is classified apart from job failure\n");
}

static void test_provider_outage_arms_global_backoff(void)
{
   kb_curator_provider_backoff_recovered();
   assert(!kb_curator_provider_backoff_active());
   kb_curator_provider_backoff_note();
   assert(kb_curator_provider_backoff_active());
   kb_curator_provider_backoff_recovered();
   assert(!kb_curator_provider_backoff_active());
   printf("  PASS: provider outage arms process-wide LLM-lane backoff\n");
}

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

static void test_polymorphic_async_subject(sqlite3 *db)
{
   /* memory_facts uses a memories.id as the queue subject. It must not require
    * an unrelated kb_documents row with the same numeric id. */
   assert(db2_kb_async_enqueue("memory_facts", 777777, "memory") == 0);
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db,
                             "SELECT count(*) FROM kb_async_jobs"
                             " WHERE kind='memory_facts' AND document_id=777777",
                             -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(sqlite3_column_int(st, 0) == 1);
   sqlite3_finalize(st);
   seed(db, "DELETE FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=777777");
   printf("  PASS: polymorphic async subject accepts a memory id without a document row\n");
}

/* A backlog nothing will drain has to be countable, and countable SEPARATELY from
 * work already finished.
 *
 * memory.store enqueues a memory_facts job whenever typed_facts is on (the
 * default); the only consumer runs on the curator LLM lane, which does not start
 * without a synthesis endpoint. On an install with no synth provider -- a
 * supported configuration -- that queue grows by one row per stored memory and is
 * never claimed. Observed on the e2e VM: 4 pending for 11.5 hours, attempts=0,
 * while health reported ok with an empty warnings array.
 *
 * The health surface reports it, and to do that it needs PENDING, not total:
 * total cannot tell a queue that is draining from one that never will. */
static void test_pending_count_excludes_finished(sqlite3 *db)
{
   seed(db, "DELETE FROM kb_async_jobs WHERE kind='memory_facts'");
   assert(db2_kb_async_count_kind_pending("memory_facts") == 0);

   assert(db2_kb_async_enqueue("memory_facts", 881001, "memory") == 0);
   assert(db2_kb_async_enqueue("memory_facts", 881002, "memory") == 0);
   assert(db2_kb_async_count_kind_pending("memory_facts") == 2);
   assert(db2_kb_async_count_kind("memory_facts") == 2);

   /* One finishes. The backlog is 1, even though two rows exist -- reporting 2
    * here would keep warning about work that already completed. */
   seed(db, "UPDATE kb_async_jobs SET status='done' WHERE kind='memory_facts'"
            " AND document_id=881001");
   assert(db2_kb_async_count_kind_pending("memory_facts") == 1);
   assert(db2_kb_async_count_kind("memory_facts") == 2);

   /* Other kinds are not counted into this backlog: kb_async_jobs is shared. */
   assert(db2_kb_async_enqueue("extract_doc", 881003, "p") == 0);
   assert(db2_kb_async_count_kind_pending("memory_facts") == 1);

   seed(db, "DELETE FROM kb_async_jobs WHERE document_id IN (881001,881002,881003)");
   printf("  PASS: pending count is the backlog, not the row total\n");
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
   /* Backing docs preserve the semantic validity of the extract_doc fixtures.
    * The ids sit deliberately far above docs seeded earlier in this process,
    * because kb_async_jobs is UNIQUE(kind, document_id). */
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
   (void)kb_memory_facts_drain(8);

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
            " VALUES (9401,'p','bo.md','boh',0,'t','')");
   /* Backoff still running: the job is pending but must be passed over. */
   seed(db,
        "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,next_attempt_at)"
        " VALUES (9401,'extract_doc',9401,'p','pending',1,'2999-01-01 00:00:00')");

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 1;
   snprintf(opts.extract_command, sizeof(opts.extract_command), "%s", "true");

   (void)kb_curator_extract_one(&opts);
   assert(job_attempts(db, 9401) == 1); /* untouched: still deferred */
   assert(strcmp(job_status(db, 9401), "pending") == 0);

   /* Backoff elapsed: claimable again. */
   seed(db, "UPDATE kb_async_jobs SET next_attempt_at='2000-01-01 00:00:00' WHERE id=9401");
   (void)kb_curator_extract_one(&opts);
   assert(job_attempts(db, 9401) == 2); /* claimed: attempts advanced */

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

/* The behaviour that matters: a job already AT its attempt limit must survive the
 * provider being down. Before the fix this row went terminal 'failed' and the
 * document left the pipeline for good. */
static void test_provider_outage_requeues(sqlite3 *db)
{
   seed(db, "INSERT INTO kb_documents (id,project,file_path,file_hash,chunk_index,content,doc_kind)"
            " VALUES (9601,'p','out.md','oh1',0,'t','')");
   /* attempts=3 is the exhausted budget: the next terminal decision fails it. */
   seed(db, "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,claimed_by,"
            "claimed_at,created_at,updated_at) VALUES (9601,'extract_doc',9601,'p','running',3,"
            "'kb.curator.drain',datetime('now'),datetime('now'),datetime('now'))");

   kb_curator_mark_retry_provider_unavailable(9601, 3, "provider HTTP 503");

   /* Retryable, not terminal — the outage says nothing about this document. */
   assert(strcmp(job_status(db, 9601), "pending") == 0);
   /* The claim's increment is given back, so a long outage cannot walk the
    * budget to exhaustion one refused claim at a time. */
   assert(job_attempts(db, 9601) == 2);
   printf("  PASS: provider outage requeues at attempt limit without spending budget\n");
}

static void test_only_current_document_generation_queues(sqlite3 *db)
{
   int before = jobs(db);
   seed(db, "UPDATE projects SET current_generation=2 WHERE name='p'");
   seed(db, "INSERT INTO kb_documents"
            " (project,generation,file_path,file_hash,chunk_index,content,doc_kind) VALUES"
            " ('p',1,'stale.md','stale',0,'old',''),"
            " ('p',2,'current.md','current',0,'new','')");
   (void)kb_curator_queue_docs_for_project("p");
   assert(jobs(db) == before + 1);
   printf("  PASS: only current-generation documents enter the curator queue\n");
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

   /* Curator work is generation-fenced: queued documents are eligible only
    * while their stable project and generation are current. */
   seed(db, "INSERT INTO projects (name,root,scanned_at)"
            " VALUES ('p','/p','2026-01-01 00:00:00')");

   test_polymorphic_async_subject(db);
   test_pending_count_excludes_finished(db);

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
   test_provider_unavailable_is_not_a_job_failure();
   test_provider_outage_arms_global_backoff();
   test_provider_outage_requeues(db);
   test_only_current_document_generation_queues(db);

   printf("test_curator_queue: all tests passed\n");
   return 0;
}
