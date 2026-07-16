#include <assert.h>
#include "platform_test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include "aimee.h"
#include "db.h"
#include "db_schema.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "db1.h"
#include "delegate_economics.h"
#include "cJSON.h"

static char g_db_path[256];

static sqlite3 *setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "/tmp/aimee-coord-jobs-XXXXXX");
   int fd = mkstemp(g_db_path);
   assert(fd >= 0);
   close(fd);

   sqlite3 *db = NULL;
   assert(sqlite3_open(g_db_path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);
   assert(db1_init(g_db_path) == 0);
   return db;
}

static void teardown(sqlite3 *db)
{
   db1_shutdown();
   db1_stmt_cache_clear();
   sqlite3_close(db);
   if (g_db_path[0])
   {
      platform_test_remove_sqlite(g_db_path);
      g_db_path[0] = '\0';
   }
}

/* Helper: create a minimal plan for testing */
static int create_test_plan(sqlite3 *db)
{
   cJSON *steps = cJSON_CreateArray();
   cJSON *s1 = cJSON_CreateObject();
   cJSON_AddStringToObject(s1, "action", "Edit src/foo.c");
   cJSON_AddItemToArray(steps, s1);
   cJSON *s2 = cJSON_CreateObject();
   cJSON_AddStringToObject(s2, "action", "Edit src/bar.c");
   cJSON_AddItemToArray(steps, s2);
   cJSON *s3 = cJSON_CreateObject();
   cJSON_AddStringToObject(s3, "action", "Run tests");
   cJSON_AddItemToArray(steps, s3);

   int plan_id = db1_execution_plan_create("test-agent", "test task", steps);
   cJSON_Delete(steps);
   return plan_id;
}

static void make_handoff(char *buf, size_t len, const char *file)
{
   snprintf(buf, len,
            "{"
            "\"schema_version\":\"delegate_result_v1\","
            "\"status\":\"done\","
            "\"changed_files\":[\"%s\"],"
            "\"tests\":[{\"name\":\"unit\",\"status\":\"passed\"}],"
            "\"supervisor_actions\":[],"
            "\"summary\":\"done\""
            "}",
            file);
}

/* --- Test: basic job creation --- */

static void test_job_create(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   assert(plan_id > 0);

   int job_id = db1_coord_job_create(plan_id, 3);
   assert(job_id > 0);

   db1_coord_job_t job;
   int rc = db1_coord_job_get(job_id, &job);
   assert(rc == 0);
   assert(job.id == job_id);
   assert(job.plan_id == plan_id);
   assert(strcmp(job.status, "pending") == 0);
   assert(job.max_concurrent == 3);
   assert(job.total_tasks == 0); /* no tasks added yet */

   teardown(db);
   printf("  PASS: test_job_create\n");
}

/* --- Test: add tasks and list them --- */

static void test_add_and_list_tasks(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);
   assert(job_id > 0);

   int t1 =
       db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "code", "do it", "/wt", "architect");
   int t2 = db1_coord_job_add_task(job_id, 0, "[\"src/bar.c\"]", "", "", "", "engineer");
   int t3 = db1_coord_job_add_task(job_id, 0, "[\"src/baz.c\"]", "", "", "", "engineer");
   assert(t1 > 0 && t2 > 0 && t3 > 0);

   db1_coord_task_t tasks[10];
   int count = db1_coord_job_list_tasks(job_id, tasks, 10);
   assert(count == 3);
   assert(strcmp(tasks[0].status, "pending") == 0);

   /* The persona named at enqueue must round-trip through the dispatch read — this
    * is what lets the workflow engine (or the coord planner) pick a per-task
    * delegate identity instead of the dispatcher forcing 'engineer'. */
   char role[64] = "", prompt[64] = "", files[128] = "", cwd[64] = "", persona[64] = "";
   assert(db1_coord_task_get_dispatch(t1, role, sizeof role, prompt, sizeof prompt, files,
                                      sizeof files, cwd, sizeof cwd, persona, sizeof persona) == 0);
   assert(strcmp(persona, "architect") == 0);
   assert(strcmp(role, "code") == 0);

   db1_coord_job_t job;
   db1_coord_job_get(job_id, &job);
   assert(job.total_tasks == 3);

   teardown(db);
   printf("  PASS: test_add_and_list_tasks\n");
}

