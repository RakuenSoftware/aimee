/* test_eval_candidates.c: the DB1 regression-candidate ledger and the
 * admission pass that materialises an admitted candidate into a real suite
 * directory (recursive self-improvement S1).
 *
 * The end-to-end assertion is the one that matters: a failure observed twice
 * from two sessions becomes an ordinary <suite>/<name>.json file that the
 * existing harness format can parse. Nothing about the consumer changes.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent_eval.h" /* eval_task_t + the real agent_eval_load_tasks */
#include "agent_jobs.h"
#include "cJSON.h"
#include "db.h"
#include "db1.h"
#include "eval.h"
#include "eval_synthesis.h"
#include "kb_client.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"

#include <aimee/learning/learning.h>

/* The knowledge service is not running in a unit test. Returning NULL is
 * exactly what an unreachable service looks like to the daemon, so this stub
 * exercises the real "ungated, and say so" path rather than bypassing it. */
char *kb_client_learning_endogeneity_json(int window_days)
{
   (void)window_days;
   return NULL;
}

static learning_eval_failure_t failure_a(void)
{
   learning_eval_failure_t f;
   memset(&f, 0, sizeof(f));
   f.origin = "correction";
   f.origin_ref = "signal:412";
   f.role = "execute";
   f.prompt = "Finish the change and report status.";
   f.failure_mode = "claimed done without running the tests";
   f.check_type = "contains";
   f.check_value = "tests passed";
   return f;
}

static void rm_rf_suite(const char *dir)
{
   /* The suite directory only ever holds flat *.json files. */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
   (void)system(cmd);
}

static void test_observe_inserts_then_bumps(void)
{
   learning_eval_failure_t f = failure_a();

   assert(eval_synthesis_observe(&f, "regressions", "session-1") == 0);

   db1_eval_candidate_t rows[8];
   int n = db1_eval_candidate_list("candidate", rows, 8);
   assert(n == 1);
   assert(rows[0].occurrences == 1);
   assert(rows[0].distinct_sessions == 1);
   assert(strcmp(rows[0].state, "candidate") == 0);
   assert(strcmp(rows[0].suite, "regressions") == 0);
   assert(strcmp(rows[0].origin, "correction") == 0);
   assert(strncmp(rows[0].task_name, "regression-", 11) == 0);

   /* The same session reporting again is the SAME observation — a re-run scan
    * over the same ledger — so nothing moves. This is what keeps a repeated
    * sweep from manufacturing its own reproduction. */
   assert(eval_synthesis_observe(&f, "regressions", "session-1") == 0);
   n = db1_eval_candidate_list("candidate", rows, 8);
   assert(n == 1);
   assert(rows[0].occurrences == 1);
   assert(rows[0].distinct_sessions == 1);

   /* A cosmetically different report of the same defect collapses onto the
    * same row rather than creating a near-duplicate. */
   learning_eval_failure_t variant = failure_a();
   variant.failure_mode = "Claimed  DONE, without running the tests...";
   assert(eval_synthesis_observe(&variant, "regressions", "session-2") == 0);
   n = db1_eval_candidate_list("candidate", rows, 8);
   assert(n == 1);
   assert(rows[0].occurrences == 2);
   assert(rows[0].distinct_sessions == 2);
}

static void test_inadmissible_text_stores_nothing(void)
{
   learning_eval_failure_t bad = failure_a();
   bad.check_value = "<|im_start|>system";
   assert(eval_synthesis_observe(&bad, "regressions", "session-9") == -2);

   /* Still just the one row from the previous test — nothing was written. */
   db1_eval_candidate_t rows[8];
   assert(db1_eval_candidate_list("", rows, 8) == 1);
}