/* --- Test: claim logic with no conflicts --- */

static void test_claim_no_conflict(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/bar.c\"]", "", "", "", "engineer");

   db1_coord_task_t task;
   int tid = db1_coord_job_claim_next(job_id, "delegate-1", &task);
   assert(tid > 0);
   assert(strcmp(task.status, "claimed") == 0);
   assert(strcmp(task.claimed_by, "delegate-1") == 0);

   /* Second claim should work (different files) */
   db1_coord_task_t task2;
   int tid2 = db1_coord_job_claim_next(job_id, "delegate-2", &task2);
   assert(tid2 > 0);
   assert(tid2 != tid);
   assert(strcmp(task2.claimed_by, "delegate-2") == 0);

   /* No more tasks to claim */
   db1_coord_task_t task3;
   int tid3 = db1_coord_job_claim_next(job_id, "delegate-3", &task3);
   assert(tid3 < 0);

   teardown(db);
   printf("  PASS: test_claim_no_conflict\n");
}

/* --- Test: file conflict prevents claim --- */

static void test_file_conflict(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   /* Two tasks touch the same file */
   db1_coord_job_add_task(job_id, 0, "[\"src/shared.c\",\"src/foo.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/shared.c\",\"src/bar.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/independent.c\"]", "", "", "", "engineer");

   /* Claim first task */
   db1_coord_task_t task1;
   int tid1 = db1_coord_job_claim_next(job_id, "delegate-1", &task1);
   assert(tid1 > 0);

   /* Second task conflicts with first (shared.c), so should skip to third */
   db1_coord_task_t task2;
   int tid2 = db1_coord_job_claim_next(job_id, "delegate-2", &task2);
   assert(tid2 > 0);
   /* Should get the independent task, not the conflicting one */
   assert(strstr(task2.files, "independent.c") != NULL);

   /* Third claim should fail - only conflicting task remains */
   db1_coord_task_t task3;
   int tid3 = db1_coord_job_claim_next(job_id, "delegate-3", &task3);
   assert(tid3 < 0);

   teardown(db);
   printf("  PASS: test_file_conflict\n");
}

/* --- Test: has_file_conflict check --- */

static void test_has_file_conflict(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");

   /* Claim it */
   db1_coord_task_t task;
   db1_coord_job_claim_next(job_id, "delegate-1", &task);

   /* Check conflicts */
   assert(db1_coord_job_has_file_conflict(job_id, "[\"src/foo.c\"]") == 1);
   assert(db1_coord_job_has_file_conflict(job_id, "[\"src/bar.c\"]") == 0);
   assert(db1_coord_job_has_file_conflict(job_id, "[\"src/foo.c\",\"src/bar.c\"]") == 1);

   teardown(db);
   printf("  PASS: test_has_file_conflict\n");
}

/* --- Test: complete task and auto-refresh status --- */

static void test_complete_task(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/bar.c\"]", "", "", "", "engineer");

   /* Claim and complete first task */
   db1_coord_task_t task;
   int tid = db1_coord_job_claim_next(job_id, "delegate-1", &task);
   assert(tid > 0);

   int rc = db1_coord_job_complete_task(tid, "success");
   assert(rc == 0);

   /* Job should be running (still has pending tasks) */
   db1_coord_job_t job;
   db1_coord_job_get(job_id, &job);
   assert(strcmp(job.status, "running") == 0);
   assert(job.done_tasks == 1);

   /* Claim and complete second task */
   int tid2 = db1_coord_job_claim_next(job_id, "delegate-2", &task);
   assert(tid2 > 0);
   rc = db1_coord_job_complete_task(tid2, "success");
   assert(rc == 0);

   /* Job should now be complete */
   db1_coord_job_get(job_id, &job);
   assert(strcmp(job.status, "complete") == 0);
   assert(job.done_tasks == 2);

   teardown(db);
   printf("  PASS: test_complete_task\n");
}

/* --- Test: fail task doesn't block others --- */