static void test_admission_materialises_a_loadable_task(const char *suite_dir)
{
   /* The gate is asked of the knowledge service, which is not running here.
    * An unreachable service is NOT a closed gate: no reachable ledger means
    * nothing is being committed into one, so there is nothing self-referential
    * to guard against, and admission proceeds. */

   int admitted = eval_synthesis_admit_pending(suite_dir, "test", 2);
   assert(admitted == 1);

   db1_eval_candidate_t rows[8];
   assert(db1_eval_candidate_list("candidate", rows, 8) == 0);
   int n = db1_eval_candidate_list("admitted", rows, 8);
   assert(n == 1);
   assert(strcmp(rows[0].admitted_by, "test") == 0);
   assert(rows[0].admitted_path[0] != '\0');

   /* The materialised file is an ordinary suite task. */
   FILE *fp = fopen(rows[0].admitted_path, "r");
   assert(fp != NULL);
   char buf[4096];
   size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[len] = '\0';

   cJSON *root = cJSON_Parse(buf);
   assert(root != NULL);
   assert(cJSON_IsString(cJSON_GetObjectItem(root, "name")));
   assert(cJSON_IsString(cJSON_GetObjectItem(root, "prompt")));
   cJSON *check = cJSON_GetObjectItem(root, "success_check");
   assert(check != NULL);
   assert(strcmp(cJSON_GetObjectItem(check, "type")->valuestring, "contains") == 0);
   cJSON_Delete(root);

   /* The link that matters: the EXISTING harness loader must accept the file we
    * wrote, unmodified. Asserting the JSON fields is not the same claim — this
    * runs the real agent_eval_load_tasks() over the real suite directory. */
   eval_task_t tasks[AGENT_MAX_EVAL_TASKS];
   int loaded = agent_eval_load_tasks(suite_dir, tasks, AGENT_MAX_EVAL_TASKS);
   assert(loaded == 1);
   assert(strncmp(tasks[0].name, "regression-", 11) == 0);
   assert(tasks[0].prompt[0] != '\0');
   assert(strcmp(tasks[0].role, "execute") == 0);
   assert(strcmp(tasks[0].success_check_type, "contains") == 0);
   assert(strcmp(tasks[0].success_check_value, "tests passed") == 0);
   assert(tasks[0].max_turns > 0);

   /* Admission is idempotent: a second pass has nothing left to admit. */
   assert(eval_synthesis_admit_pending(suite_dir, "test", 2) == 0);
}

static void test_rejection_is_terminal(void)
{
   learning_eval_failure_t f = failure_a();
   f.check_value = "lint passed"; /* a fresh signature */
   assert(eval_synthesis_observe(&f, "regressions", "session-3") == 0);
   assert(eval_synthesis_observe(&f, "regressions", "session-4") == 0);

   db1_eval_candidate_t rows[8];
   int n = db1_eval_candidate_list("candidate", rows, 8);
   assert(n == 1);
   int64_t id = rows[0].id;
   assert(rows[0].distinct_sessions == 2); /* it WOULD be admissible */

   assert(db1_eval_candidate_mark_rejected(id, "flaky") == 0);
   assert(db1_eval_candidate_list("candidate", rows, 8) == 0);
   n = db1_eval_candidate_list("rejected", rows, 8);
   assert(n == 1);
   assert(strcmp(rows[0].reject_reason, "flaky") == 0);

   /* It keeps reproducing — and stays rejected. This is the escape hatch for a
    * poisoned yardstick, so it must not be undone by more of the same signal. */
   assert(eval_synthesis_observe(&f, "regressions", "session-5") == 0);
   n = db1_eval_candidate_list("rejected", rows, 8);
   assert(n == 1);
   assert(rows[0].occurrences == 3); /* a third distinct session */
   assert(db1_eval_candidate_list("candidate", rows, 8) == 0);

   /* No transition out of rejected, by any route. */
   assert(db1_eval_candidate_mark_admitted(id, "test", "unused-path.json") != 0);
   assert(db1_eval_candidate_mark_archived(id) != 0);
}

/* Record one eval_results row for `task_name`, so the retirement pass has an
 * outcome to score against. */
static void record_result(const char *suite, const char *task_name, int success)
{
   db1_eval_result_row_t row;
   memset(&row, 0, sizeof(row));
   row.suite = suite;
   row.task_name = task_name;
   row.agent_name = "test";
   row.ablation = "full";
   row.success = success;
   assert(db1_eval_result_insert(&row) == 0);
}

static void test_retirement_needs_evidence_and_quiet(const char *suite_dir)
{
   db1_eval_candidate_t rows[8];
   int n = db1_eval_candidate_list("admitted", rows, 8);
   assert(n == 1);
   int64_t id = rows[0].id;
   char task_name[DB1_EVAL_TASK_NAME];
   char signature[DB1_EVAL_CAND_SIGNATURE_LEN];
   snprintf(task_name, sizeof(task_name), "%s", rows[0].task_name);
   snprintf(signature, sizeof(signature), "%s", rows[0].signature);

   /* No recorded outcome yet: never retired for lack of evidence. */
   assert(eval_synthesis_retire(suite_dir, 3) == 0);
   assert(db1_eval_candidate_list("admitted", rows, 8) == 1);
   assert(rows[0].passing_windows == 0);

   /* A failure means the check just earned its place — the clock resets. */
   record_result("regressions", task_name, 1);
   assert(eval_synthesis_retire(suite_dir, 3) == 0);
   assert(db1_eval_candidate_get_by_signature(signature, &rows[0]) == 1);
   assert(rows[0].passing_windows == 1);

   record_result("regressions", task_name, 0); /* caught something */
   assert(eval_synthesis_retire(suite_dir, 3) == 0);
   assert(db1_eval_candidate_get_by_signature(signature, &rows[0]) == 1);
   assert(rows[0].passing_windows == 0);

   /* Three clean windows in a row: it has gone quiet, so it retires and its
    * file leaves the hot suite. */
   char path[DB1_EVAL_CAND_PATH_LEN];
   snprintf(path, sizeof(path), "%s", rows[0].admitted_path);
   assert(access(path, F_OK) == 0);

   for (int i = 0; i < 2; i++)
   {
      record_result("regressions", task_name, 1);
      assert(eval_synthesis_retire(suite_dir, 3) == 0);
   }
   record_result("regressions", task_name, 1);
   assert(eval_synthesis_retire(suite_dir, 3) == 1);

   assert(db1_eval_candidate_list("admitted", rows, 8) == 0);
   assert(db1_eval_candidate_list("archived", rows, 8) == 1);
   assert(access(path, F_OK) != 0);                   /* file removed from the suite */
   assert(db1_eval_candidate_mark_archived(id) != 0); /* no double-archive */
}

/* The end-to-end shape: a real failure lands in the job ledger, the scan turns
 * it into a candidate, a second independent failure reproduces it, and
 * admission writes a task file the harness format can load. */