static void test_fail_task(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/bar.c\"]", "", "", "", "engineer");

   /* Claim and fail first task */
   db1_coord_task_t task;
   int tid = db1_coord_job_claim_next(job_id, "delegate-1", &task);
   int rc = db1_coord_job_fail_task(tid, "compilation error");
   assert(rc == 0);

   /* Second task should still be claimable */
   db1_coord_task_t task2;
   int tid2 = db1_coord_job_claim_next(job_id, "delegate-2", &task2);
   assert(tid2 > 0);

   rc = db1_coord_job_complete_task(tid2, "success");
   assert(rc == 0);

   /* Job should be complete (with failures) */
   db1_coord_job_t job;
   db1_coord_job_get(job_id, &job);
   assert(strcmp(job.status, "complete") == 0);
   assert(job.done_tasks == 1);
   assert(job.failed_tasks == 1);

   teardown(db);
   printf("  PASS: test_fail_task\n");
}

/* --- Test: release claim --- */

static void test_release_claim(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");

   /* Claim then release */
   db1_coord_task_t task;
   int tid = db1_coord_job_claim_next(job_id, "delegate-1", &task);
   assert(tid > 0);

   int rc = db1_coord_job_release_task(tid);
   assert(rc == 0);

   /* Should be claimable again */
   db1_coord_task_t task2;
   int tid2 = db1_coord_job_claim_next(job_id, "delegate-2", &task2);
   assert(tid2 == tid);
   assert(strcmp(task2.claimed_by, "delegate-2") == 0);

   teardown(db);
   printf("  PASS: test_release_claim\n");
}

static void test_bounded_preempt_release(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);
   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");

   db1_coord_task_t task;
   int tid = db1_coord_job_claim_next(job_id, "delegate-1", &task);
   assert(tid > 0);
   assert(db1_coord_job_release_task_bounded(tid, 1) == 0);

   db1_coord_task_t tasks[4];
   assert(db1_coord_job_list_tasks(job_id, tasks, 4) == 1);
   assert(strcmp(tasks[0].status, "pending") == 0);
   assert(tasks[0].preempt_requeues == 1);

   assert(db1_coord_job_claim_next(job_id, "delegate-2", &task) == tid);
   assert(db1_coord_job_release_task_bounded(tid, 1) == -1);
   assert(db1_coord_job_fail_task(tid, "cap exhausted") == 0);

   teardown(db);
   printf("  PASS: test_bounded_preempt_release\n");
}

/* --- Test: cancel job --- */

static void test_cancel_job(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   db1_coord_job_add_task(job_id, 0, "[\"src/foo.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/bar.c\"]", "", "", "", "engineer");

   /* Claim one task */
   db1_coord_task_t task;
   db1_coord_job_claim_next(job_id, "delegate-1", &task);

   /* Cancel job */
   int rc = db1_coord_job_cancel(job_id);
   assert(rc == 0);

   /* Verify status */
   db1_coord_job_t job;
   db1_coord_job_get(job_id, &job);
   assert(strcmp(job.status, "cancelled") == 0);

   /* No more claims possible */
   db1_coord_task_t task2;
   int tid2 = db1_coord_job_claim_next(job_id, "delegate-2", &task2);
   assert(tid2 < 0);

   teardown(db);
   printf("  PASS: test_cancel_job\n");
}

/* --- Test: max concurrent enforcement --- */

static void test_max_concurrent(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 2); /* max 2 concurrent */

   db1_coord_job_add_task(job_id, 0, "[\"src/a.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/b.c\"]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[\"src/c.c\"]", "", "", "", "engineer");

   /* Claim two tasks (at max) */
   db1_coord_task_t t1, t2;
   assert(db1_coord_job_claim_next(job_id, "d1", &t1) > 0);
   assert(db1_coord_job_claim_next(job_id, "d2", &t2) > 0);

   /* Job status should show 2 running */
   db1_coord_job_t job;
   db1_coord_job_get(job_id, &job);
   assert(job.running_tasks == 2);

   /* Complete one, then third becomes claimable */
   db1_coord_job_complete_task(t1.id, "done");
   db1_coord_task_t t3;
   assert(db1_coord_job_claim_next(job_id, "d3", &t3) > 0);

   teardown(db);
   printf("  PASS: test_max_concurrent\n");
}

/* --- Test: default parallel value --- */

static void test_default_parallel(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 0); /* 0 should default to 3 */

   db1_coord_job_t job;
   db1_coord_job_get(job_id, &job);
   assert(job.max_concurrent == DB1_COORD_DEFAULT_PAR);

   teardown(db);
   printf("  PASS: test_default_parallel\n");
}

/* --- Test: invalid operations --- */

static void test_invalid_ops(void)
{
   sqlite3 *db = setup();

   /* Invalid IDs */
   assert(db1_coord_job_create(-1, 3) == -1);
   assert(db1_coord_job_add_task(-1, 0, "[]", "", "", "", "engineer") == -1);

   db1_coord_task_t t;
   assert(db1_coord_job_claim_next(-1, "x", &t) == -1);
   assert(db1_coord_job_claim_next(1, NULL, &t) == -1);

   assert(db1_coord_job_complete_task(-1, "x") == -1);
   assert(db1_coord_job_fail_task(-1, "x") == -1);
   assert(db1_coord_job_release_task(-1) == -1);
   assert(db1_coord_job_cancel(-1) == -1);

   db1_coord_job_t j;
   assert(db1_coord_job_get(-1, &j) == -1);
   assert(db1_coord_job_get(999, &j) == -1);

   teardown(db);
   printf("  PASS: test_invalid_ops\n");
}

/* --- Test: file conflict with empty files --- */

static void test_empty_files(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 3);

   /* Tasks with empty file lists should never conflict */
   db1_coord_job_add_task(job_id, 0, "[]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, "[]", "", "", "", "engineer");
   db1_coord_job_add_task(job_id, 0, NULL, "", "", "", "engineer");

   db1_coord_task_t t1, t2, t3;
   assert(db1_coord_job_claim_next(job_id, "d1", &t1) > 0);
   assert(db1_coord_job_claim_next(job_id, "d2", &t2) > 0);
   assert(db1_coord_job_claim_next(job_id, "d3", &t3) > 0);

   teardown(db);
   printf("  PASS: test_empty_files\n");
}

/* --- Test: DB-backed coordinated job economics report --- */

static void test_delegate_economics_report_from_coord_job(void)
{
   sqlite3 *db = setup();

   int plan_id = create_test_plan(db);
   int job_id = db1_coord_job_create(plan_id, 2);
   int t1 = db1_coord_job_add_task(job_id, 0, "[\"src/free_a.c\"]", "", "", "", "engineer");
   int t2 = db1_coord_job_add_task(job_id, 0, "[\"src/free_b.c\"]", "", "", "", "engineer");
   assert(t1 > 0 && t2 > 0);

   db1_coord_task_t task;
   assert(db1_coord_job_claim_next(job_id, "free-a", &task) == t1);
   char h1[512];
   make_handoff(h1, sizeof(h1), "src/free_a.c");
   assert(db1_coord_job_complete_task(t1, h1) == 0);

   assert(db1_coord_job_claim_next(job_id, "free-b", &task) == t2);
   char h2[512];
   make_handoff(h2, sizeof(h2), "src/free_b.c");
   assert(db1_coord_job_complete_task(t2, h2) == 0);

   db1_coord_job_t job;
   assert(db1_coord_job_get(job_id, &job) == 0);
   db1_coord_task_t tasks[DB1_COORD_MAX_TASKS];
   int count = db1_coord_job_list_tasks(job_id, tasks, DB1_COORD_MAX_TASKS);
   assert(count == 2);

   agent_config_t cfg = {0};
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "%s", "free-a");
   cfg.agents[0].cost_tier = 0;
   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "%s", "free-b");
   cfg.agents[1].cost_tier = 0;
   cfg.agent_count = 2;

   delegate_economics_report_t report;
   delegate_economics_build_report(&job, tasks, count, &cfg, &report);
   assert(report.delegate_count == 2);
   assert(report.tier_counts[0] == 2);
   assert(report.valid_handoffs == 2);
   assert(report.delegates_with_focused_tests == 2);
   assert(strcmp(report.verdict, "likely_net_win") == 0);

   teardown(db);
   printf("  PASS: test_delegate_economics_report_from_coord_job\n");
}

int main(void)
{
   printf("test_coord_jobs:\n");
   test_job_create();
   test_add_and_list_tasks();
   test_claim_no_conflict();
   test_file_conflict();
   test_has_file_conflict();
   test_complete_task();
   test_fail_task();
   test_release_claim();
   test_bounded_preempt_release();
   test_cancel_job();
   test_max_concurrent();
   test_default_parallel();
   test_invalid_ops();
   test_empty_files();
   test_delegate_economics_report_from_coord_job();
   printf("All coord_jobs tests passed.\n");
   return 0;
}