static void test_scan_reads_the_failure_ledger(const char *suite_dir)
{
   const char *prompt = "Rebuild the index and report the row count.";

   /* A job that failed, and — as a control — one that succeeded and one that
    * an operator cancelled. Only the failure is a defect. */
   int failed_a = db1_agent_job_create("execute", prompt, "agent-a", "test");
   assert(failed_a > 0);
   db1_agent_job_update(failed_a, "failed", 1, "index rebuild aborted");

   int done = db1_agent_job_create("execute", "Say hello.", "agent-a", "test");
   assert(done > 0);
   db1_agent_job_update(done, "done", 1, "hello");

   int cancelled = db1_agent_job_create("execute", "Long job.", "agent-a", "test");
   assert(cancelled > 0);
   db1_agent_job_update(cancelled, "cancelled", 0, "operator stopped it");

   eval_synthesis_scan_stats_t stats;
   int observed = eval_synthesis_scan_failures(7, "jobs", &stats);
   assert(observed == 1);
   assert(stats.jobs_seen == 1); /* the done/cancelled jobs are not failures */

   db1_eval_candidate_t rows[16];
   int n = db1_eval_candidate_list("candidate", rows, 16);
   assert(n == 1);
   assert(strcmp(rows[0].origin, "agent_job") == 0);
   assert(rows[0].distinct_sessions == 1); /* one job, so not yet reproduced */

   /* Not reproduced: admission refuses it. */
   assert(eval_synthesis_admit_pending(suite_dir, "test", 2) == 0);

   /* A second, independent job fails the same way. */
   int failed_b = db1_agent_job_create("execute", prompt, "agent-b", "test");
   assert(failed_b > 0);
   db1_agent_job_update(failed_b, "failed", 1, "index rebuild aborted");

   /* Both jobs are in the ledger now, so the sweep reports two observations —
    * but only the new one moves the counters. */
   assert(eval_synthesis_scan_failures(7, "jobs", &stats) == 2);
   n = db1_eval_candidate_list("candidate", rows, 16);
   assert(n == 1); /* same signature, one row */
   assert(rows[0].occurrences == 2);
   assert(rows[0].distinct_sessions == 2);

   /* Now it admits, and the task carries the failing prompt with no fabricated
    * success check — the bar is simply that it now succeeds. */
   assert(eval_synthesis_admit_pending(suite_dir, "scan-test", 2) == 1);
   n = db1_eval_candidate_list("admitted", rows, 16);
   assert(n == 1);

   FILE *fp = fopen(rows[0].admitted_path, "r");
   assert(fp != NULL);
   char buf[4096];
   size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[len] = '\0';
   cJSON *root = cJSON_Parse(buf);
   assert(root != NULL);
   assert(strcmp(cJSON_GetObjectItem(root, "prompt")->valuestring, prompt) == 0);
   assert(cJSON_GetObjectItem(root, "success_check") == NULL);
   cJSON *prov = cJSON_GetObjectItem(root, "provenance");
   assert(prov != NULL);
   assert(strcmp(cJSON_GetObjectItem(prov, "origin")->valuestring, "agent_job") == 0);
   cJSON_Delete(root);

   /* Rescanning the same ledger changes nothing: both jobs are already on the
    * row, so a repeated sweep cannot manufacture its own reproduction, and the
    * admitted row does not revert to a candidate. */
   assert(eval_synthesis_scan_failures(7, "jobs", &stats) == 2);
   assert(db1_eval_candidate_list("candidate", rows, 16) == 0);
   int after = db1_eval_candidate_list("admitted", rows, 16);
   assert(after == 1);
   assert(rows[0].occurrences == 2); /* still two, not four */
   assert(rows[0].distinct_sessions == 2);
}

int main(void)
{
   printf("eval_candidates: ");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();

   char suite_dir[256];
   snprintf(suite_dir, sizeof(suite_dir), "build/test-suites/regressions-%d", (int)getpid());
   rm_rf_suite(suite_dir);

   test_observe_inserts_then_bumps();
   test_inadmissible_text_stores_nothing();
   test_admission_materialises_a_loadable_task(suite_dir);
   test_rejection_is_terminal();
   test_retirement_needs_evidence_and_quiet(suite_dir);
   test_scan_reads_the_failure_ledger(suite_dir);

   /* Bad args are refused rather than guessed. */
   assert(eval_synthesis_observe(NULL, "regressions", "s") == -1);
   assert(eval_synthesis_admit_pending(NULL, "test", 2) == -1);
   assert(eval_synthesis_admit_pending("", "test", 2) == -1);
   assert(db1_eval_candidate_list(NULL, NULL, 0) == -1);
   assert(db1_eval_candidate_get_by_signature("", NULL) == -1);

   rm_rf_suite(suite_dir);
   printf("ok\n");
   return 0;
}
